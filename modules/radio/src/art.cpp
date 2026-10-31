/**
 * @file art.cpp
 * @brief The radio's picture: the playing song's cover, else the station's logo. See
 *        radio_art_task.h.
 *
 * The song's cover: the station's ICY title goes to Deezer's search over plain HTTP, and the
 * first hit that is the same song (radio_art::best) has its 500 px JPEG fetched over plain
 * HTTP too. The station's logo: its favicon from Radio Browser — carried by a catalog row,
 * or asked for by the station's uuid — fetched once per station.
 *
 * What the tile shows, in order: the song's cover; the station's logo; nothing. Last song's
 * album beside this song's title is worse than the station's logo, and the logo of the last
 * station is worse than none. Only this task writes the tile, so the order holds.
 *
 * TLS: never for a cover, and for a logo only when the station's own stream is plain HTTP —
 * the panel holds one TLS session at a time, and while the radio plays it is the station's.
 * Most logos are HTTPS-only (10 of 13 among the top Ukrainian stations, 05.10.2026), so a
 * station streaming over HTTPS mostly shows none.
 */

#include "radio_art_task.h"
#include "radio_art.h"
#include "radio_catalog.h"
#include "radio_http.h"
#include "cover.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>

namespace {

const char* TAG = "Radio";   // the module's tag: on the INFO allowlist
namespace rc = radio_catalog;

constexpr const char* SEARCH_HOST  = "api.deezer.com";
constexpr const char* CATALOG_HOST = "all.api.radio-browser.info";
constexpr size_t SEARCH_MAX = 48 * 1024;    // ten hits are ~22 KB (05.10.2026)
constexpr size_t IMAGE_MAX  = 256 * 1024;   // a 500 px cover is tens of KB
constexpr size_t LOGO_MAX   = 128 * 1024;   // favicons are small; a few are full-size art
constexpr int    TITLE_LEN  = 256;
constexpr int    HITS       = 10;
constexpr auto   TILE       = player_core::CoverSource::Radio;

struct TitleAsk {
    char     title[TITLE_LEN];
    uint32_t serial;
};

struct StationAsk {
    char     uuid[rc::UUID_LEN];
    char     favicon[rc::URL_LEN];
    bool     tls_free;     ///< the station streams over plain HTTP: a logo may use TLS
    uint32_t serial;
};

enum class Shown : uint8_t { NONE, LOGO, TRACK };

struct ArtTask {
    QueueHandle_t         titles   = nullptr;
    QueueHandle_t         stations = nullptr;
    char*                 search   = nullptr;   // PSRAM
    uint8_t*              image    = nullptr;   // PSRAM
    uint8_t*              logo     = nullptr;   // PSRAM: the current station's logo
    size_t                logo_len = 0;
    char                  logo_uuid[rc::UUID_LEN]   = "";   // whose logo it is
    char                  logo_favicon[rc::URL_LEN] = "";
    radio_art::Hit*       hits     = nullptr;   // PSRAM
    std::atomic<uint32_t> latest{0};            // the newest title's serial
    std::atomic<uint32_t> station{0};           // the newest station's serial
    Shown                 shown = Shown::NONE;  // task only
    bool                  started = false;
    bool                  failed  = false;
};
ArtTask a;

bool current_title(uint32_t serial) { return a.latest.load(std::memory_order_acquire) == serial; }

/// No song cover: the station's logo if there is one, else nothing. A logo already on the
/// tile is left there: decoded again on every title with no cover, a PNG one cost the
/// software decoder its whole run each time, for the picture that was already up.
void fall_back() {
    if (a.logo_len && a.shown == Shown::LOGO) return;
    if (a.logo_len) {
        const uint32_t gen = player_core::cover_view(TILE).gen;
        if (player_core::cover_set_jpeg(a.logo, a.logo_len, TILE, gen)) {
            a.shown = Shown::LOGO;
            return;
        }
    }
    player_core::cover_clear(TILE);
    a.shown = Shown::NONE;
}

void serve_title(const TitleAsk& rq) {
    const int64_t t0 = esp_timer_get_time();
    if (!rq.title[0]) {                       // between songs, or covers off
        fall_back();
        return;
    }
    radio_art::Title want;
    if (!radio_art::split(rq.title, want)) {
        ESP_LOGI(TAG, "art: \"%s\" is not \"artist - title\"", rq.title);
        fall_back();
        return;
    }
    char path[512];
    char why[64] = "";
    if (!radio_art::search_path(want, path, sizeof(path))) { fall_back(); return; }
    const int n = radio_http::get(SEARCH_HOST, 80, path, a.search, SEARCH_MAX,
                                  radio_http::user_agent(), why, sizeof(why));
    if (n < 0) {
        ESP_LOGW(TAG, "art: search: %s", why);
        fall_back();
        return;
    }
    if (!current_title(rq.serial)) return;    // a newer title is waiting: spend nothing more here

    const size_t need = rc::scratch_bytes(a.search, (size_t)n);
    void* scratch = need ? heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : nullptr;
    const int hits = scratch ? radio_art::parse_search(a.search, (size_t)n, scratch, need, a.hits, HITS)
                             : -1;
    heap_caps_free(scratch);
    const int pick = hits > 0 ? radio_art::best(want, a.hits, hits) : -1;
    if (pick < 0) {
        ESP_LOGI(TAG, "art: no cover for \"%s\" - \"%s\" (%d hits)", want.artist, want.title, hits);
        fall_back();
        return;
    }
    const int len = radio_http::get_url(a.hits[pick].cover, (char*)a.image, IMAGE_MAX,
                                        radio_http::user_agent(), false, why, sizeof(why));
    if (len <= 0) {
        ESP_LOGW(TAG, "art: image: %s", why);
        fall_back();
        return;
    }
    if (!current_title(rq.serial)) return;
    const uint32_t gen = player_core::cover_view(TILE).gen;
    const bool shown = player_core::cover_set_jpeg(a.image, (size_t)len, TILE, gen);
    ESP_LOGI(TAG, "art: \"%s\" - \"%s\" is %s / %s: %d B, %s, %u ms", want.artist, want.title,
             a.hits[pick].artist, a.hits[pick].title, len, shown ? "shown" : "not shown",
             (unsigned)((esp_timer_get_time() - t0) / 1000));
    if (shown) a.shown = Shown::TRACK;
    else       fall_back();
}

/// The favicon URL for a station known only by its uuid.
bool favicon_by_uuid(const char* uuid, char* out, size_t out_sz) {
    char path[96];
    snprintf(path, sizeof(path), "/json/stations/byuuid?uuids=%s", uuid);
    char why[64] = "";
    const int n = radio_http::get(CATALOG_HOST, 80, path, a.search, SEARCH_MAX,
                                  radio_http::user_agent(), why, sizeof(why));
    if (n < 0) {
        ESP_LOGW(TAG, "art: station %s: %s", uuid, why);
        return false;
    }
    const size_t need = rc::scratch_bytes(a.search, (size_t)n);
    void* scratch = need ? heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : nullptr;
    const bool ok = scratch && rc::parse_favicon(a.search, (size_t)n, scratch, need, out, out_sz);
    heap_caps_free(scratch);
    return ok;
}

void serve_station(const StationAsk& rq) {
    // The same station again (Play after Stop): its logo is already here.
    if (a.logo_len && !strcmp(rq.uuid, a.logo_uuid) && !strcmp(rq.favicon, a.logo_favicon)) {
        if (a.shown != Shown::TRACK) fall_back();
        return;
    }
    a.logo_len = 0;                           // the last station's logo is not this one's
    snprintf(a.logo_uuid, sizeof(a.logo_uuid), "%s", rq.uuid);
    snprintf(a.logo_favicon, sizeof(a.logo_favicon), "%s", rq.favicon);
    if (a.shown == Shown::LOGO) fall_back();  // … and must not stay on the tile meanwhile
    char favicon[rc::URL_LEN];
    snprintf(favicon, sizeof(favicon), "%s", rq.favicon);
    if (!favicon[0] && rq.uuid[0] && !favicon_by_uuid(rq.uuid, favicon, sizeof(favicon))) return;
    if (!favicon[0]) return;

    // Plain HTTP first, even for an https:// logo: some hosts still serve it there.
    char why[64] = "";
    char plain[rc::URL_LEN];
    snprintf(plain, sizeof(plain), "%s", favicon);
    radio_art::plain_http(plain);
    int len = radio_http::get_url(plain, (char*)a.logo, LOGO_MAX, radio_http::user_agent(),
                                  false, why, sizeof(why));
    if (len <= 0 && strcmp(plain, favicon) != 0) {
        len = radio_http::get_url(favicon, (char*)a.logo, LOGO_MAX, radio_http::user_agent(),
                                  rq.tls_free, why, sizeof(why));
    }
    if (a.station.load(std::memory_order_acquire) != rq.serial) return;   // a newer station
    if (len <= 0) {
        ESP_LOGI(TAG, "art: no logo from %s: %s", favicon, why);
        return;
    }
    const rc::Image kind = rc::image_kind(a.logo, (size_t)len);
    if (kind == rc::Image::NONE) {
        ESP_LOGI(TAG, "art: logo %s is neither JPEG nor PNG", favicon);
        return;
    }
    a.logo_len = (size_t)len;
    ESP_LOGI(TAG, "art: logo %d B (%s) from %s", len, kind == rc::Image::JPEG ? "JPEG" : "PNG",
             favicon);
    if (a.shown != Shown::TRACK) fall_back();   // shows it, unless a song's cover is up
}

void art_task(void*) {
    TitleAsk   t;
    StationAsk st;
    for (;;) {
        if (xQueueReceive(a.stations, &st, 0) == pdTRUE) serve_station(st);
        if (xQueueReceive(a.titles, &t, pdMS_TO_TICKS(200)) == pdTRUE && current_title(t.serial)) {
            serve_title(t);
        }
    }
}

/// Queues, buffers and task: PSRAM, like the cover workers of AirPlay and Bluetooth; the
/// task never writes flash, so a PSRAM stack is allowed.
///
/// 16 KB of stack since a PNG logo was decoded in this task by stb_image (cover_soft.c),
/// whose inflate keeps its two Huffman tables on the stack (~4 KB) and builds the code
/// lengths' table deeper down (~2.5 KB more). With 8 KB the first PNG logo on the bench —
/// Hit FM's 120 px favicon, 05.10.2026 — ended in a stack protection fault and a reboot.
/// That decode now runs on the cover component's own task (SOFT_STACK in
/// components/cover/src/cover.cpp), so this is headroom rather than a requirement.
constexpr uint32_t ART_STACK = 16 * 1024;

bool start() {
    a.titles   = xQueueCreate(1, sizeof(TitleAsk));
    a.stations = xQueueCreate(1, sizeof(StationAsk));
    a.search   = (char*)heap_caps_malloc(SEARCH_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    a.image    = (uint8_t*)heap_caps_malloc(IMAGE_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    a.logo     = (uint8_t*)heap_caps_malloc(LOGO_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    void* h    = heap_caps_calloc(HITS, sizeof(radio_art::Hit), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (h) {
        a.hits = static_cast<radio_art::Hit*>(h);
        for (int i = 0; i < HITS; i++) new (&a.hits[i]) radio_art::Hit();
    }
    if (!a.titles || !a.stations || !a.search || !a.image || !a.logo || !a.hits) return false;
    // Idle priority, as the card player's artwork worker has (player_module.cpp, on_init): a
    // PNG or progressive picture goes to the software decoder, which takes this task's
    // priority and never blocks. At 2 it ran above the radio's decoder and reader, LVGL, main
    // and IDLE0 on core 0, and a logo of about 700 px was a dropout and a frozen screen.
    return xTaskCreatePinnedToCoreWithCaps(art_task, "radio_art", ART_STACK, nullptr,
                                           tskIDLE_PRIORITY, nullptr, 0,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS;
}

bool ensure_started() {
    if (!a.started) {
        a.started = true;
        a.failed  = !start();
        if (a.failed) ESP_LOGE(TAG, "art: could not start (memory)");
    }
    return !a.failed;
}

}  // namespace

namespace radio_art_task {

void update(const char* title, bool enabled) {
    static char last[TITLE_LEN] = "";
    const char* want = (enabled && title) ? title : "";
    if (!strcmp(want, last)) return;
    snprintf(last, sizeof(last), "%s", want);
    if (!ensure_started()) return;
    TitleAsk rq;
    snprintf(rq.title, sizeof(rq.title), "%s", want);
    rq.serial = a.latest.fetch_add(1, std::memory_order_acq_rel) + 1;
    xQueueOverwrite(a.titles, &rq);
}

void station(const char* uuid, const char* favicon, bool tls_free) {
    if (!ensure_started()) return;
    StationAsk rq;
    snprintf(rq.uuid, sizeof(rq.uuid), "%s", uuid ? uuid : "");
    snprintf(rq.favicon, sizeof(rq.favicon), "%s", favicon ? favicon : "");
    rq.tls_free = tls_free;
    rq.serial = a.station.fetch_add(1, std::memory_order_acq_rel) + 1;
    xQueueOverwrite(a.stations, &rq);
}

}  // namespace radio_art_task
