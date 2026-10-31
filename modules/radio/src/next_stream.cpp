#include "next_stream.h"
#include "http_body.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <atomic>
#include <cstring>
#include <cstdio>
#include <new>

namespace radio_core::next_stream {
namespace {
constexpr const char *TAG = "NextStream";
SemaphoreHandle_t guard;
TaskHandle_t worker_handle;
std::atomic<uint32_t> revision{0};
uint32_t wanted_owner = 0;
char wanted_uri[257]{};
Prepared *ready = nullptr;

bool current(uint32_t serial) { return revision.load(std::memory_order_acquire) == serial; }

Prepared *prepare(uint32_t owner, const char *uri, uint32_t serial) {
    auto *memory = heap_caps_calloc(1, sizeof(Prepared), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!memory) return nullptr;
    auto *p = new (memory) Prepared{};
    p->owner = owner;
    snprintf(p->uri, sizeof(p->uri), "%s", uri);
    p->prefix = (uint8_t *)heap_caps_malloc(PREFIX_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    p->pcm = (uint8_t *)heap_caps_malloc(PCM_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p->prefix || !p->pcm) { destroy(p); return nullptr; }
    esp_http_client_config_t cfg{};
    cfg.url = uri;
    cfg.timeout_ms = 1000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.buffer_size = 2048;
    cfg.buffer_size_tx = 1024;
    p->http = esp_http_client_init(&cfg);
    if (!p->http) { destroy(p); return nullptr; }
    esp_http_client_set_header(p->http, "Connection", "close");
    const int64_t deadline = esp_timer_get_time() + 12000000;
    bool opened = false;
    for (int redirect = 0; redirect <= 3 && current(serial); ++redirect) {
        if (esp_http_client_open(p->http, 0) != ESP_OK) break;
        int64_t length = esp_http_client_fetch_headers(p->http);
        int status = esp_http_client_get_status_code(p->http);
        if (status >= 300 && status < 400) {
            if (esp_http_client_set_redirection(p->http) != ESP_OK) break;
            esp_http_client_close(p->http);
            continue;
        }
        if (status != 200 || length <= 0) break;
        p->total = (uint64_t)length;
        opened = true;
        break;
    }
    if (!opened) { destroy(p); return nullptr; }
    const size_t target = p->total < PREFIX_BYTES ? (size_t)p->total : PREFIX_BYTES;
    while (p->size < target && current(serial) && esp_timer_get_time() < deadline) {
        int n = esp_http_client_read(p->http, (char *)p->prefix + p->size,
                                    target - p->size);
        if (n < 0) break;
        p->size += n;
        if (esp_http_client_is_complete_data_received(p->http)) break;
        if (!n) vTaskDelay(1);
    }
    p->complete = esp_http_client_is_complete_data_received(p->http);
    if (!current(serial) || p->size != target ||
        (p->complete && p->size != p->total)) { destroy(p); return nullptr; }

    // Sniff the finite file, not its server's sometimes-generic Content-Type.
    esp_audio_simple_dec_cfg_t dec{};
    if (p->size >= 4 && !memcmp(p->prefix, "fLaC", 4))
        dec.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC;
    else if (p->size >= 12 && !memcmp(p->prefix, "RIFF", 4) && !memcmp(p->prefix + 8, "WAVE", 4))
        dec.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
    else if ((p->size >= 3 && !memcmp(p->prefix, "ID3", 3)) ||
             (p->size >= 2 && p->prefix[0] == 0xff && (p->prefix[1] & 0xe0) == 0xe0))
        dec.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    else { destroy(p); return nullptr; }
    p->type = dec.dec_type;
    if (esp_audio_simple_dec_open(&dec, &p->decoder) != ESP_AUDIO_ERR_OK) {
        destroy(p); return nullptr;
    }
    while (p->consumed < p->size && current(serial)) {
        size_t bytes = p->size - p->consumed;
        if (bytes > 4096) bytes = 4096;
        esp_audio_simple_dec_raw_t raw{};
        raw.buffer = p->prefix + p->consumed;
        raw.len = bytes;
        raw.eos = p->complete && p->consumed + bytes == p->size;
        esp_audio_simple_dec_out_t out{};
        out.buffer = p->pcm;
        out.len = PCM_BYTES;
        auto result = esp_audio_simple_dec_process(p->decoder, &raw, &out);
        if (result != ESP_AUDIO_ERR_OK || raw.consumed > bytes) break;
        p->consumed += raw.consumed;
        if (out.decoded_size) {
            p->pcm_size = out.decoded_size;
            if (esp_audio_simple_dec_get_info(p->decoder, &p->info) == ESP_AUDIO_ERR_OK &&
                p->info.sample_rate && current(serial)) return p;
            break;
        }
        if (!raw.consumed) break;
    }
    destroy(p);
    return nullptr;
}

void worker(void *) {
    uint32_t seen = 0;
    for (;;) {
        uint32_t serial = revision.load(std::memory_order_acquire);
        if (serial == seen) { ulTaskNotifyTake(pdTRUE, portMAX_DELAY); continue; }
        seen = serial;
        char uri[257];
        uint32_t owner;
        xSemaphoreTake(guard, portMAX_DELAY);
        owner = wanted_owner;
        snprintf(uri, sizeof(uri), "%s", wanted_uri);
        Prepared *discard = ready;
        ready = nullptr;
        xSemaphoreGive(guard);
        destroy(discard);
        if (!uri[0]) continue;
        auto *p = prepare(owner, uri, serial);
        if (!p) {
            if (current(serial)) ESP_LOGW(TAG, "not prepared; ordinary transition remains available");
            continue;
        }
        const uint32_t rate = p->info.sample_rate;
        xSemaphoreTake(guard, portMAX_DELAY);
        if (current(serial)) { ready = p; p = nullptr; }
        xSemaphoreGive(guard);
        if (!p) ESP_LOGI(TAG, "ready at %u Hz: %s", (unsigned)rate, uri);
        destroy(p);
    }
}
} // namespace

bool init() {
    if (guard) return true;
    guard = xSemaphoreCreateMutex();
    if (!guard) return false;
    if (xTaskCreatePinnedToCoreWithCaps(worker, "radio_next", 12288, nullptr, 1, &worker_handle, 0,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        vSemaphoreDelete(guard); guard = nullptr; return false;
    }
    return true;
}
void request(uint32_t owner, const char *uri) {
    if (!guard) return;
    bool changed = false;
    xSemaphoreTake(guard, portMAX_DELAY);
    if (owner != wanted_owner || strcmp(uri, wanted_uri)) {
        wanted_owner = owner;
        snprintf(wanted_uri, sizeof(wanted_uri), "%s", uri);
        revision.fetch_add(1, std::memory_order_release);
        changed = true;
    }
    xSemaphoreGive(guard);
    if (changed) xTaskNotifyGive(worker_handle);
}
Prepared *take(uint32_t owner, uint32_t source_rate) {
    if (!guard || xSemaphoreTake(guard, 0) != pdTRUE) return nullptr;
    Prepared *p = nullptr;
    if (ready && ready->owner == owner && owner == wanted_owner &&
        !strcmp(ready->uri, wanted_uri) && ready->info.sample_rate == source_rate) {
        p = ready; ready = nullptr;
    }
    xSemaphoreGive(guard);
    return p;
}
int read(Prepared *p, uint8_t *out, size_t size) {
    if (esp_http_client_is_complete_data_received(p->http)) return HttpBody::END;
    int n = esp_http_client_read(p->http, (char *)out, size);
    if (n > 0) return n;
    if (esp_http_client_is_complete_data_received(p->http)) return HttpBody::END;
    return n == 0 || n == -ESP_ERR_HTTP_EAGAIN ? 0 : HttpBody::ERROR;
}
void destroy(Prepared *p) {
    if (!p) return;
    if (p->decoder) esp_audio_simple_dec_close(p->decoder);
    if (p->http) esp_http_client_cleanup(p->http);
    heap_caps_free(p->prefix);
    heap_caps_free(p->pcm);
    p->~Prepared();
    heap_caps_free(p);
}
} // namespace radio_core::next_stream
