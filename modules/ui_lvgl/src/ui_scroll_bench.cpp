/**
 * @file ui_scroll_bench.cpp
 * @brief GET /api/ui/scrollbench — a scripted scroll of a long list, timed frame by frame.
 *
 * Development panels only (CONFIG_UI_SCROLL_BENCH). Scrolling is where this UI is slowest and
 * where "it feels better" is least trustworthy, so a change to the drawing path is judged by
 * this instead: the same 600 synthetic rows at the same speed for the same time, on every
 * build.
 *
 * The list is a real m3w::VList on an opaque host in front of the page, so what is timed is
 * the path a card listing takes: rows recycled by VList::place, labels laid out, the area
 * rendered, rotated by the PPA and copied into the frame buffer. The scroll moves in
 * LV_EVENT_REFR_START, once per refresh and by the time since the previous one, so the speed
 * does not depend on how fast frames come: a slow frame shows as a bigger step, as under a
 * finger's flick, and as fewer frames.
 *
 * One frame's cost, split the only way LVGL's own events allow:
 *   frame  — REFR_START to REFR_READY: layout, rendering, every flush;
 *   flush  — FLUSH_START to FLUSH_FINISH, summed: the port's flush callback, which is the
 *            PPA rotation (blocking) and the start of the DMA2D copy;
 *   wait   — FLUSH_WAIT_START to FLUSH_WAIT_FINISH, summed: LVGL spinning until the
 *            previous copy has finished;
 *   place  — VList::place, called from the scroll event inside the frame;
 *   render — what is left: scrolling the objects, layout, drawing.
 *
 * Open to anyone on the network, as /api/screenshot is under CONFIG_UI_SCREENSHOT_OPEN: a
 * bench panel's password is not something a script should hold. A run lasts at most 20 s and
 * changes nothing that persists. The firmware says so at boot.
 */

#include "ui_scroll_bench.h"
#include "sdkconfig.h"

#if CONFIG_UI_SCROLL_BENCH

#include "ui_common.h"
#include "ui_m3_widgets.h"
#include "ppa_flush.h"
#include "modesp/net/http_routes.h"
#include "driver/ppa.h"
extern "C" {                   // esp_lcd's private header has no C++ guard of its own
#include "esp_async_fbcpy.h"   // esp_lcd's DMA2D 2D copy; see CMakeLists.txt
}
#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "src/display/lv_display_private.h"   // the two draw buffers, for ?lines=
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static const char* TAG = "UiBench";

