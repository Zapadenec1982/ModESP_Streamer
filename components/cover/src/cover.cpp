/**
 * @file cover.cpp
 * @brief JPEG in, a small RGB565 tile out — see cover.h.
 *
 * Both halves of the work are hardware on this chip. The JPEG engine decodes the whole
 * image, because it has no scaler of its own, and the PPA then scales that down to the
 * card. The full-size buffer is borrowed for the length of one call and given straight
 * back: it can be several megabytes, and holding it between tracks would be holding it
 * for the ninety-nine percent of the time nothing is being decoded.
 *
 * Non-square artwork is centre-cropped rather than squashed. A cover is a photograph of a
 * square object; stretching it to fit is the one result nobody wants.
 */

#include "cover.h"
#include "cover_guard.h"
#include "cover_thumb.h"

#include "driver/jpeg_decode.h"
#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_err.h"

#include <atomic>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <cstring>

// The software fallback, in its own translation unit because stb_image is C.
extern "C" {
int            cover_soft_info(const unsigned char* jpg, int len, int* w, int* h);
unsigned char* cover_soft_decode(const unsigned char* jpg, int len, int* w, int* h, int comp);
void           cover_soft_free(unsigned char* px);
const char*    cover_soft_reason(void);
}

namespace player_core {
namespace {

const char* TAG = "Cover";

/// Anything larger is not artwork, it is a photograph somebody dropped in the folder.
///
/// Two limits, because the two decoders cost wildly different amounts for the same picture.
/// The hardware engine writes RGB565 straight out: W x H x 2, and 4096 a side is 32 MB of
/// PSRAM asked through an allocator that fails cleanly. stb_image holds the finished RGB
/// picture AND a w2 x h2 byte buffer per component AND, when the file is progressive, a
/// w2 x h2 x sizeof(short) coefficient buffer per component all at once (stb_image.h:3337
/// and :3346) - about 7.5 times the pixel count at 4:2:0, so 2048 a side wants 31 MB and
/// 2560 49 MB. What the running firmware has free is less than either: about 23 MB in one
/// block, measured on the bench. So MAX_SIDE is only the outer cap; the limit that decides
/// is decode_soft()'s check of stb's own peak against the largest free block at the time
/// (cover_guard.h). At the bench's figure that refuses progressive 4:2:0 from about 1670 px.
///
/// A size limit bounds memory, not time. stb walks the whole frame once per scan and a file
/// can carry thousands of scans, so the time is bounded separately, by the scan count
/// (cover_guard.h).
constexpr uint32_t MAX_SIDE      = 2048;   ///< the software path
constexpr uint32_t MAX_SIDE_HW   = 4096;   ///< the hardware path

/// Which SOF marker the file carries, which decides whether the hardware engine can have it
/// at all: the P4's JPEG decoder is baseline only.
///
/// Worth naming rather than inferring. A 2560x2560 cover that never displays is a completely
/// different problem depending on the answer - baseline means raise the hardware limit and
/// it works, progressive means no decoder on this chip can do it.
enum class Sof { Baseline, Progressive, Other };
/// `frame`, when given, also gets what the header says about stb_image's memory: the
/// components and their sampling factors.
Sof sof_kind(const uint8_t* p, size_t len, cover_guard::Frame* frame = nullptr) {
    if (len < 4 || p[0] != 0xFF || p[1] != 0xD8) return Sof::Other;
    size_t i = 2;
    while (i + 3 < len) {
        if (p[i] != 0xFF) { ++i; continue; }          // resync over fill bytes
        const uint8_t m = p[i + 1];
        if (m == 0xFF) { ++i; continue; }
        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { i += 2; continue; }
        if (m == 0xDA || m == 0xD9) return Sof::Other;   // image data began, no SOF seen
        if (m == 0xC0 || m == 0xC1 || m == 0xC2) {
            if (frame) *frame = cover_guard::frame_from_sof(p + i, len - i);
            return m == 0xC2 ? Sof::Progressive : Sof::Baseline;
        }
        const size_t seg = ((size_t)p[i + 2] << 8) | p[i + 3];
        if (seg < 2) return Sof::Other;
        i += 2 + seg;
    }
    return Sof::Other;
}
const char* kind_name(Sof k, bool png) {
    return png ? "PNG"
         : k == Sof::Baseline ? "baseline JPEG"
         : k == Sof::Progressive ? "progressive JPEG" : "JPEG of unknown kind";
}

bool is_png(const uint8_t* p, size_t len) {
    return len >= 8 && !memcmp(p, "\x89PNG\r\n\x1a\n", 8);
}

/// stb_image's reason for the last failure, or a stand-in when it has none.
///
/// stbi__g_failure_reason is a plain global left NULL until something actually fails, and
/// ESP_LOGx hands %s to vfprintf without looking at it. Every caller here can be reached on
/// a path where stb did not fail, so no caller may pass the raw pointer.
const char* soft_reason() {
    const char* r = cover_soft_reason();
    return (r && *r) ? r : "no reason recorded";
}
/// The PPA scales in sixteenths, so every factor is quantised to this grid.
constexpr float    SCALE_STEP = 1.0f / 16.0f;
// Match jpeg_parse_sof_marker() in the pinned IDF: MCU = Y sampling factor * 8.
// Overallocating is harmless, but a wrong row stride corrupts PPA's input image.
bool jpeg_layout(const jpeg_decode_picture_info_t& info, uint32_t& w, uint32_t& h) {
    uint32_t mx = 8, my = 8;
    switch (info.sample_method) {
    case JPEG_DOWN_SAMPLING_YUV444: break;
    case JPEG_DOWN_SAMPLING_YUV422: mx = 16; break;
    case JPEG_DOWN_SAMPLING_YUV420: mx = my = 16; break;
    default: return false; // grayscale needs the software RGB conversion
    }
    w = (info.width + mx - 1) / mx * mx;
    h = (info.height + my - 1) / my * my;
    return true;
}

struct Store {
    uint16_t* slot[2] = {nullptr, nullptr};
    // Which slot the UI should read, and how many times it has changed. `cur` is -1 until
    // something has been published; `gen` is what the UI compares against.
    std::atomic<int32_t>  cur{-1};
    std::atomic<uint32_t> gen{0};
    int      w[2] = {0, 0};
    int      h[2] = {0, 0};
    uint32_t rgb[2] = {0, 0};        ///< each slot's colour (cover_tone)
    int last_slot = 1;
    bool     tried = false;          ///< hardware set-up attempted; do not retry every track
};

Store stores[COVER_SOURCES];
/// One JPEG engine and one PPA client for every source, not one per Store. The DecodeLock
/// already serialises their use, and IDF 5.5.5 hands a decoder error to one engine only: each
/// engine adds a handler to the codec's one interrupt list, the first handler run reads and
/// clears the status, and every other engine is sent a zero event (jpeg_common.c jpeg_isr,
/// jpeg_decode.c jpeg_decoder_isr_handle_default). An engine that was not the newest waited out
/// its whole 1 s timeout on a corrupt cover, with the lock held.
jpeg_decoder_handle_t s_jpeg = nullptr;
ppa_client_handle_t   s_ppa  = nullptr;
bool                  s_engines_tried = false;
portMUX_TYPE publication_mux = portMUX_INITIALIZER_UNLOCKED;
// Serializes decoder/PPA use and slot writers across file and Bluetooth workers.
SemaphoreHandle_t decoder_mutex() {
    static StaticSemaphore_t storage;
    static SemaphoreHandle_t mutex = xSemaphoreCreateMutexStatic(&storage);
    return mutex;
}
struct DecodeLock {
    DecodeLock() { xSemaphoreTake(decoder_mutex(), portMAX_DELAY); }
    ~DecodeLock() { xSemaphoreGive(decoder_mutex()); }
};

/// One slot, cache-line aligned because the PPA writes it by DMA.
uint16_t* alloc_slot() {
    const size_t bytes = (size_t)COVER_PX * COVER_PX * sizeof(uint16_t);
    return (uint16_t*)heap_caps_aligned_alloc(64, (bytes + 63) / 64 * 64,
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

/// Brings up the two engines, for every source, the first time anything asks. Once: if the
/// JPEG engine cannot be had, asking again at every track would log the same failure for every
/// track. Called under the DecodeLock.
void ensure_engines() {
    if (s_engines_tried) return;
    s_engines_tried = true;
    jpeg_decode_engine_cfg_t eng = {};
    eng.timeout_ms = 1000;
    if (jpeg_new_decoder_engine(&eng, &s_jpeg) != ESP_OK) {
        s_jpeg = nullptr;
        ESP_LOGW(TAG, "no JPEG engine — artwork disabled");
    } else {
        ppa_client_config_t pc = {};
        pc.oper_type = PPA_OPERATION_SRM;
        pc.max_pending_trans_num = 1;
        if (ppa_register_client(&pc, &s_ppa) != ESP_OK) {
            s_ppa = nullptr;
            ESP_LOGW(TAG, "no PPA client — artwork disabled");
        }
    }
}

/// The engines (ensure_engines) and this source's two slots, once. Called under the DecodeLock.
bool ensure_ready(Store& g) {
    ensure_engines();
    if (g.tried) return s_jpeg && s_ppa && g.slot[0] && g.slot[1];
    g.tried = true;
    if (!s_jpeg || !s_ppa) return false;

    g.slot[0] = alloc_slot();
    g.slot[1] = alloc_slot();
    if (!g.slot[0] || !g.slot[1]) {
        ESP_LOGW(TAG, "no room for the artwork tiles");
        return false;
    }
    return true;
}

/// The smallest sixteenth that still covers the card: rounding DOWN would leave a border
/// where the image should reach the edge, and cropping a few source pixels is free.
float scale_for(uint32_t src_side) {
    float s = (float)COVER_PX / (float)src_side;
    s = (float)((int)(s / SCALE_STEP) + 1) * SCALE_STEP;
    if (s > 16.0f) s = 16.0f;
    return s;
}

/// Average a rectangle of the source down to one pixel, for the software path.
///
/// A box average rather than a nearest-neighbour pick: reducing a thousand pixels to a
/// hundred and fifty throws away nine tenths of them either way, and averaging keeps the
/// colour while picking keeps whichever pixel happened to land on the grid. On a cover
/// with fine detail the difference is between a photograph and a field of speckles.
uint16_t box_average(const uint8_t* rgb, int pic_w, int x0, int y0, int bw, int bh) {
    uint32_t r = 0, g = 0, b = 0;
    for (int y = 0; y < bh; y++) {
        const uint8_t* row = rgb + ((size_t)(y0 + y) * pic_w + x0) * 3;
        for (int x = 0; x < bw; x++) {
            r += row[0]; g += row[1]; b += row[2];
            row += 3;
        }
    }
    const uint32_t n = (uint32_t)bw * bh;
    r /= n; g /= n; b /= n;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/// Publishes whichever slot was just filled. Its colour is worked out first, outside the
/// spinlock: 4 900 pixels are not something to hold one for.
bool publish(Store& g, int slot, uint32_t expected) {
    const uint32_t rgb = cover_tone(g.slot[slot], COVER_PX, COVER_PX);
    portENTER_CRITICAL(&publication_mux);
    if (expected != UINT32_MAX && g.gen.load() != expected) {
        portEXIT_CRITICAL(&publication_mux); return false;
    }
    g.w[slot] = COVER_PX;
    g.h[slot] = COVER_PX;
    g.rgb[slot] = rgb;
    g.last_slot = slot;
    g.cur.store(slot, std::memory_order_release);
    g.gen.fetch_add(1, std::memory_order_release);
    portEXIT_CRITICAL(&publication_mux);
    return true;
}

int free_slot(Store& g) {
    return g.last_slot == 0 ? 1 : 0;
}

/// Whether stb_image may be handed this picture: its size, the work its scans would cost and the
/// memory the decode would hold, all judged from the file before anything is spent. `w`, `h` and
/// `png` come back for the decode; a refusal is said at WARN.
bool soft_admit(const uint8_t* jpg, size_t len, int& w, int& h, bool& png) {
    // Ask the size before spending anything on it. The size decides the memory the decode will
    // ask for (see MAX_SIDE); it says nothing about the time, which is the scan count's, below.
    if (!cover_soft_info(jpg, (int)len, &w, &h)) {
        ESP_LOGW(TAG, "not a JPEG at all: %s", soft_reason());
        return false;
    }
    png = is_png(jpg, len);
    cover_guard::Frame frame;
    const Sof kind = png ? Sof::Other : sof_kind(jpg, len, &frame);
    // What stb will hold at once for this picture. Byte 24 is the PNG header's bit depth.
    const uint64_t peak = png ? cover_guard::png_soft_peak(w, h, len > 24 && jpg[24] == 16)
                              : cover_guard::jpeg_soft_peak(frame, w, h);
    // Split from the test above, and not for tidiness. A JPEG this decoder can read but will
    // not spend memory on is not a broken JPEG: stb never failed, so it recorded no reason,
    // and the single test printed that absent reason through %s. A panel died in vfprintf
    // walking it, on a 488 KB cover embedded in a track on the card. The message was wrong
    // as well as fatal - it said "not a JPEG at all" about a good one.
    if (w <= 0 || h <= 0 || w > (int)MAX_SIDE || h > (int)MAX_SIDE) {
        // The kind is the actionable half of this message. Baseline here means the hardware
        // engine refused it for some other reason and the fix is on that side; progressive
        // means no decoder on this chip can take a picture this size, and the answer is a
        // smaller cover.jpg beside the album rather than anything in this file.
        ESP_LOGW(TAG, "a %s at %dx%d: the software decoder wants about %u MB and the "
                      "limit is %u a side",
                 kind_name(kind, png), w, h, (unsigned)(peak >> 20), (unsigned)MAX_SIDE);
        return false;
    }
    // The time, bounded before it is spent: stb cannot be stopped once it starts, and a file
    // whose scans each re-walk the frame keeps it busy on core 0 until the task watchdog panics.
    // The charge is per scan, over the components that scan names and the frame's MCU padding,
    // with the sequential-frame rule - the scan count alone misses interleaving and the IDCT.
    if (!png) {
        const size_t scans = cover_guard::jpeg_scan_upper_bound(jpg, len);
        if (!cover_guard::soft_scan_work_ok(jpg, len, frame, w, h)) {
            ESP_LOGW(TAG, "a %s at %dx%d with up to %u scans: over the software decoder's "
                          "budget (%u scans, %lu block visits)",
                     kind_name(kind, png), w, h, (unsigned)scans,
                     (unsigned)cover_guard::MAX_SCANS,
                     (unsigned long)cover_guard::SCAN_BLOCK_BUDGET);
            return false;
        }
    }
    // And the memory, against what is free now rather than what the chip has. stb asks for
    // its output only after the last scan, so a picture that does not fit used to cost the
    // whole decode - seconds, at the sizes where this bites - and then fail on "outofmem".
    //
    // Asked by taking the block and giving it straight back: TLSF finds one or refuses in
    // constant time. heap_caps_get_largest_free_block() answers the same question by walking
    // every block of the 32 MB pool with interrupts off on this core, 2.6-4.3 ms - longer than
    // the satellite UART's FIFO lasts at 460800 baud, so a decode during a Bluetooth cover
    // lost a frame of it (main.cpp took the same call out of its once-a-second report).
    const uint64_t need = cover_guard::soft_memory_need(peak);
    void* room = need <= SIZE_MAX
               ? heap_caps_malloc((size_t)need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : nullptr;
    if (!room) {
        ESP_LOGW(TAG, "a %s at %dx%d: the software decoder would hold about %u KB at once, and "
                      "PSRAM has no free block of %u KB for that and the reserve",
                 kind_name(kind, png), w, h, (unsigned)(peak >> 10), (unsigned)(need >> 10));
        return false;
    }
    heap_caps_free(room);
    ESP_LOGI(TAG, "software path: %dx%d, PSRAM free %u", w, h,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return true;
}

/// The whole software path: decode with stb_image, then average it down into the tile.
/// Runs on the decoder's own task, never on the caller's stack - see set_jpeg_soft().
bool decode_soft(Store& g, const uint8_t* jpg, size_t len, uint32_t expected) {
    int w = 0, h = 0;
    bool png = false;
    if (!soft_admit(jpg, len, w, h, png)) return false;
    uint8_t* rgb = cover_soft_decode(jpg, (int)len, &w, &h, 3);
    if (!rgb) {
        ESP_LOGW(TAG, "software decode failed: %s", soft_reason());
        return false;
    }
    if (w <= 0 || h <= 0 || w > (int)MAX_SIDE || h > (int)MAX_SIDE) { cover_soft_free(rgb); return false; }

    // The centred square, so a cover that is not quite square is cropped rather than squashed.
    const int side = w < h ? w : h;
    const int ox = (w - side) / 2, oy = (h - side) / 2;
    const int slot = free_slot(g);
    uint16_t* dst = g.slot[slot];
    for (int y = 0; y < COVER_PX; y++) {
        const int sy0 = oy + y * side / COVER_PX;
        int sy1 = oy + (y + 1) * side / COVER_PX;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int x = 0; x < COVER_PX; x++) {
            const int sx0 = ox + x * side / COVER_PX;
            int sx1 = ox + (x + 1) * side / COVER_PX;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            dst[y * COVER_PX + x] = box_average(rgb, w, sx0, sy0, sx1 - sx0, sy1 - sy0);
        }
    }
    cover_soft_free(rgb);
    const bool published = publish(g, slot, expected);
    // The high-water mark covers every image this task has decoded, so the line answers
    // "is SOFT_STACK still enough" for the whole session, not for this cover alone.
    ESP_LOGI(TAG, "artwork %dx%d -> %d px (software; decoder stack never below %u B free)",
             w, h, COVER_PX, (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    return published;
}

/// The software path needs more stack than any task that hands this file a cover can spare,
/// so it runs on a task of its own and the caller waits for it.
///
/// A PNG takes 8736 bytes of stack under cover_soft_decode(): the inflate state with both
/// Huffman tables in it, 4112; the code-length table built under that, 2544; the PNG
/// reader's palette above both, 1296. A progressive JPEG takes 868. Measured by running the
/// firmware's own cover_soft.c.obj under QEMU on a painted stack, and within 16 bytes of
/// -fstack-usage. The callers had 6 KB
/// (Bluetooth, AirPlay), 8 KB (the player) and 12 KB with 2.7 KB of it their own (DLNA), so a
/// PNG cover through any of them was a stack overflow and a reboot - which the radio's art
/// task, then 8 KB, met first, on a station's logo.
///
/// 16 KB leaves at least 6.7 KB under the deepest image measured, the interrupt frame and
/// every coprocessor save area counted. PSRAM, because this task never writes flash; core 0,
/// because core 1 is the audio task's. It takes the priority of whoever asked, so a cover is
/// decoded exactly as urgently as it was on the caller's own stack.
constexpr uint32_t SOFT_STACK = 16 * 1024;

/// What the decoder's task is to run: `run(arg)`, a tile's decode or a thumbnail's.
struct SoftJob {
    bool (*run)(void* arg);
    void* arg;
    bool  ok;
};
TaskHandle_t soft_task = nullptr;
SoftJob*     soft_job  = nullptr;   ///< the waiting caller's, valid until soft_done() is given

SemaphoreHandle_t soft_done() {
    static StaticSemaphore_t storage;
    static SemaphoreHandle_t done = xSemaphoreCreateBinaryStatic(&storage);
    return done;
}

void soft_worker(void*) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        soft_job->ok = soft_job->run(soft_job->arg);
        xSemaphoreGive(soft_done());
    }
}

/// `run(arg)` on the decoder's task, the caller waiting. Every caller holds the DecodeLock, so
/// there is one job at a time and soft_job belongs to it.
bool on_soft_task(bool (*run)(void*), void* arg) {
    const UBaseType_t prio = uxTaskPriorityGet(nullptr);
    if (!soft_task &&
        xTaskCreatePinnedToCoreWithCaps(soft_worker, "cover_soft", SOFT_STACK, nullptr, prio,
                                        &soft_task, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGW(TAG, "no task for the software decoder: PNG and progressive covers not shown");
        return false;
    }
    vTaskPrioritySet(soft_task, prio);
    SoftJob job{run, arg, false};
    soft_job = &job;
    xTaskNotifyGive(soft_task);
    // A task that blocks on the DecodeLock meanwhile lends the caller its priority, but it is
    // waiting for this decode, not for the caller, and FreeRTOS lends no further. So pass on
    // whatever the caller holds now.
    while (xSemaphoreTake(soft_done(), pdMS_TO_TICKS(10)) != pdTRUE)
        vTaskPrioritySet(soft_task, uxTaskPriorityGet(nullptr));
    return job.ok;
}

struct TileJob {
    Store*         g;
    const uint8_t* jpg;
    size_t         len;
    uint32_t       expected;
};

/// decode_soft() on the decoder's task.
bool set_jpeg_soft(Store& g, const uint8_t* jpg, size_t len, uint32_t expected) {
    TileJob t{&g, jpg, len, expected};
    return on_soft_task([](void* p) {
        const TileJob& j = *static_cast<TileJob*>(p);
        return decode_soft(*j.g, j.jpg, j.len, j.expected);
    }, &t);
}

struct ThumbJob {
    const uint8_t* img;
    size_t         len;
    uint16_t*      out;
    int            side;
};

/// A thumbnail through stb_image, on the decoder's task: the same admission as a tile, and a PNG
/// keeps its alpha, which cover_thumb puts over the ground.
bool thumb_soft(const ThumbJob& j) {
    int w = 0, h = 0;
    bool png = false;
    if (!soft_admit(j.img, j.len, w, h, png)) return false;
    const int comp = png ? 4 : 3;
    uint8_t* px = cover_soft_decode(j.img, (int)j.len, &w, &h, comp);
    if (!px) {
        ESP_LOGW(TAG, "software decode failed: %s", soft_reason());
        return false;
    }
    if (w <= 0 || h <= 0 || w > (int)MAX_SIDE || h > (int)MAX_SIDE) { cover_soft_free(px); return false; }
    const auto layout = png ? cover_thumb::Layout::RGBA8888 : cover_thumb::Layout::RGB888;
    const size_t stride = (size_t)w * comp;
    cover_thumb::render(px, layout, w, h, stride, j.out, j.side,
                        cover_thumb::ground(px, layout, w, h, stride));
    cover_soft_free(px);
    ESP_LOGI(TAG, "logo %dx%d -> %d px (software)", w, h, j.side);
    return true;
}

bool set_thumb_soft(ThumbJob& j) {
    return on_soft_task([](void* p) { return thumb_soft(*static_cast<ThumbJob*>(p)); }, &j);
}

}  // namespace

bool cover_set_jpeg(const uint8_t* jpg, size_t len, CoverSource source, uint32_t expected_generation) {
    DecodeLock lock;
    Store& g = stores[(int)source];
    if (expected_generation != UINT32_MAX && g.gen.load() != expected_generation) return false;
    if (!jpg || len < 4 || !ensure_ready(g)) return false;

    if (is_png(jpg, len)) return set_jpeg_soft(g, jpg, len, expected_generation);
    jpeg_decode_picture_info_t info = {};
    if (jpeg_decoder_get_info(jpg, (uint32_t)len, &info) != ESP_OK) {
        return set_jpeg_soft(g, jpg, len, expected_generation);        // progressive, most likely
    }
    // MAX_SIDE_HW, not MAX_SIDE: this engine costs W x H x 2 and asks for it through an
    // allocator that fails cleanly, so the ceiling can be where the memory actually runs out
    // rather than where the software decoder's does. A 2560-pixel cover is 13 MB here and
    // 49 MB through stb.
    if (!info.width || !info.height || info.width > MAX_SIDE_HW || info.height > MAX_SIDE_HW) {
        ESP_LOGW(TAG, "artwork %lux%lu is out of range (limit %u a side)",
                 (unsigned long)info.width, (unsigned long)info.height, (unsigned)MAX_SIDE_HW);
        return false;
    }

    // The decoded picture, at macroblock-aligned dimensions. The helper adds whatever
    // alignment the DMA needs and reports how much it actually took.
    uint32_t aw = 0, ah = 0;
    if (!jpeg_layout(info, aw, ah)) return set_jpeg_soft(g, jpg, len, expected_generation);
    size_t full_size = 0;
    jpeg_decode_memory_alloc_cfg_t mem = {};
    mem.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER;
    uint8_t* full = (uint8_t*)jpeg_alloc_decoder_mem((size_t)aw * ah * 2, &mem, &full_size);
    if (!full) {
        ESP_LOGW(TAG, "no room to decode %lux%lu", (unsigned long)aw, (unsigned long)ah);
        return false;
    }

    // The bit stream has to be reachable by the engine's DMA too, so it is copied into a
    // buffer the driver's own allocator provided rather than fed from wherever it was read.
    size_t in_size = 0;
    jpeg_decode_memory_alloc_cfg_t in_mem = {};
    in_mem.buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER;
    uint8_t* in = (uint8_t*)jpeg_alloc_decoder_mem(len, &in_mem, &in_size);
    if (!in) {
        free(full);
        ESP_LOGW(TAG, "no room for %u bytes of JPEG", (unsigned)len);
        return false;
    }
    memcpy(in, jpg, len);

    bool ok = false;
    uint32_t out_size = 0;
    jpeg_decode_cfg_t dec = {};
    dec.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
    // IDF's RGB565 "RGB" setting emits big-endian bytes. PPA/LVGL consume
    // native little-endian RGB565; the documented "BGR" setting provides it.
    dec.rgb_order     = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;
    const esp_err_t derr =
        jpeg_decoder_process(s_jpeg, &dec, in, (uint32_t)len, full, full_size, &out_size);
    if (derr == ESP_OK && out_size == aw * ah * 2) {
        // Centre square of the source, scaled to as near the card as the PPA's grid allows.
        const uint32_t side = info.width < info.height ? info.width : info.height;
        const float    scale = scale_for(side);
        uint32_t block = (uint32_t)((float)COVER_PX / scale);
        if (block > side) block = side;
        uint32_t out_w = (uint32_t)((float)block * scale);
        if (out_w > (uint32_t)COVER_PX) out_w = COVER_PX;
        if (!out_w) out_w = 1;

        const int next = free_slot(g);
        // Letterbox colour for the strip the scaled block does not cover.
        memset(g.slot[next], 0, (size_t)COVER_PX * COVER_PX * sizeof(uint16_t));
        // Written back now, because the PPA will not: ppa_srm.c only invalidates the output
        // rows (M2C) before its DMA, which drops these zeros where they are still dirty in the
        // cache, and the columns beside the block keep whatever PSRAM held.
        esp_cache_msync(g.slot[next], (COVER_PX * COVER_PX * sizeof(uint16_t) + 63) / 64 * 64,
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);

        ppa_srm_oper_config_t op = {};
        op.in.buffer         = full;
        op.in.pic_w          = aw;         // the padded picture is what is actually in memory
        op.in.pic_h          = ah;
        op.in.block_w        = block;
        op.in.block_h        = block;
        op.in.block_offset_x = (info.width  - block) / 2;
        op.in.block_offset_y = (info.height - block) / 2;
        op.in.srm_cm         = PPA_SRM_COLOR_MODE_RGB565;
        op.out.buffer        = g.slot[next];
        op.out.buffer_size   = (uint32_t)((COVER_PX * COVER_PX * sizeof(uint16_t) + 63) / 64 * 64);
        op.out.pic_w         = COVER_PX;
        op.out.pic_h         = COVER_PX;
        op.out.block_offset_x = (COVER_PX - out_w) / 2;
        op.out.block_offset_y = (COVER_PX - out_w) / 2;
        op.out.srm_cm        = PPA_SRM_COLOR_MODE_RGB565;
        op.rotation_angle    = PPA_SRM_ROTATION_ANGLE_0;
        op.scale_x           = scale;
        op.scale_y           = scale;
        op.mode              = PPA_TRANS_MODE_BLOCKING;

        if (ppa_do_scale_rotate_mirror(s_ppa, &op) == ESP_OK) {
            ok = publish(g, next, expected_generation);
            ESP_LOGI(TAG, "artwork %lux%lu (DMA %lux%lu, sampling %d) -> %d px",
                     (unsigned long)info.width, (unsigned long)info.height,
                     (unsigned long)aw, (unsigned long)ah, (int)info.sample_method, COVER_PX);
        } else {
            ESP_LOGW(TAG, "PPA refused %lux%lu block %lu scale %.4f",
                     (unsigned long)info.width, (unsigned long)info.height,
                     (unsigned long)block, (double)scale);
        }
    }
    if (derr == ESP_OK && out_size != aw * ah * 2)
        ESP_LOGW(TAG, "JPEG output layout mismatch: %lu bytes for %lux%lu",
                 (unsigned long)out_size, (unsigned long)aw, (unsigned long)ah);
    free(in);
    free(full);
    // The engine reads baseline JPEG only. Progressive is common enough on real albums that
    // refusing it means refusing whole records, so the slow decoder gets its turn.
    if (!ok && (expected_generation == UINT32_MAX || g.gen.load() == expected_generation))
        ok = set_jpeg_soft(g, jpg, len, expected_generation);
    return ok;
}

bool cover_thumb(const uint8_t* img, size_t len, uint16_t* out, int side) {
    if (!img || len < 4 || !out || side <= 0) return false;
    DecodeLock lock;
    ThumbJob job{img, len, out, side};
    if (is_png(img, len)) return set_thumb_soft(job);
    ensure_engines();
    jpeg_decode_picture_info_t info = {};
    uint32_t aw = 0, ah = 0;
    // Progressive, grayscale or no engine: the software decoder, as for a cover.
    if (!s_jpeg || jpeg_decoder_get_info(img, (uint32_t)len, &info) != ESP_OK || !jpeg_layout(info, aw, ah))
        return set_thumb_soft(job);
    if (!info.width || !info.height || info.width > MAX_SIDE_HW || info.height > MAX_SIDE_HW) {
        ESP_LOGW(TAG, "logo %lux%lu is out of range (limit %u a side)",
                 (unsigned long)info.width, (unsigned long)info.height, (unsigned)MAX_SIDE_HW);
        return false;
    }
    // The whole picture decoded at its macroblock-aligned size, then averaged down here: the PPA
    // scales by sixteen at most, and a logo of a few hundred pixels needs more than that.
    size_t full_size = 0, in_size = 0;
    jpeg_decode_memory_alloc_cfg_t mem = {};
    mem.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER;
    uint8_t* full = (uint8_t*)jpeg_alloc_decoder_mem((size_t)aw * ah * 2, &mem, &full_size);
    jpeg_decode_memory_alloc_cfg_t in_mem = {};
    in_mem.buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER;
    uint8_t* in = full ? (uint8_t*)jpeg_alloc_decoder_mem(len, &in_mem, &in_size) : nullptr;
    if (!full || !in) {
        free(in);
        free(full);
        ESP_LOGW(TAG, "no room to decode a %lux%lu logo", (unsigned long)aw, (unsigned long)ah);
        return false;
    }
    memcpy(in, img, len);
    jpeg_decode_cfg_t dec = {};
    dec.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
    dec.rgb_order     = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;   // native little-endian RGB565, as for a tile
    uint32_t out_size = 0;
    const bool ok = jpeg_decoder_process(s_jpeg, &dec, in, (uint32_t)len, full, full_size, &out_size) == ESP_OK &&
                    out_size == aw * ah * 2;
    if (ok) {
        const size_t stride = (size_t)aw * 2;
        const int w = (int)info.width, h = (int)info.height;
        cover_thumb::render(full, cover_thumb::Layout::RGB565, w, h, stride, out, side,
                            cover_thumb::ground(full, cover_thumb::Layout::RGB565, w, h, stride));
        ESP_LOGI(TAG, "logo %dx%d -> %d px", w, h, side);
    }
    free(in);
    free(full);
    return ok || set_thumb_soft(job);
}

void cover_clear(CoverSource source) {
    Store& g = stores[(int)source];
    portENTER_CRITICAL(&publication_mux);
    g.cur.store(-1, std::memory_order_release);
    // Also cancels a decoder which has not published yet.
    g.gen.fetch_add(1, std::memory_order_release);
    portEXIT_CRITICAL(&publication_mux);
}

CoverView cover_view(CoverSource source) {
    Store& g = stores[(int)source];
    CoverView v;
    portENTER_CRITICAL(&publication_mux);
    v.gen = g.gen.load(std::memory_order_acquire);
    const int32_t c = g.cur.load(std::memory_order_acquire);
    if (c >= 0) { v.px = g.slot[c]; v.w = g.w[c]; v.h = g.h[c]; v.rgb = g.rgb[c]; }
    portEXIT_CRITICAL(&publication_mux);
    return v;
}

CoverView cover_now(int32_t source, bool push, CoverSource& tile) {
    if (!cover_tile_for(source, push, tile)) return CoverView{};
    CoverView v = cover_view(tile);
    if (tile == CoverSource::Network && !v.px) {
        tile = CoverSource::Radio;
        v = cover_view(tile);
    }
    return v;
}

}  // namespace player_core
