#include "bt_cover.h"
#include "cover.h"
#include "sat_link/cover_assembly.h"
#include "modesp/liveness.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <cstdlib>

namespace bt_cover {
namespace {
using player_core::CoverSource;
constexpr auto SOURCE = CoverSource::Bluetooth;
struct Job { uint8_t* jpg; size_t len; uint32_t revision; };
QueueHandle_t queue;
sat::CoverAssembly assembly;
uint8_t* buffer;
size_t capacity;
uint32_t last_ms;
/// 1.7 retry state. `want_retry` is one pending request; `chain` is how many repeats this
/// image has already cost, so a link that loses frames steadily cannot turn into a stream of
/// OBEX traffic; `awaiting` distinguishes the RESET that answers our own asking from the one
/// a new track brings, which is the only way to know whether the chain should restart.
uint32_t retry_nonce, retry_gen, chain;
bool want_retry, awaiting;
void release() { free(buffer); buffer=nullptr; capacity=0; }
/// Records that this pair is worth asking for again, if there is any budget left. Called
/// from every path that sets assembly.failed, because every one of them ends the same way:
/// the image is discarded and nothing is on the screen.
void want() {
    if (!assembly.nonce || !assembly.generation || chain>=COVER_RETRY_MAX) return;
    retry_nonce=assembly.nonce; retry_gen=assembly.generation; want_retry=true;
}
void publish() {
    Job job{buffer,assembly.total,player_core::cover_view(SOURCE).gen};
    buffer=nullptr; capacity=0;
    // An image that arrived is the evidence the link is fine, so the next one starts with a
    // full budget however many repeats this one took. `awaiting` goes too: a retry the
    // satellite never served would otherwise keep the next track from restarting the chain.
    chain=0; want_retry=false; awaiting=false;
    if (xQueueSend(queue,&job,0)!=pdTRUE) free(job.jpg);
}
/// Bumped once per turn of the worker. The wait below times out rather than blocking for
/// ever so that this is a heartbeat: a JPEG decode that never returns is the fault worth
/// catching, and with portMAX_DELAY an idle worker would have looked identical to it.
modesp::liveness::Counter beat{0};

void worker(void*) {
    Job job;
    for (;;) {
        modesp::liveness::tick(beat);
        if (xQueueReceive(queue,&job,pdMS_TO_TICKS(1000))!=pdTRUE) continue;
        if (player_core::cover_view(SOURCE).gen==job.revision) {
            const bool ok=player_core::cover_set_jpeg(job.jpg,job.len,SOURCE,job.revision);
            ESP_LOGI("BtCover", "JPEG %u bytes: %s", (unsigned)job.len, ok?"display ready":"discarded");
        }
        free(job.jpg);
    }
}
}
bool init() {
    if (queue) return true;
    queue=xQueueCreate(1,sizeof(Job));
    if (!queue) return false;
    // A PSRAM stack, as AirPlay's cover worker has for the same decode (it writes no flash, so
    // it never has to run while the flash cache is off): 6 KB off the internal RAM DMA needs.
    if (xTaskCreatePinnedToCoreWithCaps(worker,"bt_cover",6144,nullptr,tskIDLE_PRIORITY,nullptr,0,
                                        MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)!=pdPASS) {
        vQueueDelete(queue); queue=nullptr; return false;
    }
    // 30 s against a 1 Hz beat, as the other cover workers have: at idle priority a
    // software decode waits for everything else on core 0.
    modesp::liveness::watch("bt_cover",&beat,30000);
    return true;
}
void clear() {
    release(); assembly.reset(); want_retry=false;
    player_core::cover_clear(SOURCE);
    Job job;
    if (queue && xQueueReceive(queue,&job,0)==pdTRUE) free(job.jpg);
}
void reset(const uint8_t* p,size_t len) {
    if (len!=8 || !cover_u32(p) || !cover_u32(p+4)) return;
    const auto n=cover_u32(p), g=cover_u32(p+4);
    if (assembly.nonce==n && assembly.generation==g) return;
    // A RESET we did not ask for means the phone moved on: the budget belongs to an image,
    // not to the session, so it restarts. One we did ask for carries the chain forward.
    // A track change landing between our asking and this frame costs the new track its
    // budget for one image, which the next success gives back.
    if (!awaiting) chain=0;
    awaiting=false;
    clear(); assembly.reset(n,g);   // clear() also drops a request for the pair just gone
    ESP_LOGI("BtCover","track %lu/%lu",(unsigned long)n,(unsigned long)g);
}
void data(const uint8_t* p,size_t len,uint32_t now,bool allow_stream) {
    if (!queue || len<16 || !assembly.nonce || assembly.failed || assembly.complete ||
        cover_u32(p)!=assembly.nonce || cover_u32(p+4)!=assembly.generation) return;
    if (!buffer) {
        const uint32_t size=(!cover_u32(p+8) && allow_stream)?COVER_BYTES_MAX:cover_u32(p+8);
        if (!size || size>COVER_BYTES_MAX || cover_u32(p+12)!=0) { assembly.failed=true; want(); return; }
        buffer=(uint8_t*)heap_caps_malloc(size,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        capacity=buffer?size:0;
    }
    if (!assembly.append(p,len,buffer,capacity,allow_stream)) {
        if (assembly.failed) {
            ESP_LOGW("BtCover","incomplete image: received %lu, next offset %lu, bytes %u",
                     (unsigned long)assembly.used,(unsigned long)cover_u32(p+12),(unsigned)(len-16));
            release(); want();
        }
        return;
    }
    last_ms=now;
    if (assembly.complete) {
        publish();
    }
}
void end(const uint8_t* p,size_t len) {
    if (!queue) return;
    // data() or tick() may have failed this pair already and asked for it then: the END the
    // satellite had queued behind the gap must not ask a second time, or the retry that
    // answers the first one starts with the image's budget spent.
    const bool was_failed=assembly.failed;
    if (assembly.finish(p,len)) publish();
    else if (assembly.failed) {
        if (len==16) ESP_LOGW("BtCover","stream discarded: size %lu/%lu, CRC %08lx/%08lx",
                             (unsigned long)assembly.used,(unsigned long)cover_u32(p+8),
                             (unsigned long)assembly.crc,(unsigned long)cover_u32(p+12));
        release(); if (!was_failed) want();
    }
}
void tick(uint32_t now) {
    if (buffer && now-last_ms>5000) {
        ESP_LOGW("BtCover","transfer timed out");
        release(); assembly.failed=true; want();
    }
}
bool take_retry(uint32_t& nonce,uint32_t& generation) {
    if (!want_retry) return false;
    want_retry=false;
    nonce=retry_nonce; generation=retry_gen;
    chain++; awaiting=true;
    return true;
}
}