namespace {

constexpr int     ENTRIES   = 600;
constexpr int     HIST_MS   = 100;      ///< 1 ms buckets; the last one holds everything longer
constexpr int64_t SETTLE_US = 300000;   ///< the list's first full paint is not part of the run

struct Acc {
    uint32_t n   = 0;
    uint64_t sum = 0;
    uint32_t max = 0;
    void add(uint32_t v) { n++; sum += v; if (v > max) max = v; }
    double avg_ms() const { return n ? (double)sum / (double)n / 1000.0 : 0.0; }
};

/// Written by the LVGL task while a run is active, read by the HTTP task after it.
struct Stats {
    int32_t  speed = 0;                  ///< px/s
    int64_t  t_start = 0, t_turn = 0, t_end = 0;
    int64_t  last_step = 0, last_refr = 0, first_refr = 0, final_refr = 0;
    float    carry = 0.0f;               ///< the part of a pixel not scrolled yet
    int64_t  scrolled_px = 0;
    // the frame being timed
    bool     timing = false;
    int64_t  t_frame = 0, t_flush = 0, t_wait = 0;
    uint32_t flush_us = 0, wait_us = 0, place_us = 0;
    // results
    Acc      frame, interval, flush, wait, place, render;
    uint16_t frame_hist[HIST_MS] = {};
    uint16_t interval_hist[HIST_MS] = {};
};

std::atomic<bool> s_active{false};
std::atomic<uint32_t> s_refr_starts{0};      ///< every REFR_START of a run, settling included
std::atomic<int64_t>  s_run_t0{0};           ///< main-loop gaps count from here
// The main loop's pace, kept by ui_scroll_bench_note_main_turn (main loop, one writer).
std::atomic<uint32_t> s_main_turns{0};
std::atomic<uint32_t> s_main_gap_max{0};     ///< µs, longest wait between turns during a run
int64_t               s_main_last = 0;
std::atomic<bool> s_done{false};
Stats             s_st;
m3w::VList        s_list;
lv_obj_t*         s_host = nullptr;
// ?ring=1: a finger's touch ring over the list, as the UI module draws it, moving with the content
// it drags but only every third frame — the module updates it every 40 ms (ui_lvgl_module.cpp).
lv_obj_t*         s_ring = nullptr;
int32_t           s_ring_due = 0;     ///< content movement not yet followed
int               s_ring_frames = 0;

// ?overlay=1: a popup over the list's lower part, gone the very frame the scroll starts — what a
// shift must not carry along. Magenta, so a copy of it left behind shows in the screenshot.
lv_obj_t*         s_overlay = nullptr;

void drop_overlay() {
    if (!s_overlay) return;
    lv_obj_delete(s_overlay);
    s_overlay = nullptr;
}

void drop_ring() {
    if (!s_ring) return;
    ppa_flush_floating(s_ring, false);
    lv_obj_delete(s_ring);
    s_ring = nullptr;
}

void bucket(uint16_t* h, uint32_t us) {
    uint32_t ms = us / 1000;
    if (ms >= (uint32_t)HIST_MS) ms = HIST_MS - 1;
    if (h[ms] < UINT16_MAX) h[ms]++;
}

/// The upper edge, in ms, of the bucket that holds the 95th percentile.
uint32_t p95_ms(const uint16_t* h, uint32_t n) {
    if (!n) return 0;
    const uint32_t want = (n * 95 + 99) / 100;
    uint32_t seen = 0;
    for (int i = 0; i < HIST_MS; i++) {
        seen += h[i];
        if (seen >= want) return (uint32_t)i + 1;
    }
    return HIST_MS;
}

/// A card listing at its worst: every title too long for the row, so every label ends in an
/// ellipsis, Cyrillic throughout, a subtitle on every row and one row highlighted.
void fill(int idx, m3w::VItem& it, void*) {
    static char title[128], sub[80];
    snprintf(title, sizeof(title),
             "%03d. Довга назва композиції, яка не вміщається в один рядок списку", idx + 1);
    snprintf(sub, sizeof(sub), "Виконавець %d — Альбом %d · FLAC 24/96", idx % 37 + 1, idx % 11 + 1);
    it.lead  = UI_SYM_MUSIC;
    it.title = title;
    it.sub   = sub;
    it.on    = (idx == 3);
}

/// Towards the end of the list for the first half of the run, back for the second.
void step_scroll(int64_t now) {
    lv_obj_t* o = s_list.obj();
    if (!o) return;
    const float dy = (float)s_st.speed * (float)(now - s_st.last_step) / 1e6f + s_st.carry;
    s_st.last_step = now;
    const int32_t px = (int32_t)dy;
    s_st.carry = dy - (float)px;
    if (px <= 0) return;
    lv_obj_scroll_by_bounded(o, 0, now < s_st.t_turn ? -px : px, LV_ANIM_OFF);
    s_st.scrolled_px += px;
    if (s_ring) {
        s_ring_due += now < s_st.t_turn ? -px : px;
        if (++s_ring_frames % 3 == 0) {
            int32_t y = lv_obj_get_y(s_ring) + s_ring_due;
            s_ring_due = 0;
            const int32_t top = uig::CONT_Y, span = uig::CONT_H - 30;
            while (y < top) y += span;
            while (y > top + span) y -= span;
            lv_obj_set_y(s_ring, y);
        }
    }
}

void disp_cb(lv_event_t* e) {
    if (!s_active.load(std::memory_order_acquire)) return;
    const int64_t now = esp_timer_get_time();
    switch (lv_event_get_code(e)) {
    case LV_EVENT_REFR_START: {
        // LVGL pauses its refresh timer at the start of every refresh and only an invalidation
        // resumes it (lv_refr.c), so a list that is not moving yet would stop the run before it
        // began. An invalidation is not enough on its own: lv_inv_area drops an area that one
        // already pending contains without resuming anything, and the host's own first paint is
        // such an area - runs stalled for the whole of their time that way. So the timer is
        // resumed outright. The list itself is not invalidated: the scroll below repaints what
        // it has to, and with two frame buffers that is only the strip that came into view.
        s_refr_starts.fetch_add(1, std::memory_order_relaxed);
        if (lv_timer_t* rt = lv_display_get_refr_timer(lv_display_get_default())) lv_timer_resume(rt);
        if (now < s_st.t_start) { s_st.last_step = now; return; }   // still settling
        if (s_overlay && !lv_obj_has_flag(s_overlay, LV_OBJ_FLAG_HIDDEN))
            lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);           // gone as the scroll starts
        if (s_st.last_refr) {
            const uint32_t d = (uint32_t)(now - s_st.last_refr);
            s_st.interval.add(d);
            bucket(s_st.interval_hist, d);
        } else {
            s_st.first_refr = now;
        }
        s_st.last_refr = now;
        s_st.timing = true;
        s_st.t_frame = now;
        s_st.flush_us = s_st.wait_us = s_st.place_us = 0;
        step_scroll(now);
        break;
    }
    case LV_EVENT_FLUSH_START:
        if (s_st.timing) s_st.t_flush = now;
        break;
    case LV_EVENT_FLUSH_FINISH:
        if (s_st.timing && s_st.t_flush) { s_st.flush_us += (uint32_t)(now - s_st.t_flush); s_st.t_flush = 0; }
        break;
    case LV_EVENT_FLUSH_WAIT_START:
        if (s_st.timing) s_st.t_wait = now;
        break;
    case LV_EVENT_FLUSH_WAIT_FINISH:
        if (s_st.timing && s_st.t_wait) { s_st.wait_us += (uint32_t)(now - s_st.t_wait); s_st.t_wait = 0; }
        break;
    case LV_EVENT_REFR_READY: {
        if (!s_st.timing) return;
        s_st.timing = false;
        const uint32_t f = (uint32_t)(now - s_st.t_frame);
        s_st.frame.add(f);
        bucket(s_st.frame_hist, f);
        s_st.flush.add(s_st.flush_us);
        s_st.wait.add(s_st.wait_us);
        const uint32_t known = s_st.flush_us + s_st.wait_us + s_st.place_us;
        s_st.render.add(f > known ? f - known : 0);
        s_st.final_refr = now;
        if (now >= s_st.t_end) {
            s_active.store(false, std::memory_order_release);
            s_done.store(true, std::memory_order_release);
        }
        break;
    }
    default:
        break;
    }
}

/// After the run, before the host goes: every row on show must say the entry its position
/// belongs to. VList recycles rows as the list scrolls, and a row left showing the wrong
/// entry is exactly the fault that recycling can make. The row's children are in VList's
/// build order (lead, title, sub, trail) and a title starts with the entry's number.
void check_rows(int& shown, int& wrong) {
    shown = wrong = 0;
    lv_obj_t* sc = s_list.obj();
    if (!sc) return;
    const uint32_t n = lv_obj_get_child_count(sc);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t* row = lv_obj_get_child(sc, (int32_t)i);
        if (lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN) || lv_obj_get_child_count(row) < 2) continue;
        // The y VList gave it (its local style), not its coordinates: those wait for a layout.
        const int idx = (int)((lv_obj_get_style_y(row, LV_PART_MAIN) - 2) / m3w::ROW_H);   // VROW_GAP / 2 above
        char want[16];
        snprintf(want, sizeof(want), "%03d.", idx + 1);
        const char* have = lv_label_get_text(lv_obj_get_child(row, 1));
        shown++;
        if (!have || strncmp(have, want, 4) != 0) wrong++;
    }
}

/// The draw buffers as the display was made with them, for ?lines= to come back to.
uint32_t s_buf_bytes = 0;

/// Render in blocks of at most `lines` rows of the full width: the same two buffers, LVGL just
/// told they are smaller. 0 puts them back. Under the lock, between frames.
uint32_t set_draw_lines(int lines) {
    lv_display_t* d = lv_display_get_default();
    if (!d || !d->buf_1 || !d->buf_2) return 0;
    if (!s_buf_bytes) s_buf_bytes = d->buf_1->data_size;
    const uint32_t row = (uint32_t)lv_display_get_horizontal_resolution(d) *
                         lv_color_format_get_size(lv_display_get_color_format(d));
    uint32_t bytes = lines > 0 ? (uint32_t)lines * row : s_buf_bytes;
    if (bytes > s_buf_bytes) bytes = s_buf_bytes;
    if (bytes != d->buf_1->data_size)
        lv_display_set_buffers(d, d->buf_1->data, d->buf_2->data, bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    return row ? d->buf_1->data_size / row : 0;
}

/// ?ibuf=N: for one run, LVGL draws into two N-row buffers in internal SRAM instead of its own
/// PSRAM ones. Whether the drawing is held up by PSRAM is the question; these answer it.
uint8_t* s_ib[2] = {nullptr, nullptr};
void*    s_ob[2] = {nullptr, nullptr};

uint32_t use_internal(int lines) {
    lv_display_t* d = lv_display_get_default();
    if (!d || !d->buf_1 || !d->buf_2 || lines <= 0) return 0;
    const uint32_t row = (uint32_t)lv_display_get_horizontal_resolution(d) *
                         lv_color_format_get_size(lv_display_get_color_format(d));
    const uint32_t bytes = (uint32_t)lines * row;
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT;
    s_ib[0] = (uint8_t*)heap_caps_aligned_alloc(64, bytes, caps);
    s_ib[1] = (uint8_t*)heap_caps_aligned_alloc(64, bytes, caps);
    if (!s_ib[0] || !s_ib[1]) {
        heap_caps_free(s_ib[0]); heap_caps_free(s_ib[1]);
        s_ib[0] = s_ib[1] = nullptr;
        return 0;
    }
    if (!s_buf_bytes) s_buf_bytes = d->buf_1->data_size;
    s_ob[0] = d->buf_1->data;
    s_ob[1] = d->buf_2->data;
    lv_display_set_buffers(d, s_ib[0], s_ib[1], bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    return bytes / row;
}

/// Back to the display's own buffers. The last block may still be on its way through the PPA,
/// out of an internal buffer, so that is waited for before the buffer goes.
void drop_internal() {
    if (!s_ib[0]) return;
    lv_display_t* d = lv_display_get_default();
    for (int i = 0; i < 100 && d->flushing; i++) vTaskDelay(1);
    lv_display_set_buffers(d, s_ob[0], s_ob[1], s_buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    heap_caps_free(s_ib[0]); heap_caps_free(s_ib[1]);
    s_ib[0] = s_ib[1] = nullptr;
}

/// MB/s of `bytes` moved by `fn`, best of three.
template <typename F> double mbps(uint32_t bytes, F fn) {
    int64_t best = INT64_MAX;
    for (int k = 0; k < 3; k++) {
        const int64_t t0 = esp_timer_get_time();
        fn();
        const int64_t dt = esp_timer_get_time() - t0;
        if (dt < best) best = dt;
    }
    return best > 0 ? (double)bytes / (double)best : 0.0;   // bytes per us = MB/s
}

/// ?mem=1: how fast this panel moves the bytes LVGL moves. A 752-pixel RGB888 row is 2256
/// bytes; LVGL fills a rectangle by copying its first row down, and draws an opaque image by
/// copying its rows, so row copies are what is timed — into PSRAM as the draw buffers are now,
/// and inside internal SRAM. The panel keeps scanning out of PSRAM throughout, as in use.
void mem_probe(char* out, size_t n) {
    constexpr uint32_t ROW = 2256, PS = ROW * 63, IN = ROW * 7;
    uint8_t* p1 = (uint8_t*)heap_caps_aligned_alloc(64, PS, MALLOC_CAP_SPIRAM);
    uint8_t* p2 = (uint8_t*)heap_caps_aligned_alloc(64, PS, MALLOC_CAP_SPIRAM);
    uint8_t* i1 = (uint8_t*)heap_caps_aligned_alloc(64, IN, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint8_t* i2 = (uint8_t*)heap_caps_aligned_alloc(64, IN, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t int_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t int_big  = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    double ps_fill = 0, ps_copy = 0, ps_small = 0, in_fill = 0, in_copy = 0, in_to_ps = 0;
    if (p1 && p2) {
        memset(p1, 0x5a, PS);
        ps_fill = mbps(PS - ROW, [&] { for (uint32_t o = ROW; o < PS; o += ROW) memcpy(p1 + o, p1, ROW); });
        ps_copy = mbps(PS, [&] { for (uint32_t o = 0; o < PS; o += ROW) memcpy(p2 + o, p1 + o, ROW); });
        // the same copy over 16 KB, which stays in the L2 cache
        ps_small = mbps(16 * ROW * 7, [&] {
            for (int r = 0; r < 16; r++) for (uint32_t o = 0; o < 7 * ROW; o += ROW) memcpy(p2 + o, p1 + o, ROW);
        });
    }
    if (i1 && i2) {
        memset(i1, 0x5a, IN);
        in_fill = mbps(9 * (IN - ROW), [&] {
            for (int r = 0; r < 9; r++) for (uint32_t o = ROW; o < IN; o += ROW) memcpy(i1 + o, i1, ROW);
        });
        in_copy = mbps(9 * IN, [&] {
            for (int r = 0; r < 9; r++) for (uint32_t o = 0; o < IN; o += ROW) memcpy(i2 + o, i1 + o, ROW);
        });
        if (p1) in_to_ps = mbps(9 * IN, [&] {
            for (int r = 0; r < 9; r++) for (uint32_t o = 0; o < IN; o += ROW) memcpy(p1 + r * IN + o, i1 + o, ROW);
        });
    }
    heap_caps_free(p1); heap_caps_free(p2); heap_caps_free(i1); heap_caps_free(i2);
    snprintf(out, n,
             "{\"mb_per_s\":{\"psram_fill\":%.1f,\"psram_copy\":%.1f,\"psram_copy_in_l2\":%.1f,"
             "\"sram_fill\":%.1f,\"sram_copy\":%.1f,\"sram_to_psram\":%.1f},"
             "\"internal_free\":%u,\"internal_dma_largest\":%u}",
             ps_fill, ps_copy, ps_small, in_fill, in_copy, in_to_ps, (unsigned)int_free, (unsigned)int_big);
}

/// ?blit=1: what the hardware can move instead of the CPU. A list that scrolls could be shifted
/// in the frame buffer rather than drawn again; these time the candidates on the list's own
/// window as it lies in the panel's frame buffer (its rows turned to columns: 384 x 752 pixels),
/// always into frame buffer 1, which nothing scans out. µs, best of three.
SemaphoreHandle_t s_blit_sem = nullptr;

IRAM_ATTR bool blit_done(esp_async_fbcpy_handle_t, esp_async_fbcpy_event_data_t*, void*) {
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_blit_sem, &woken);
    return woken == pdTRUE;
}

/// One DMA2D copy of a `cw` x `ch` window from (sx, sy) of `src` to (dx, dy) of `dst`, both
/// frame buffers `w` x `h`. µs from the call to its done, negative on failure.
int64_t dma_copy(esp_async_fbcpy_handle_t fc, const uint8_t* src, uint8_t* dst, uint32_t w, uint32_t h,
                 uint32_t sx, uint32_t sy, uint32_t dx, uint32_t dy, uint32_t cw, uint32_t ch) {
    esp_async_fbcpy_trans_desc_t t = {};
    t.src_buffer = src;
    t.dst_buffer = dst;
    t.src_buffer_size_x = w; t.src_buffer_size_y = h;
    t.dst_buffer_size_x = w; t.dst_buffer_size_y = h;
    t.src_offset_x = sx; t.src_offset_y = sy;
    t.dst_offset_x = dx; t.dst_offset_y = dy;
    t.copy_size_x = cw;  t.copy_size_y = ch;
    t.pixel_format_unique_id.color_type_id = COLOR_TYPE_ID(COLOR_SPACE_RGB, COLOR_PIXEL_RGB888);
    xSemaphoreTake(s_blit_sem, 0);
    const int64_t t0 = esp_timer_get_time();
    if (esp_async_fbcpy(fc, &t, blit_done, nullptr) != ESP_OK) return -1;
    if (xSemaphoreTake(s_blit_sem, pdMS_TO_TICKS(500)) != pdTRUE) return -2;
    return esp_timer_get_time() - t0;
}

template <typename F> int64_t best_of_3(F fn) {
    int64_t best = INT64_MAX;
    for (int k = 0; k < 3; k++) {
        const int64_t v = fn();
        if (v < 0) return v;
        if (v < best) best = v;
    }
    return best;
}

/// One blocking PPA rotation (0 or 270 degrees) of a `bw` x `bh` block of `in` (`iw` x `ih`, at
/// (ix, iy)) into frame buffer `out` at (ox, oy). µs, negative on failure.
int64_t ppa_copy(ppa_client_handle_t srm, const uint8_t* in, uint32_t iw, uint32_t ih, uint32_t ix, uint32_t iy,
                 uint32_t bw, uint32_t bh, uint8_t* out, uint32_t ow, uint32_t oh, uint32_t ox, uint32_t oy,
                 ppa_srm_rotation_angle_t rot) {
    ppa_srm_oper_config_t op = {};
    op.in.buffer = in;
    op.in.pic_w = iw;  op.in.pic_h = ih;
    op.in.block_w = bw; op.in.block_h = bh;
    op.in.block_offset_x = ix; op.in.block_offset_y = iy;
    op.in.srm_cm = PPA_SRM_COLOR_MODE_RGB888;
    op.out.buffer = out;
    op.out.buffer_size = ow * oh * 3;
    op.out.pic_w = ow; op.out.pic_h = oh;
    op.out.block_offset_x = ox; op.out.block_offset_y = oy;
    op.out.srm_cm = PPA_SRM_COLOR_MODE_RGB888;
    op.rotation_angle = rot;
    op.scale_x = 1.0f; op.scale_y = 1.0f;
    op.mode = PPA_TRANS_MODE_BLOCKING;
    const int64_t t0 = esp_timer_get_time();
    if (ppa_do_scale_rotate_mirror(srm, &op) != ESP_OK) return -1;
    return esp_timer_get_time() - t0;
}

void blit_probe(char* out, size_t n) {
    PpaFlushFbs f = {};
    // From the buffer on the glass into the one nothing scans: frame buffer 1 when there is
    // one buffer to scan, the idle one when frames are composed off the glass.
    uint8_t* idle = ppa_flush_double() ? ppa_flush_idle_back() : nullptr;
    if (!ppa_flush_frame_buffers(&f) || !f.fb[1] || (ppa_flush_double() && !idle)) {
        snprintf(out, n, "{\"error\":\"needs two DPI frame buffers\"}");
        return;
    }
    if (!s_blit_sem) s_blit_sem = xSemaphoreCreateBinary();
    esp_async_fbcpy_handle_t fc = nullptr;
    esp_async_fbcpy_config_t fcfg = {};
    ppa_client_handle_t srm = nullptr;
    ppa_client_config_t pcfg = {};
    pcfg.oper_type = PPA_OPERATION_SRM;
    pcfg.max_pending_trans_num = 1;
    // The list's window, logical (CONT_X, CONT_Y, 752 x 384), in the frame buffer of a panel
    // turned 270 degrees: logical rows are its columns, x = 479 - y.
    const uint32_t lw = uig::CONT_H, lh = uig::CONT_W;                  // 384 x 752
    const uint32_t lx = f.w - (uint32_t)(uig::CONT_Y + uig::CONT_H);    // 22
    const uint32_t ly = (uint32_t)uig::CONT_X;                          // 24
    const uint32_t SHIFT = 40;                                           // a 60 fps step at 2400 px/s
    uint8_t* tmp = (uint8_t*)heap_caps_aligned_alloc(64, (size_t)uig::CONT_W * uig::CONT_H * 3, MALLOC_CAP_SPIRAM);
    int64_t d_list = -9, d_full = -9, d_shift = -9, d_strip = -9, p_list0 = -9, p_rot = -9, p_strip = -9,
            par = -9, par_dma = -9;
    if (esp_async_fbcpy_install(&fcfg, &fc) == ESP_OK && ppa_register_client(&pcfg, &srm) == ESP_OK && tmp) {
        memset(tmp, 0x40, (size_t)uig::CONT_W * uig::CONT_H * 3);
        esp_cache_msync(tmp, (size_t)uig::CONT_W * uig::CONT_H * 3, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        uint8_t* const a = ppa_flush_double() ? ppa_flush_glass() : f.fb[0];
        uint8_t* const z = ppa_flush_double() ? idle : f.fb[1];
        d_list  = best_of_3([&] { return dma_copy(fc, a, z, f.w, f.h, lx, ly, lx, ly, lw, lh); });
        d_full  = best_of_3([&] { return dma_copy(fc, a, z, f.w, f.h, 0, 0, 0, 0, f.w, f.h); });
        d_shift = best_of_3([&] { return dma_copy(fc, a, z, f.w, f.h, lx + SHIFT, ly, lx, ly, lw - SHIFT, lh); });
        d_strip = best_of_3([&] { return dma_copy(fc, a, z, f.w, f.h, lx, ly, lx, ly, SHIFT, lh); });
        p_list0 = best_of_3([&] { return ppa_copy(srm, a, f.w, f.h, lx, ly, lw, lh, z, f.w, f.h, lx, ly,
                                                 PPA_SRM_ROTATION_ANGLE_0); });
        // what the flush does today for a whole list: LVGL's 752 x 384 turned into the window
        p_rot   = best_of_3([&] { return ppa_copy(srm, tmp, uig::CONT_W, uig::CONT_H, 0, 0, uig::CONT_W, uig::CONT_H,
                                                 z, f.w, f.h, lx, ly, PPA_SRM_ROTATION_ANGLE_270); });
        // and for the strip a shift leaves to draw
        p_strip = best_of_3([&] { return ppa_copy(srm, tmp, uig::CONT_W, SHIFT, 0, 0, uig::CONT_W, SHIFT,
                                                 z, f.w, f.h, lx + lw - SHIFT, ly, PPA_SRM_ROTATION_ANGLE_270); });
        // the shift and the strip at once, as a scrolling frame would have them
        par = best_of_3([&] {
            esp_async_fbcpy_trans_desc_t t = {};
            t.src_buffer = a; t.dst_buffer = z;
            t.src_buffer_size_x = f.w; t.src_buffer_size_y = f.h;
            t.dst_buffer_size_x = f.w; t.dst_buffer_size_y = f.h;
            t.src_offset_x = lx + SHIFT; t.src_offset_y = ly;
            t.dst_offset_x = lx; t.dst_offset_y = ly;
            t.copy_size_x = lw - SHIFT; t.copy_size_y = lh;
            t.pixel_format_unique_id.color_type_id = COLOR_TYPE_ID(COLOR_SPACE_RGB, COLOR_PIXEL_RGB888);
            xSemaphoreTake(s_blit_sem, 0);
            const int64_t t0 = esp_timer_get_time();
            if (esp_async_fbcpy(fc, &t, blit_done, nullptr) != ESP_OK) return (int64_t)-1;
            if (ppa_copy(srm, tmp, uig::CONT_W, SHIFT, 0, 0, uig::CONT_W, SHIFT, z, f.w, f.h,
                         lx + lw - SHIFT, ly, PPA_SRM_ROTATION_ANGLE_270) < 0) return (int64_t)-3;
            const int64_t t1 = esp_timer_get_time();
            if (xSemaphoreTake(s_blit_sem, pdMS_TO_TICKS(500)) != pdTRUE) return (int64_t)-2;
            const int64_t t2 = esp_timer_get_time();
            par_dma = t2 - t0;
            return (t2 > t1 ? t2 : t1) - t0;
        });
    }
    if (srm) ppa_unregister_client(srm);
    if (fc) esp_async_fbcpy_uninstall(fc);
    heap_caps_free(tmp);
    const double list_px = (double)lw * lh, full_px = (double)f.w * f.h;
    auto mpx = [](double px, int64_t us) { return us > 0 ? px / (double)us : 0.0; };
    snprintf(out, n,
             "{\"us\":{\"dma_list\":%lld,\"dma_full\":%lld,\"dma_shift\":%lld,\"dma_strip\":%lld,"
             "\"ppa_list_rot0\":%lld,\"ppa_list_rot270\":%lld,\"ppa_strip_rot270\":%lld,"
             "\"shift_with_strip\":%lld},"
             "\"mpx_per_s\":{\"dma_list\":%.1f,\"dma_full\":%.1f,\"ppa_list_rot0\":%.1f,\"ppa_list_rot270\":%.1f},"
             "\"window\":[%lu,%lu,%lu,%lu],\"shift\":%lu}",
             (long long)d_list, (long long)d_full, (long long)d_shift, (long long)d_strip,
             (long long)p_list0, (long long)p_rot, (long long)p_strip, (long long)par,
             mpx(list_px, d_list), mpx(full_px, d_full), mpx(list_px, p_list0), mpx(list_px, p_rot),
             (unsigned long)lx, (unsigned long)ly, (unsigned long)lw, (unsigned long)lh, (unsigned long)SHIFT);
    (void)par_dma;
}

using modesp::http_routes::send_json;

/// ?ms=6000 (1000..20000), ?v=1200 px/s (100..4000), ?screen=<id> to switch the page under the
/// list first — "set" is a quiet one, so the page's own refresh does not add to the frames.
/// ?keep=1 leaves the list up until the next run, for a screenshot. ?lines=N renders in blocks
/// of N full-width rows, ?ibuf=N (8 or more) into two N-row internal SRAM buffers, for that run
/// only. ?mem=1 times memory copies instead (mem_probe).
esp_err_t handle_bench(httpd_req_t* req) {
    int32_t ms = 6000, speed = 1200;
    bool keep = false;                  // ?keep=1: leave the list up for a screenshot, until the next run
    int  lines = 0;                     // ?lines=N: render in blocks of N full-width rows (0: as built)
    int  ibuf = 0;                      // ?ibuf=N: draw into two N-row internal SRAM buffers
    bool mem = false;                   // ?mem=1: time memory copies instead of a scroll
    bool blit = false;                  // ?blit=1: time DMA2D and PPA frame-buffer copies instead
    bool oneway = false;                // ?oneway=1: down only, so ?keep=1 leaves it mid-list
    bool ring = false;                  // ?ring=1: a touch ring over the list, moving with it
    bool overlay = false;               // ?overlay=1: a popup over the list, hidden as the scroll starts
    char screen[16] = "";
    char query[96];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char v[16];
        if (httpd_query_key_value(query, "ms", v, sizeof(v)) == ESP_OK) ms = atoi(v);
        if (httpd_query_key_value(query, "v", v, sizeof(v)) == ESP_OK)  speed = atoi(v);
        if (httpd_query_key_value(query, "screen", screen, sizeof(screen)) != ESP_OK) screen[0] = '\0';
        if (httpd_query_key_value(query, "keep", v, sizeof(v)) == ESP_OK) keep = atoi(v) != 0;
        if (httpd_query_key_value(query, "lines", v, sizeof(v)) == ESP_OK) lines = atoi(v);
        if (httpd_query_key_value(query, "ibuf", v, sizeof(v)) == ESP_OK) ibuf = atoi(v);
        if (httpd_query_key_value(query, "mem", v, sizeof(v)) == ESP_OK) mem = atoi(v) != 0;
        if (httpd_query_key_value(query, "blit", v, sizeof(v)) == ESP_OK) blit = atoi(v) != 0;
        if (httpd_query_key_value(query, "oneway", v, sizeof(v)) == ESP_OK) oneway = atoi(v) != 0;
        if (httpd_query_key_value(query, "ring", v, sizeof(v)) == ESP_OK) ring = atoi(v) != 0;
        if (httpd_query_key_value(query, "overlay", v, sizeof(v)) == ESP_OK) overlay = atoi(v) != 0;
    }
    if (ms < 1000) ms = 1000; else if (ms > 20000) ms = 20000;
    if (speed < 100) speed = 100; else if (speed > 4000) speed = 4000;

    if (!lvgl_port_lock(500)) return send_json(req, "503 Service Unavailable", "{\"error\":\"LVGL busy\"}");
    if (s_active.load()) {
        lvgl_port_unlock();
        return send_json(req, "409 Conflict", "{\"error\":\"a run is in progress\"}");
    }
    drop_ring();
    drop_overlay();
    if (s_host) { lv_obj_delete(s_host); s_host = nullptr; }   // the list a ?keep=1 run left up
    if (blit) {
        // Under the lock, and after LVGL's last block has left: nothing else uses the PPA.
        lv_display_t* d = lv_display_get_default();
        for (int i = 0; i < 100 && d && d->flushing; i++) vTaskDelay(1);
        static char probe[640];
        blit_probe(probe, sizeof(probe));
        lvgl_port_unlock();
        ESP_LOGW(TAG, "%s", probe);
        return send_json(req, "200 OK", probe);
    }
    if (mem) {
        // Under the lock: LVGL draws nothing meanwhile, so only the scan-out shares PSRAM.
        static char probe[384];
        mem_probe(probe, sizeof(probe));
        lvgl_port_unlock();
        ESP_LOGW(TAG, "%s", probe);
        return send_json(req, "200 OK", probe);
    }
    if (screen[0] && !ui_shell_go(screen)) {
        lvgl_port_unlock();
        return send_json(req, "404 Not Found", "{\"error\":\"unknown screen\"}");
    }
    uint32_t lines_used = 0;
    bool internal = false;
    if (ibuf > 0) {
        // Not under 8 rows: ibuf=4 reset the panel by watchdog — LVGL waiting on a flush that
        // never finished, cause not found — while PSRAM buffers of 4 rows ran.
        lines_used = ibuf >= 8 ? use_internal(ibuf) : 0;
        internal = lines_used > 0;
        if (!internal) {
            lvgl_port_unlock();
            return send_json(req, "507 Insufficient Storage",
                             "{\"error\":\"no internal SRAM for the buffers, or ibuf under 8\"}");
        }
    } else {
        lines_used = set_draw_lines(lines);
    }
    // On the active screen and in front, not on the top layer: LVGL draws only from the
    // topmost opaque object that covers an area down, but the top layer comes after the
    // screen whatever covers it — so a host up there had the page under it drawn first,
    // every frame, and timed two pages where a list page draws one.
    s_host = m3w::box(lv_screen_active(), uig::CONT_X, uig::CONT_Y, uig::CONT_W, uig::CONT_H);
    lv_obj_set_style_bg_color(s_host, m3::col(m3::theme().c.surface), 0);
    lv_obj_set_style_bg_opa(s_host, LV_OPA_COVER, 0);
    lv_obj_move_foreground(s_host);
    s_list.build(s_host, 0, 0, uig::CONT_W, uig::CONT_H, fill, nullptr, nullptr, nullptr);
    s_list.set_count(ENTRIES);
    if (overlay) {
        s_overlay = m3w::box(lv_screen_active(), uig::CONT_X + 200, uig::CONT_Y + 290, 300, 60);
        lv_obj_set_style_bg_color(s_overlay, lv_color_hex(0xFF00FF), 0);
        lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    }
    if (ring) {
        s_ring = m3w::box(lv_screen_active(), uig::CONT_X + 330, uig::CONT_Y + 160, 30, 30);
        lv_obj_set_style_radius(s_ring, 15, 0);
        lv_obj_set_style_border_width(s_ring, 2, 0);
        lv_obj_set_style_border_color(s_ring, lv_color_hex(0xFFB74D), 0);
        lv_obj_set_style_border_opa(s_ring, LV_OPA_70, 0);
        ppa_flush_floating(s_ring, true);
        s_ring_due = 0;
        s_ring_frames = 0;
    }

    const int64_t now = esp_timer_get_time();
    s_st = Stats{};
    s_st.speed   = speed;
    s_st.t_start = now + SETTLE_US;
    s_st.t_turn  = s_st.t_start + (int64_t)ms * (oneway ? 1000 : 500);
    s_st.t_end   = s_st.t_start + (int64_t)ms * 1000;
    s_st.last_step = s_st.t_start;
    s_done.store(false);
    s_main_gap_max.store(0, std::memory_order_relaxed);
    ppa_flush_stats(nullptr, true);
    s_refr_starts.store(0, std::memory_order_relaxed);
    // Building the list above held the lock, and with it the main loop: that wait is not the run's.
    s_run_t0.store(now, std::memory_order_relaxed);
    const uint32_t turns0 = s_main_turns.load(std::memory_order_relaxed);
    s_active.store(true, std::memory_order_release);
    lvgl_port_unlock();

    const int64_t deadline = s_st.t_end + 5000000;
    while (!s_done.load(std::memory_order_acquire) && esp_timer_get_time() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    const bool finished = s_done.load(std::memory_order_acquire);
    const uint32_t turns = s_main_turns.load(std::memory_order_relaxed) - turns0;
    const int64_t  ran_us = esp_timer_get_time() - (s_st.t_start - SETTLE_US);

    // Stopped under the lock, so the LVGL task is between frames and writes nothing more.
    int rows_shown = 0, rows_wrong = -1;
    if (lvgl_port_lock(2000)) {
        s_active.store(false, std::memory_order_release);
        check_rows(rows_shown, rows_wrong);
        if (internal) drop_internal(); else set_draw_lines(0);
        if (!keep) {
            drop_ring();
            drop_overlay();
            lv_obj_delete(s_host);
            s_host = nullptr;
        }
        lvgl_port_unlock();
    } else {
        s_active.store(false, std::memory_order_release);   // the host stays; later runs refuse
    }

    const Stats& s = s_st;
    const double secs = s.final_refr > s.first_refr ? (double)(s.final_refr - s.first_refr) / 1e6 : 0.0;
    PpaFlushStats fs = {};
    ppa_flush_stats(&fs, false);
    static char body[1600];
    snprintf(body, sizeof(body),
             "{\"finished\":%s,\"lines\":%lu,\"internal\":%s,\"refr_starts\":%lu,"
             "\"rows\":{\"shown\":%d,\"wrong\":%d},"
             "\"frames\":%lu,\"seconds\":%.2f,\"fps\":%.1f,"
             "\"px_per_s\":%ld,\"scrolled_px\":%lld,"
             "\"frame_ms\":{\"avg\":%.2f,\"p95\":%lu,\"max\":%.2f},"
             "\"interval_ms\":{\"avg\":%.2f,\"p95\":%lu,\"max\":%.2f},"
             "\"render_ms_avg\":%.2f,\"flush_ms_avg\":%.2f,\"wait_ms_avg\":%.2f,"
             "\"place\":{\"calls\":%lu,\"avg_ms\":%.3f,\"max_ms\":%.2f,\"per_frame_ms\":%.2f},"
             "\"main_loop\":{\"hz\":%.1f,\"max_gap_ms\":%.1f},\"ppa\":{\"refused\":%lu,\"split\":%lu,\"cpu\":%lu},"
             "\"compose\":{\"double\":%s,\"frames\":%lu,\"shifted\":%lu,\"dropped\":%lu,\"ineligible\":%lu,"
             "\"swap_late\":%lu,\"copy_fail\":%lu,\"wait_ms\":%.2f,\"sync_ms\":%.2f,\"shift_ms\":%.2f,"
             "\"block_wait_ms\":%.2f,\"overlaid\":%lu,"
             "\"why\":{\"hook_hit\":%lu,\"hook_miss\":%lu,\"covered\":%lu,\"not_seen\":%lu,\"too_soon\":%lu,"
             "\"moved\":%lu,\"too_far\":%lu,\"miss\":[%d,%d,%d,%d,%d,%d,%d,%d]}}}",
             finished ? "true" : "false", (unsigned long)lines_used, internal ? "true" : "false",
             (unsigned long)s_refr_starts.load(std::memory_order_relaxed), rows_shown, rows_wrong, (unsigned long)s.frame.n, secs,
             secs > 0.0 ? (double)s.interval.n / secs : 0.0,
             (long)s.speed, (long long)s.scrolled_px,
             s.frame.avg_ms(), (unsigned long)p95_ms(s.frame_hist, s.frame.n), s.frame.max / 1000.0,
             s.interval.avg_ms(), (unsigned long)p95_ms(s.interval_hist, s.interval.n),
             s.interval.max / 1000.0,
             s.render.avg_ms(), s.flush.avg_ms(), s.wait.avg_ms(),
             (unsigned long)s.place.n, s.place.avg_ms(), s.place.max / 1000.0,
             s.frame.n ? (double)s.place.sum / (double)s.frame.n / 1000.0 : 0.0,
             ran_us > 0 ? (double)turns * 1e6 / (double)ran_us : 0.0,
             s_main_gap_max.load(std::memory_order_relaxed) / 1000.0,
             (unsigned long)ppa_flush_refused(), (unsigned long)ppa_flush_split(),
             (unsigned long)ppa_flush_cpu(),
             ppa_flush_double() ? "true" : "false", (unsigned long)fs.frames, (unsigned long)fs.shifted,
             (unsigned long)fs.dropped, (unsigned long)fs.ineligible, (unsigned long)fs.swap_late,
             (unsigned long)fs.copy_fail,
             fs.frames ? fs.wait_us / 1000.0 / fs.frames : 0.0,
             fs.frames ? fs.sync_us / 1000.0 / fs.frames : 0.0,
             fs.shifted ? fs.shift_us / 1000.0 / fs.shifted : 0.0,
             fs.frames ? fs.block_wait_us / 1000.0 / fs.frames : 0.0, (unsigned long)fs.overlaid,
             (unsigned long)fs.hook_hit, (unsigned long)fs.hook_miss, (unsigned long)fs.covered,
             (unsigned long)fs.not_seen, (unsigned long)fs.too_soon, (unsigned long)fs.moved,
             (unsigned long)fs.too_far, fs.miss_hook[0], fs.miss_hook[1], fs.miss_hook[2], fs.miss_hook[3],
             fs.miss_got[0], fs.miss_got[1], fs.miss_got[2], fs.miss_got[3]);
    ESP_LOGW(TAG, "%s", body);
    return send_json(req, "200 OK", body);
}

void register_route(httpd_handle_t server, void*) {
    httpd_uri_t uri{};
    uri.uri = "/api/ui/scrollbench";
    uri.method = HTTP_GET;
    uri.handler = handle_bench;
    if (httpd_register_uri_handler(server, &uri) != ESP_OK) {
        ESP_LOGW(TAG, "/api/ui/scrollbench not registered");
        return;
    }
    ESP_LOGW(TAG, "/api/ui/scrollbench answers WITHOUT login (CONFIG_UI_SCROLL_BENCH) — "
                  "development build");
}

} // namespace

void ui_scroll_bench_init() {
    if (!lvgl_port_lock(500)) return;
    if (lv_display_t* d = lv_display_get_default()) {
        static const lv_event_code_t codes[] = {
            LV_EVENT_REFR_START, LV_EVENT_REFR_READY, LV_EVENT_FLUSH_START,
            LV_EVENT_FLUSH_FINISH, LV_EVENT_FLUSH_WAIT_START, LV_EVENT_FLUSH_WAIT_FINISH,
        };
        for (lv_event_code_t c : codes) lv_display_add_event_cb(d, disp_cb, c, nullptr);
    }
    lvgl_port_unlock();
    if (!modesp::http_routes::add(register_route)) {
        ESP_LOGW(TAG, "no room in the route table — /api/ui/scrollbench will 404");
    }
}

void ui_scroll_bench_note_place(uint32_t us) {
    if (!s_active.load(std::memory_order_relaxed) || !s_st.timing) return;
    s_st.place_us += us;
    s_st.place.add(us);
}

void ui_scroll_bench_note_main_turn() {
    const int64_t now = esp_timer_get_time();
    if (s_active.load(std::memory_order_relaxed) &&
        s_main_last >= s_run_t0.load(std::memory_order_relaxed)) {
        const uint32_t gap = (uint32_t)(now - s_main_last);
        if (gap > s_main_gap_max.load(std::memory_order_relaxed))
            s_main_gap_max.store(gap, std::memory_order_relaxed);
    }
    s_main_last = now;
    s_main_turns.fetch_add(1, std::memory_order_relaxed);
}

#else

void ui_scroll_bench_init() {}
void ui_scroll_bench_note_place(uint32_t) {}
void ui_scroll_bench_note_main_turn() {}

#endif // CONFIG_UI_SCROLL_BENCH
