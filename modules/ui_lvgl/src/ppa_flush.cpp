/**
 * @file ppa_flush.cpp
 * @brief The flush for a rotated DSI panel: the PPA turns each block straight into a frame
 *        buffer, LVGL draws the next block while it does, and with two frame buffers a frame is
 *        composed off the glass and a scrolling list is shifted rather than drawn again.
 *
 * esp_lvgl_port's own flush (sw_rotate with CONFIG_LVGL_PORT_ENABLE_PPA) rotates a block into a
 * buffer of its own with the PPA in blocking mode, then hands that buffer to the DPI driver,
 * which copies it into the frame buffer with DMA2D. Every pixel crossed PSRAM twice more than it
 * had to, and the LVGL task sat out the rotation of each block before it could draw the next:
 * ~21 ms of every ~88 ms frame of a scrolling list (/api/ui/scrollbench, 05.10.2026).
 *
 * Here the PPA writes each block at its own place in a DPI frame buffer (out.pic is the whole
 * panel, out.block_offset where the block lands), the call returns at once (NON_BLOCKING), and
 * the PPA's done interrupt tells LVGL the draw buffer is free again. LVGL has two draw buffers,
 * so it renders the next block into the other one while the PPA works on this one.
 *
 * The PPA driver writes the draw buffer's lines back from the cache before it reads them and
 * invalidates the frame buffer window it writes (ppa_srm.c), so the PPA path needs no sync here.
 *
 * Some rotated blocks must never reach the PPA whole. On this silicon (v1.3) a transfer whose
 * last unit is smaller than the DMA2D FIFO never raises its done interrupt unless the
 * macro-block order is bypassed, and IDF 5.5.5 decides the bypass from the block unrotated
 * (esp-idf#19023, fixed on master by 469aa16, a v5.5 backport promised). One flushed block in
 * eight is such a block. They go as stripes that are each safe (include/ppa_geometry.h, pinned
 * by tests/host/test_ppa_geometry.cpp), in blocking mode; the few no stripe can save — narrower
 * than one transfer unit — are rotated on the CPU.
 *
 * Two frame buffers (CONFIG_UI_DOUBLE_FB, the DPI panel made with num_fbs = 2). Blocks go into
 * the buffer that is not on the glass; once a frame's last block is in, the DPI driver is told
 * to scan that buffer instead: draw_bitmap with a pointer into one of its frame buffers only
 * changes cur_fb_index (esp_lcd_panel_dpi.c), and the DMA takes it up when it restarts at the end
 * of the frame being scanned, which it reports through on_frame_buf_complete. Nothing is ever
 * written into the buffer being scanned, so nothing tears. The other buffer is then a frame
 * behind: before the next frame's first block, whatever the last frame wrote is copied across
 * by DMA2D (esp_async_fbcpy, esp_lcd's own frame-buffer copy: ~54 Mpx/s against the PPA's ~21,
 * /api/ui/scrollbench?blit=1, 05.10.2026).
 *
 * A list that scrolls is shifted, not drawn again. LVGL repaints a scroller whole after every
 * scroll (lv_obj_scroll_by_raw: the children move, LV_EVENT_SCROLL, then lv_obj_invalidate), and
 * repainting 752 x 384 pixels of text costs ~70 ms here however it is drawn. VList reports each
 * scroll instead (ppa_flush_list_scrolled): the list's window, exactly as the glass shows it, is
 * copied into the other buffer moved by the distance scrolled (~5 ms of DMA2D), and the one
 * invalidation that follows is replaced on its way in (LV_EVENT_INVALIDATE_AREA lets a handler
 * change the area) by the strip that came into view. The scrollbar's band is drawn again too,
 * since the bar does not move with the content. Anything else that changes inside the window is
 * invalidated as usual and drawn after the shift. A shift is taken only when the copy is exact:
 * one plain colour behind the list, nothing drawn over its window, and the glass holding the
 * window as the list last left it; otherwise the window is simply drawn again, as before.
 */

#include "ppa_flush.h"
#include "ppa_geometry.h"
#include "driver/ppa.h"
#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"
// LVGL 9.3 keeps these in its private headers: area arithmetic, an object's extra draw size, and
// lv_inv_area — what a shift needs to compute and invalidate exactly what lv_obj_invalidate does.
#include "src/core/lv_obj_draw_private.h"
#include "src/core/lv_refr_private.h"
#include "src/misc/lv_area_private.h"
#include <atomic>

extern "C" {                   // esp_lcd's private header has no C++ guard of its own
#include "esp_async_fbcpy.h"   // esp_lcd's DMA2D 2D copy; see CMakeLists.txt
}

static const char* TAG = "UiFlush";

namespace {

constexpr int      MAX_STRIPES = 16;
constexpr uint32_t LINE = CONFIG_CACHE_L2_CACHE_LINE_SIZE;   // ≥ the L1 line on this chip
constexpr int32_t  BAND_W = 16;       ///< logical px at a list's right edge: its scrollbar, drawn again on a shift
constexpr int64_t  SWAP_GUARD_US = 300;   ///< a frame end this soon after a swap request may have read the old index
constexpr int      MAX_DAMAGE = 24;

/// A rectangle of the frame buffer, inclusive.
struct Rect {
    int32_t x1, y1, x2, y2;
};

bool inside(const Rect& a, const Rect& b) {
    return a.x1 >= b.x1 && a.y1 >= b.y1 && a.x2 <= b.x2 && a.y2 <= b.y2;
}

/// What a frame wrote into its buffer: what the other buffer lacks once it is on the glass.
struct Damage {
    Rect r[MAX_DAMAGE];
    int  n = 0;
    void clear() { n = 0; }
    void add(const Rect& a) {
        for (int i = 0; i < n; i++) if (inside(a, r[i])) return;
        if (n < MAX_DAMAGE) { r[n++] = a; return; }
        Rect b = a;                       // full: one box around everything
        for (int i = 0; i < n; i++) {
            if (r[i].x1 < b.x1) b.x1 = r[i].x1;
            if (r[i].y1 < b.y1) b.y1 = r[i].y1;
            if (r[i].x2 > b.x2) b.x2 = r[i].x2;
            if (r[i].y2 > b.y2) b.y2 = r[i].y2;
        }
        r[0] = b;
        n = 1;
    }
};

struct Flush {
    esp_lcd_panel_handle_t panel = nullptr;
    ppa_client_handle_t   srm = nullptr;
    uint8_t*              fb = nullptr;               ///< single-buffered: the one on the glass
    uint32_t              fb_size = 0;
    uint32_t              panel_w = 0, panel_h = 0;   ///< the frame buffer's own, unrotated
    ppa_srm_color_mode_t  in_cm = PPA_SRM_COLOR_MODE_RGB888;
    uint32_t              in_bpp = 3;                 ///< bytes per pixel of LVGL's draw buffer
    std::atomic<uint32_t> refused{0}, split{0}, cpu{0};

    // two frame buffers
    bool                  dbl = false;
    uint8_t*              fbs[2] = {nullptr, nullptr};
    int                   back = 1;                   ///< frames are composed here; fbs[back ^ 1] is on the glass
    volatile int          shown = 0;                  ///< the last complete frame's buffer: on the glass, or about to be
    bool                  flip = false;               ///< the last frame asked for a swap
    bool                  in_frame = false;           ///< this frame's first block has gone through begin_frame
    volatile bool         swap_pending = false;       ///< asked for, not yet on the glass (ISR clears)
    volatile int64_t      swap_t = 0;
    volatile uint32_t     presented = 0;              ///< frames that reached the glass (ISR counts)
    uint32_t              ended = 0;                  ///< frames composed; they land in this order
    SemaphoreHandle_t     swap_sem = nullptr;
    esp_async_fbcpy_handle_t fbcpy = nullptr;
    SemaphoreHandle_t     copy_sem = nullptr;
    Damage                last;                       ///< written by the frame now on the glass
    Damage                now;                        ///< written by the frame being composed
    PpaFlushStats         st = {};

    // A frame's copies, worked out by the LVGL task as rendering starts and done by the
    // compositor task while LVGL draws: wait for the last swap, catch up, shift.
    struct Plan {
        bool  flip = false;                           ///< wait for the swap asked for, then compose in the other buffer
        Rect  sync[MAX_DAMAGE];                       ///< what the last frame wrote, to copy across
        int   n_sync = 0;
        bool  shift = false;
        Rect  from = {};                              ///< the window's part to keep, in the glass...
        int32_t mx = 0, my = 0;                       ///< ...and where it goes in the buffer behind
    } plan;
    TaskHandle_t          task = nullptr;
    TaskHandle_t          lvgl = nullptr;             ///< LVGL's task, as seen from its own callbacks
    UBaseType_t           lvgl_prio = 0;              ///< its priority as the port made it
    SemaphoreHandle_t     plan_sem = nullptr;         ///< LVGL -> task: a plan is ready
    SemaphoreHandle_t     done_sem = nullptr;         ///< task -> LVGL: its copies are done
    bool                  planned = false;            ///< a plan was given and its done not yet taken
    volatile bool         shift_failed = false;       ///< the task could not shift: the window must be drawn
    bool                  redraw_all = false;         ///< ...so the next frame draws the screen whole (LVGL task only)
};
Flush s_flush;

/// A pending shift of a scrolled list, and what the glass is known to show.
struct Shift {
    bool      armed = false;      ///< the next invalidation is LVGL's repaint of the scroller...
    lv_area_t hook = {};          ///< ...of exactly this area...
    lv_area_t repl = {};          ///< ...and becomes this one
    bool      active = false;     ///< the next frame shifts `win` by `dy`
    lv_area_t win = {};           ///< logical: the list's window on the screen
    int32_t   dy = 0;             ///< logical: how far its content moved down since the glass's frame
    lv_obj_t* watch = nullptr;    ///< the list last scrolled or filled: checked for overlays every frame
    const lv_obj_t* seen = nullptr;   ///< the list whose window the glass is known to show...
    lv_area_t seen_win = {};
    uint32_t  seen_at = 0;        ///< ...once a frame composed after this many is on the glass
    uint32_t  clear_from = 0;     ///< no shift until this many frames are on the glass: something was over a window
};
Shift s_shift;

bool same(const lv_area_t& a, const lv_area_t& b) {
    return a.x1 == b.x1 && a.y1 == b.y1 && a.x2 == b.x2 && a.y2 == b.y2;
}

/// The PPA's done interrupt. Not IRAM-safe, and need not be: with CONFIG_PPA_ISR_IRAM_SAFE off
/// the interrupt is held while the cache is off, and LVGL simply waits a little longer.
bool ppa_done(ppa_client_handle_t, ppa_event_data_t*, void* user) {
    lv_display_flush_ready(static_cast<lv_display_t*>(user));
    return false;
}

/// The DPI driver finished scanning a frame and restarted on cur_fb_index. The request is made
/// only once draw_bitmap has stored the new index (end_frame), so a frame end before the store
/// finds none pending; the driver reads the index first and calls this after, so one that read
/// the old index just before the store (from the other core) and calls this just after the
/// request is turned away by SWAP_GUARD_US. Either way the swap is reported a frame later, never
/// early. IRAM: the DSI interrupt is IRAM-safe here (CONFIG_LCD_DSI_ISR_IRAM_SAFE), so it may run
/// with the cache off.
IRAM_ATTR bool frame_done(esp_lcd_panel_handle_t, esp_lcd_dpi_panel_event_data_t*, void*) {
    if (!s_flush.swap_pending) return false;
    std::atomic_thread_fence(std::memory_order_acquire);
    if (esp_timer_get_time() - s_flush.swap_t < SWAP_GUARD_US) return false;
    s_flush.swap_pending = false;
    s_flush.presented = s_flush.presented + 1;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_flush.swap_sem, &woken);
    return woken == pdTRUE;
}

IRAM_ATTR bool copy_done(esp_async_fbcpy_handle_t, esp_async_fbcpy_event_data_t*, void*) {
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_flush.copy_sem, &woken);
    return woken == pdTRUE;
}

/// Where logical pixel (lx, ly) lands in the frame buffer. esp_lvgl_port's own block arithmetic
/// (common/ppa/lcd_ppa.c), which is what drew this panel correctly, taken to a single pixel.
inline void to_panel(lv_display_rotation_t rot, int32_t hres, int32_t vres,
                     int32_t lx, int32_t ly, int32_t& px, int32_t& py) {
    switch (rot) {
    case LV_DISPLAY_ROTATION_90:  px = ly;            py = hres - 1 - lx; break;
    case LV_DISPLAY_ROTATION_180: px = hres - 1 - lx; py = vres - 1 - ly; break;
    case LV_DISPLAY_ROTATION_270: px = vres - 1 - ly; py = lx;            break;
    default:                      px = lx;            py = ly;            break;
    }
}

/// A logical area as the frame buffer's rectangle.
Rect to_fb(lv_display_t* disp, const lv_area_t& a) {
    const lv_display_rotation_t rot = lv_display_get_rotation(disp);
    const int32_t hres = lv_display_get_horizontal_resolution(disp);
    const int32_t vres = lv_display_get_vertical_resolution(disp);
    int32_t ax, ay, bx, by;
    to_panel(rot, hres, vres, a.x1, a.y1, ax, ay);
    to_panel(rot, hres, vres, a.x2, a.y2, bx, by);
    return Rect{ax < bx ? ax : bx, ay < by ? ay : by, ax < bx ? bx : ax, ay < by ? by : ay};
}

/// Copy `r` of frame buffer `src` to `dst`, moved by (mx, my). Waits for the DMA2D.
bool fb_copy(const uint8_t* src, uint8_t* dst, const Rect& r, int32_t mx, int32_t my) {
    esp_async_fbcpy_trans_desc_t t = {};
    t.src_buffer = src;
    t.dst_buffer = dst;
    t.src_buffer_size_x = s_flush.panel_w;  t.src_buffer_size_y = s_flush.panel_h;
    t.dst_buffer_size_x = s_flush.panel_w;  t.dst_buffer_size_y = s_flush.panel_h;
    t.src_offset_x = (uint32_t)r.x1;        t.src_offset_y = (uint32_t)r.y1;
    t.dst_offset_x = (uint32_t)(r.x1 + mx); t.dst_offset_y = (uint32_t)(r.y1 + my);
    t.copy_size_x = (uint32_t)(r.x2 - r.x1 + 1);
    t.copy_size_y = (uint32_t)(r.y2 - r.y1 + 1);
    t.pixel_format_unique_id.color_type_id = COLOR_TYPE_ID(COLOR_SPACE_RGB, COLOR_PIXEL_RGB888);
    xSemaphoreTake(s_flush.copy_sem, 0);
    if (esp_async_fbcpy(s_flush.fbcpy, &t, copy_done, nullptr) != ESP_OK ||
        xSemaphoreTake(s_flush.copy_sem, pdMS_TO_TICKS(50)) != pdTRUE) {
        s_flush.st.copy_fail++;
        return false;
    }
    return true;
}

/// Small objects that float over everything and may sit over a list while it scrolls — the touch
/// ring follows the finger that scrolls it. They do not stop a shift: where each was on the glass
/// and where it is now are drawn again instead (ppa_flush_list_scrolled).
constexpr int MAX_FLOATING = 4;
struct Floating {
    lv_obj_t* obj = nullptr;
    bool      shown = false;      ///< in the last frame composed...
    lv_area_t at = {};            ///< ...there
};
Floating s_float[MAX_FLOATING];

bool floating(const lv_obj_t* o) {
    for (const Floating& f : s_float) if (f.obj == o) return true;
    return false;
}

/// A registered object is being deleted: its slot goes with it. A palette or language change
/// rebuilds the whole tree (lv_obj_clean in rebuild_look) and deletes the touch ring without
/// anyone telling the flush, which reads every registered object on every frame.
void floating_deleted(lv_event_t* e) {
    const lv_obj_t* o = lv_event_get_target_obj(e);
    for (Floating& f : s_float) if (f.obj == o) f = Floating{};
}

/// Where each floating object is as this frame is drawn: after it, the glass.
void note_floating() {
    for (Floating& f : s_float) {
        if (!f.obj) continue;
        f.shown = !lv_obj_has_flag(f.obj, LV_OBJ_FLAG_HIDDEN);
        if (f.shown) {
            lv_obj_get_coords(f.obj, &f.at);
            const int32_t ext = lv_obj_get_ext_draw_size(f.obj);
            lv_area_increase(&f.at, ext, ext);
        }
    }
}

/// Any visible object over `win` that `a` is not part of: later siblings up the tree, and the
/// top and system layers, which LVGL draws after the screen. Floating objects are not counted.
bool covers(const lv_obj_t* o, const lv_area_t& win) {
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) || floating(o)) return false;
    lv_area_t c;
    lv_obj_get_coords(o, &c);
    const int32_t ext = lv_obj_get_ext_draw_size(o);
    lv_area_increase(&c, ext, ext);
    lv_area_t x;
    return lv_area_intersect(&x, &c, &win);
}

bool layers_over(lv_display_t* d, const lv_area_t& win) {
    lv_obj_t* layers[] = {lv_display_get_layer_top(d), lv_display_get_layer_sys(d)};
    for (lv_obj_t* l : layers) {
        if (!l) continue;
        const uint32_t n = lv_obj_get_child_count(l);
        for (uint32_t i = 0; i < n; i++)
            if (covers(lv_obj_get_child(l, (int32_t)i), win)) return true;
    }
    return false;
}

bool overlay_over(const lv_obj_t* a, const lv_area_t& win) {
    for (const lv_obj_t* o = a; lv_obj_get_parent(o); o = lv_obj_get_parent(o)) {
        const lv_obj_t* p = lv_obj_get_parent(o);
        const uint32_t n = lv_obj_get_child_count(p);
        for (uint32_t i = (uint32_t)lv_obj_get_index(o) + 1; i < n; i++)
            if (covers(lv_obj_get_child(p, (int32_t)i), win)) return true;
    }
    return layers_over(lv_obj_get_display(a), win);
}

/// Whether a shifted copy of the list's window is the window scrolled: everything behind the
/// content one plain colour that stays where it is. Walks up to the first opaque background; it
/// must be a flat colour, and nothing on the way may draw at a fixed place over the window.
bool plain_backdrop(const lv_obj_t* scroller) {
    for (const lv_obj_t* o = scroller; o; o = lv_obj_get_parent(o)) {
        if (lv_obj_get_style_opa(o, LV_PART_MAIN) < LV_OPA_MAX) return false;     // a layer
        if (lv_obj_get_style_border_width(o, LV_PART_MAIN) > 0) return false;
        if (lv_obj_get_style_radius(o, LV_PART_MAIN) > 0) return false;           // corners would move
        const lv_opa_t bo = lv_obj_get_style_bg_opa(o, LV_PART_MAIN);
        if (bo <= LV_OPA_MIN) continue;
        if (bo < LV_OPA_MAX) return false;
        if (lv_obj_get_style_bg_grad_dir(o, LV_PART_MAIN) != LV_GRAD_DIR_NONE) return false;
        if (lv_obj_get_style_bg_image_src(o, LV_PART_MAIN)) return false;
        return true;
    }
    return false;
}

/// The area LVGL repaints for `scroller`, as lv_obj_invalidate works it out: its coordinates,
/// clipped by its parents and the screen. False when nothing of it shows, or when it draws
/// outside itself (a shift copies only what is inside).
bool window_of(lv_obj_t* scroller, lv_area_t* win) {
    lv_display_t* disp = lv_obj_get_display(scroller);
    lv_obj_get_coords(scroller, win);
    const lv_area_t screen = {0, 0, lv_display_get_horizontal_resolution(disp) - 1,
                              lv_display_get_vertical_resolution(disp) - 1};
    return lv_obj_get_ext_draw_size(scroller) == 0 && lv_obj_area_is_visible(scroller, win) &&
           lv_area_intersect(win, win, &screen);
}

/// The watched list is being deleted (a page or the whole tree rebuilt).
void watch_deleted(lv_event_t* e) {
    if (s_shift.watch == lv_event_get_target_obj(e)) s_shift.watch = nullptr;
}

/// Watch `scroller` from now on: every frame checks whether anything lies over its window, so a
/// popup that was on the glass but is gone by the time the list scrolls still holds the shift
/// off until a frame without it is there — its pixels would travel with the shift otherwise.
void watch_list(lv_obj_t* scroller) {
    if (s_shift.watch == scroller) return;
    if (s_shift.watch) lv_obj_remove_event_cb(s_shift.watch, watch_deleted);
    s_shift.watch = scroller;
    lv_obj_add_event_cb(scroller, watch_deleted, LV_EVENT_DELETE, nullptr);
}

void drop_shift() {
    if (s_shift.active || s_shift.armed) s_flush.st.dropped++;
    s_shift.active = false;
    s_shift.armed = false;
    s_shift.dy = 0;
}

/// LV_EVENT_INVALIDATE_AREA: LVGL's repaint of a scroller that was just shifted becomes the
/// strip that came into view. Any other area that covers a pending shift's window means the
/// window is drawn whole this frame anyway, and the shift is dropped.
void inv_cb(lv_event_t* e) {
    lv_area_t* a = static_cast<lv_area_t*>(lv_event_get_param(e));
    if (!a) return;
    if (s_shift.armed) {
        s_shift.armed = false;
        if (same(*a, s_shift.hook)) {
            *a = s_shift.repl;
            s_flush.st.hook_hit++;
            return;
        }
        s_flush.st.hook_miss++;
        const lv_area_t& k = s_shift.hook;
        s_flush.st.miss_hook[0] = (int16_t)k.x1; s_flush.st.miss_hook[1] = (int16_t)k.y1;
        s_flush.st.miss_hook[2] = (int16_t)k.x2; s_flush.st.miss_hook[3] = (int16_t)k.y2;
        s_flush.st.miss_got[0] = (int16_t)a->x1; s_flush.st.miss_got[1] = (int16_t)a->y1;
        s_flush.st.miss_got[2] = (int16_t)a->x2; s_flush.st.miss_got[3] = (int16_t)a->y2;
    }
    if (s_shift.active && lv_area_is_in(&s_shift.win, a, 0)) {
        s_flush.st.covered++;
        drop_shift();
    }
}

/// What the next frame's copies are, worked out as LVGL starts rendering it (LV_EVENT_RENDER_START:
/// after the scroll events and the layout, before the first block). The compositor task does
/// them while LVGL draws; the first block waits for them (begin_frame).
void plan_frame(lv_display_t* disp) {
    // A plan whose frame drew nothing has still been done; its done is taken before the next.
    if (s_flush.planned) {
        xSemaphoreTake(s_flush.done_sem, pdMS_TO_TICKS(100));
        s_flush.planned = false;
    }
    Flush::Plan& p = s_flush.plan;
    p.flip = s_flush.flip;
    s_flush.flip = false;

    // Something over the window the glass shows: its pixels would travel with a shift.
    // Checked every frame, not only when the list scrolls: something over the window in this
    // frame is in the glass that a shift two frames on would copy.
    if (s_shift.watch) {
        lv_area_t w;
        if (window_of(s_shift.watch, &w) && overlay_over(s_shift.watch, w)) {
            s_shift.clear_from = s_flush.ended + 2;
            s_flush.st.overlaid++;
        }
    }
    note_floating();

    p.shift = s_shift.active;
    Rect win{};
    if (p.shift) win = to_fb(disp, s_shift.win);
    p.n_sync = 0;
    for (int i = 0; i < s_flush.last.n; i++) {
        const Rect& r = s_flush.last.r[i];
        if (p.shift && inside(r, win)) continue;     // the shift and this frame's strip cover it
        p.sync[p.n_sync++] = r;
    }
    s_flush.last.clear();

    if (p.shift) {
        // The content's move in the frame buffer: any logical pixel's, moved down by dy.
        const lv_display_rotation_t rot = lv_display_get_rotation(disp);
        const int32_t hres = lv_display_get_horizontal_resolution(disp);
        const int32_t vres = lv_display_get_vertical_resolution(disp);
        int32_t ax, ay, bx, by;
        to_panel(rot, hres, vres, s_shift.win.x1, s_shift.win.y1, ax, ay);
        to_panel(rot, hres, vres, s_shift.win.x1, s_shift.win.y1 + s_shift.dy, bx, by);
        p.mx = bx - ax;
        p.my = by - ay;
        // The part of the window that still has content to keep, where it comes from.
        p.from = win;
        if (p.mx > 0) p.from.x2 -= p.mx; else p.from.x1 -= p.mx;
        if (p.my > 0) p.from.y2 -= p.my; else p.from.y1 -= p.my;
        if (p.from.x1 > p.from.x2 || p.from.y1 > p.from.y2) p.shift = false;
        s_flush.now.add(win);
        s_shift.active = false;
        s_shift.dy = 0;
    }
    s_flush.planned = true;
    xSemaphoreGive(s_flush.plan_sem);

#if CONFIG_UI_SHIFT_PRIORITY > 0
    // A frame that shifts a list draws only a strip — a few ms of CPU — so for those frames LVGL
    // goes above the tasks it otherwise shares its core with, the radio's decoder and the main
    // loop, and a scroll keeps the panel's pace under load. Every other frame, a page switch
    // drawn whole above all, goes back to the port's priority, time-sliced with them as before.
    if (!s_flush.lvgl) {
        s_flush.lvgl = xTaskGetCurrentTaskHandle();
        s_flush.lvgl_prio = uxTaskPriorityGet(s_flush.lvgl);
    }
    const UBaseType_t want = p.shift && CONFIG_UI_SHIFT_PRIORITY > s_flush.lvgl_prio
                                 ? (UBaseType_t)CONFIG_UI_SHIFT_PRIORITY : s_flush.lvgl_prio;
    if (uxTaskPriorityGet(s_flush.lvgl) != want) vTaskPrioritySet(s_flush.lvgl, want);
#endif
}

void render_start_cb(lv_event_t* e) {
    plan_frame(static_cast<lv_display_t*>(lv_event_get_current_target(e)));
}

/// The compositor task: a plan's copies, by DMA2D, in order. It sleeps on the DMA meanwhile, so
/// it takes next to no time from the core it shares with LVGL.
void compose_task(void*) {
    for (;;) {
        xSemaphoreTake(s_flush.plan_sem, portMAX_DELAY);
        const Flush::Plan& p = s_flush.plan;
        const int64_t t0 = esp_timer_get_time();
        if (p.flip) {
            if (xSemaphoreTake(s_flush.swap_sem, pdMS_TO_TICKS(40)) != pdTRUE) {
                // Never reported: carry on rather than stop the screen. At worst a frame tears.
                s_flush.swap_pending = false;
                s_flush.st.swap_late++;
            }
            s_flush.back ^= 1;
        }
        const int64_t t1 = esp_timer_get_time();
        uint8_t* const glass = s_flush.fbs[s_flush.back ^ 1];
        uint8_t* const back  = s_flush.fbs[s_flush.back];
        for (int i = 0; i < p.n_sync; i++) fb_copy(glass, back, p.sync[i], 0, 0);
        const int64_t t2 = esp_timer_get_time();
        if (p.shift) {
            if (fb_copy(glass, back, p.from, p.mx, p.my)) s_flush.st.shifted++;
            else s_flush.shift_failed = true;
        }
        s_flush.st.wait_us  += (uint32_t)(t1 - t0);
        s_flush.st.sync_us  += (uint32_t)(t2 - t1);
        s_flush.st.shift_us += (uint32_t)(esp_timer_get_time() - t2);
        xSemaphoreGive(s_flush.done_sem);
    }
}

/// The first block of a frame: its plan's copies must be done before anything is written behind.
void begin_frame(lv_display_t* disp) {
    s_flush.in_frame = true;
    if (!s_flush.planned) plan_frame(disp);         // no RENDER_START seen: plan now
    const int64_t t0 = esp_timer_get_time();
    if (xSemaphoreTake(s_flush.done_sem, pdMS_TO_TICKS(100)) != pdTRUE) s_flush.st.copy_fail++;
    s_flush.planned = false;
    s_flush.st.block_wait_us += (uint32_t)(esp_timer_get_time() - t0);
    if (s_flush.shift_failed) {
        // Without the shift the window keeps the old frame's content: draw it whole next time.
        // Not from here: LVGL is rendering, and an invalidation now is its assert (while(1) on
        // this task). refr_ready_cb does it once the frame is done.
        s_flush.shift_failed = false;
        s_flush.redraw_all = true;
    }
}

/// LV_EVENT_REFR_READY: the refresh is over, rendering_in_progress is down and the frame's
/// areas are spent, so an invalidation from here is the next frame's. (RENDER_READY is still
/// inside the render.)
void refr_ready_cb(lv_event_t* e) {
    if (!s_flush.redraw_all) return;
    s_flush.redraw_all = false;
    auto* d = static_cast<lv_display_t*>(lv_event_get_current_target(e));
    lv_obj_invalidate(lv_display_get_screen_active(d));
}

/// The frame's last block is in: the DPI driver scans this buffer from its next frame.
void end_frame() {
    // A pointer into one of the panel's own frame buffers: the driver only switches to it.
    esp_lcd_panel_draw_bitmap(s_flush.panel, 0, 0, 1, 1, s_flush.fbs[s_flush.back]);
    // The swap is asked for only once the index is stored. Asked for before, a wait inside
    // draw_bitmap longer than SWAP_GUARD_US — its cache sync takes the chunked-msync mutex
    // (CONFIG_ESP_MM_CACHE_MSYNC_C2M_CHUNKED_OPS), which the audio task holds for up to ~0.4 ms, or
    // a task above LVGL's priority runs — let a frame end that restarted on the old buffer report
    // the swap, and the compositor wrote into the buffer on the glass: a torn frame.
    s_flush.swap_t = esp_timer_get_time();
    std::atomic_thread_fence(std::memory_order_release);
    s_flush.swap_pending = true;
    s_flush.shown = s_flush.back;
    s_flush.flip = true;
    s_flush.last = s_flush.now;
    s_flush.now.clear();
    s_flush.in_frame = false;
    s_flush.ended++;
    s_flush.st.frames++;
}

/// Rows `row0`..`row0 + rows - 1` of the block (logical area `a`, `w` x `h`) through the PPA.
esp_err_t submit(lv_display_t* disp, uint8_t* px, const lv_area_t* a, int32_t w, int32_t h,
                 int32_t row0, int32_t rows, lv_display_rotation_t rot, ppa_trans_mode_t mode,
                 uint8_t* fb) {
    const int32_t hres = lv_display_get_horizontal_resolution(disp);   // logical: rotated
    const int32_t vres = lv_display_get_vertical_resolution(disp);
    // The stripe's corners on the panel; its top-left there is the smaller of each.
    int32_t ax, ay, bx, by;
    to_panel(rot, hres, vres, a->x1, a->y1 + row0, ax, ay);
    to_panel(rot, hres, vres, a->x2, a->y1 + row0 + rows - 1, bx, by);

    ppa_srm_oper_config_t op = {};
    op.in.buffer   = px;
    op.in.pic_w    = (uint32_t)w;
    op.in.pic_h    = (uint32_t)h;
    op.in.block_w  = (uint32_t)w;
    op.in.block_h  = (uint32_t)rows;
    op.in.block_offset_y = (uint32_t)row0;
    op.in.srm_cm   = s_flush.in_cm;
    op.out.buffer      = fb;
    op.out.buffer_size = s_flush.fb_size;
    op.out.pic_w       = s_flush.panel_w;
    op.out.pic_h       = s_flush.panel_h;
    op.out.block_offset_x = (uint32_t)(ax < bx ? ax : bx);
    op.out.block_offset_y = (uint32_t)(ay < by ? ay : by);
    op.out.srm_cm      = PPA_SRM_COLOR_MODE_RGB888;
    op.rotation_angle  = static_cast<ppa_srm_rotation_angle_t>(rot);
    op.scale_x   = 1.0f;
    op.scale_y   = 1.0f;
    op.mode      = mode;
    op.user_data = disp;
    return ppa_do_scale_rotate_mirror(s_flush.srm, &op);
}

/// The CPU's turn, for a block no PPA transfer can take safely (narrower than one unit, so at
/// most a few thousand pixels). The frame buffer is only ever written by DMA otherwise: its
/// lines are dropped from the cache before the CPU touches them, and written back after.
void cpu_rotate(lv_display_t* disp, const uint8_t* px, const lv_area_t* a, int32_t w, int32_t h,
                lv_display_rotation_t rot, uint8_t* fb) {
    const int32_t hres = lv_display_get_horizontal_resolution(disp);
    const int32_t vres = lv_display_get_vertical_resolution(disp);
    int32_t ax, ay, bx, by;
    to_panel(rot, hres, vres, a->x1, a->y1, ax, ay);
    to_panel(rot, hres, vres, a->x2, a->y2, bx, by);
    const uint32_t row_bytes = s_flush.panel_w * 3;
    const uint32_t first = (uint32_t)(ay < by ? ay : by), last = (uint32_t)(ay < by ? by : ay);
    uint8_t* const start = fb + first * row_bytes;
    const uint32_t len = (last - first + 1) * row_bytes;
    // Whole lines, aligned out: no frame buffer line is ever dirty, so dropping a neighbour's
    // part of one loses nothing.
    const uintptr_t lo = (uintptr_t)start & ~(uintptr_t)(LINE - 1);
    const uintptr_t hi = ((uintptr_t)start + len + LINE - 1) & ~(uintptr_t)(LINE - 1);
    esp_cache_msync((void*)lo, hi - lo, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    for (int32_t y = 0; y < h; y++) {
        const uint8_t* src = px + (uint32_t)(y * w) * s_flush.in_bpp;
        for (int32_t x = 0; x < w; x++, src += s_flush.in_bpp) {
            int32_t fx, fy;
            to_panel(rot, hres, vres, a->x1 + x, a->y1 + y, fx, fy);
            uint8_t* dst = fb + (uint32_t)fy * row_bytes + (uint32_t)fx * 3;
            dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2];      // B, G, R either way
        }
    }
    esp_cache_msync(start, len, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

void flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px) {
    const int32_t w = lv_area_get_width(area);
    const int32_t h = lv_area_get_height(area);
    const lv_display_rotation_t rot = lv_display_get_rotation(disp);
    const bool quarter = rot == LV_DISPLAY_ROTATION_90 || rot == LV_DISPLAY_ROTATION_270;
    // The frame's last block is written before the swap is asked for, so it goes blocking.
    const bool last = s_flush.dbl && lv_display_flush_is_last(disp);
    if (s_flush.dbl) {
        if (!s_flush.in_frame) begin_frame(disp);
        s_flush.now.add(to_fb(disp, *area));
    }
    uint8_t* const fb = s_flush.dbl ? s_flush.fbs[s_flush.back] : s_flush.fb;

    uint16_t rows[MAX_STRIPES];
    const int n = quarter ? ppa_geom::plan_stripes((uint32_t)w, (uint32_t)h, rows, MAX_STRIPES) : 1;

    if (n == 1) {
        const ppa_trans_mode_t mode = last ? PPA_TRANS_MODE_BLOCKING : PPA_TRANS_MODE_NON_BLOCKING;
        const esp_err_t err = submit(disp, px, area, w, h, 0, h, rot, mode, fb);
        if (err != ESP_OK) {
            // Never leave LVGL waiting for a done that will not come. The block is lost; the
            // next refresh of that area paints it again.
            s_flush.refused.fetch_add(1, std::memory_order_relaxed);
        }
        if (last) end_frame();
        if (err != ESP_OK || last) lv_display_flush_ready(disp);
        return;
    }
    if (n > 1) {
        // Each stripe waits for the last; their done interrupts already mark the buffer free,
        // which is harmless while this callback still holds LVGL.
        s_flush.split.fetch_add(1, std::memory_order_relaxed);
        int32_t row0 = 0;
        for (int i = 0; i < n; i++) {
            if (submit(disp, px, area, w, h, row0, rows[i], rot, PPA_TRANS_MODE_BLOCKING, fb) != ESP_OK)
                s_flush.refused.fetch_add(1, std::memory_order_relaxed);
            row0 += rows[i];
        }
        if (last) end_frame();
        lv_display_flush_ready(disp);
        return;
    }
    s_flush.cpu.fetch_add(1, std::memory_order_relaxed);
    cpu_rotate(disp, px, area, w, h, rot, fb);
    if (last) end_frame();
    lv_display_flush_ready(disp);
}

/// Two frame buffers and what composing in them needs, or false and single-buffered.
bool prepare_double(esp_lcd_panel_handle_t panel) {
#if CONFIG_UI_DOUBLE_FB
    void* a = nullptr;
    void* b = nullptr;
    if (esp_lcd_dpi_panel_get_frame_buffer(panel, 2, &a, &b) != ESP_OK || !a || !b) {
        ESP_LOGW(TAG, "one DPI frame buffer: frames are drawn on the glass");
        return false;
    }
    esp_async_fbcpy_config_t cfg = {};
    s_flush.swap_sem = xSemaphoreCreateBinary();
    s_flush.copy_sem = xSemaphoreCreateBinary();
    s_flush.plan_sem = xSemaphoreCreateBinary();
    s_flush.done_sem = xSemaphoreCreateBinary();
    if (!s_flush.swap_sem || !s_flush.copy_sem || !s_flush.plan_sem || !s_flush.done_sem ||
        esp_async_fbcpy_install(&cfg, &s_flush.fbcpy) != ESP_OK) {
        ESP_LOGW(TAG, "no DMA2D copy: frames are drawn on the glass");
        return false;
    }
    // Above LVGL and the radio on their core, so a swap is followed by its copies at once; it
    // only starts DMA transfers and sleeps on them. Internal stack: it runs while flash is busy.
    if (xTaskCreatePinnedToCore(compose_task, "ui_compose", 3072, nullptr, 6, &s_flush.task, 0) != pdPASS) {
        ESP_LOGW(TAG, "no compositor task: frames are drawn on the glass");
        return false;
    }
    s_flush.fbs[0] = static_cast<uint8_t*>(a);
    s_flush.fbs[1] = static_cast<uint8_t*>(b);
    s_flush.back = 1;                           // the driver starts on frame buffer 0
    // Buffer 1 has never been drawn: the first frame copies all of buffer 0 into it.
    s_flush.last.add(Rect{0, 0, (int32_t)s_flush.panel_w - 1, (int32_t)s_flush.panel_h - 1});
    return true;
#else
    (void)panel;
    return false;
#endif
}

} // namespace

bool ppa_flush_prepare(esp_lcd_panel_handle_t panel, uint32_t panel_w, uint32_t panel_h) {
    if (s_flush.srm) return true;
    if (!panel) return false;
    void* fb = nullptr;
    if (esp_lcd_dpi_panel_get_frame_buffer(panel, 1, &fb) != ESP_OK || !fb) {
        ESP_LOGW(TAG, "no DPI frame buffer");
        return false;
    }
    ppa_client_config_t cfg = {};
    cfg.oper_type = PPA_OPERATION_SRM;
    cfg.max_pending_trans_num = 1;   // LVGL waits for a block's done before it flushes the next
    ppa_client_handle_t srm = nullptr;
    if (ppa_register_client(&cfg, &srm) != ESP_OK) {
        ESP_LOGW(TAG, "no PPA client");
        return false;
    }
    ppa_event_callbacks_t cbs = {};
    cbs.on_trans_done = ppa_done;
    if (ppa_client_register_event_callbacks(srm, &cbs) != ESP_OK) {
        ppa_unregister_client(srm);
        ESP_LOGW(TAG, "PPA callback refused");
        return false;
    }
    s_flush.panel   = panel;
    s_flush.srm     = srm;
    s_flush.fb      = static_cast<uint8_t*>(fb);
    s_flush.fb_size = panel_w * panel_h * 3;          // the panel is RGB888
    s_flush.panel_w = panel_w;
    s_flush.panel_h = panel_h;
    s_flush.dbl     = prepare_double(panel);
    return true;
}

bool ppa_flush_install(lv_display_t* disp) {
    if (!disp || !s_flush.srm) return false;
    const lv_color_format_t cf = lv_display_get_color_format(disp);
    if (cf == LV_COLOR_FORMAT_RGB888) {
        s_flush.in_cm = PPA_SRM_COLOR_MODE_RGB888;   s_flush.in_bpp = 3;
    } else if (cf == LV_COLOR_FORMAT_XRGB8888 || cf == LV_COLOR_FORMAT_ARGB8888) {
        s_flush.in_cm = PPA_SRM_COLOR_MODE_ARGB8888; s_flush.in_bpp = 4;   // the PPA drops the X
    } else {
        return false;
    }
    if (s_flush.dbl) {
        // Replaces esp_lvgl_port's on_color_trans_done, which only answered draw_bitmap — and
        // draw_bitmap is now only the swap, which LVGL must not take for a finished block.
        esp_lcd_dpi_panel_event_callbacks_t cbs = {};
        cbs.on_frame_buf_complete = frame_done;
        if (esp_lcd_dpi_panel_register_event_callbacks(s_flush.panel, &cbs, nullptr) != ESP_OK) {
            ESP_LOGW(TAG, "no frame-done callback: frames are drawn on the glass");
            s_flush.dbl = false;
        } else {
            lv_display_add_event_cb(disp, inv_cb, LV_EVENT_INVALIDATE_AREA, nullptr);
            lv_display_add_event_cb(disp, render_start_cb, LV_EVENT_RENDER_START, nullptr);
            lv_display_add_event_cb(disp, refr_ready_cb, LV_EVENT_REFR_READY, nullptr);
        }
    }
    lv_display_set_flush_cb(disp, flush_cb);
    ESP_LOGI(TAG, "PPA rotates straight into the frame buffer (%lux%lu, %s in, RGB888 out, %s)",
             (unsigned long)s_flush.panel_w, (unsigned long)s_flush.panel_h,
             s_flush.in_bpp == 4 ? "XRGB8888" : "RGB888",
             s_flush.dbl ? "two buffers, lists shifted" : "one buffer");
    return true;
}

bool ppa_flush_double() { return s_flush.dbl; }


void ppa_flush_list_scrolled(lv_obj_t* scroller, int32_t dy) {
    if (!s_flush.dbl || !scroller) return;
    lv_display_t* disp = lv_obj_get_display(scroller);
    watch_list(scroller);
    lv_area_t win;
    if (!window_of(scroller, &win)) {
        drop_shift();
        return;
    }
    const bool still = lv_display_get_screen_prev(disp) == nullptr &&
                       lv_obj_get_screen(scroller) == lv_display_get_screen_active(disp);
    if (!still || !plain_backdrop(scroller)) {
        s_flush.st.ineligible++;
        drop_shift();
        return;
    }
    if (overlay_over(scroller, win)) {
        s_shift.clear_from = s_flush.ended + 2;
        s_flush.st.ineligible++;
        drop_shift();
        return;
    }
    // The glass must hold this window as the list last left it: a frame composed after the list
    // was first seen there (frames composed before may land later), and nothing over it lately.
    if (s_shift.seen != scroller || !same(s_shift.seen_win, win)) {
        s_shift.seen = scroller;
        s_shift.seen_win = win;
        s_shift.seen_at = s_flush.ended;
        s_flush.st.not_seen++;
        drop_shift();
        return;
    }
    if (s_flush.presented <= s_shift.seen_at || s_flush.presented < s_shift.clear_from) {
        s_flush.st.too_soon++;
        drop_shift();
        return;
    }
    if (s_shift.active && !same(s_shift.win, win)) {
        s_flush.st.moved++;
        drop_shift();
        return;
    }
    if (!s_shift.active) {
        s_shift.active = true;
        s_shift.win = win;
        s_shift.dy = 0;
    }
    s_shift.dy += dy;
    const int32_t wh = lv_area_get_height(&win);
    if (s_shift.dy >= wh || -s_shift.dy >= wh) {      // nothing left on the glass to keep
        s_flush.st.too_far++;
        drop_shift();
        return;
    }
    // A floating object over the window travels with the shift: what the glass shows of it lands
    // dy further, and where it was gets the list's content. Both are drawn again, and where it is
    // now, so it is drawn over the shifted content wherever it went meanwhile.
    for (const Floating& f : s_float) {
        if (!f.obj) continue;
        lv_area_t a;
        if (f.shown) {
            lv_area_t moved = f.at;
            lv_area_move(&moved, 0, s_shift.dy);
            if (lv_area_intersect(&a, &moved, &win)) lv_inv_area(disp, &a);
            if (lv_area_intersect(&a, &f.at, &win)) lv_inv_area(disp, &a);
        }
        if (!lv_obj_has_flag(f.obj, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_get_coords(f.obj, &a);
            const int32_t ext = lv_obj_get_ext_draw_size(f.obj);
            lv_area_increase(&a, ext, ext);
            if (lv_area_intersect(&a, &a, &win)) lv_inv_area(disp, &a);
        }
    }
    // The scrollbar does not travel with the content: its band is drawn again.
    lv_area_t band = win;
    band.x1 = win.x2 - BAND_W + 1;
    lv_inv_area(disp, &band);
    // What came into view: at the top when the content moved down.
    lv_area_t strip = win;
    if (s_shift.dy > 0)      strip.y2 = win.y1 + s_shift.dy - 1;
    else if (s_shift.dy < 0) strip.y1 = win.y2 + s_shift.dy + 1;
    else                     strip = band;
    s_shift.armed = true;
    s_shift.hook = win;
    s_shift.repl = strip;
}

void ppa_flush_list_reset(lv_obj_t* scroller) {
    if (!s_flush.dbl || !scroller) return;
    drop_shift();
    // New entries repaint the list's rows, and rows hidden for good repaint where they were, so
    // the next frame draws the window whole. Once that frame is on the glass the window there is
    // a basis for a shift: the list's first scroll is a shift, not a repaint.
    lv_obj_update_layout(scroller);
    watch_list(scroller);
    lv_area_t win;
    if (!window_of(scroller, &win)) {
        if (s_shift.seen == scroller) s_shift.seen = nullptr;
        return;
    }
    s_shift.seen = scroller;
    s_shift.seen_win = win;
    s_shift.seen_at = s_flush.ended;
}

bool ppa_flush_floating(lv_obj_t* obj, bool on) {
    if (!obj) return false;
    if (!on) {
        for (Floating& f : s_float) {
            if (f.obj != obj) continue;
            f = Floating{};
            lv_obj_remove_event_cb(obj, floating_deleted);
            return true;
        }
        return false;
    }
    if (floating(obj)) return true;
    for (Floating& f : s_float) {
        if (f.obj) continue;
        f = Floating{};
        f.obj = obj;
        lv_obj_add_event_cb(obj, floating_deleted, LV_EVENT_DELETE, nullptr);
        return true;
    }
    return false;
}

void ppa_flush_stats(PpaFlushStats* out, bool reset) {
    if (out) *out = s_flush.st;
    if (reset) s_flush.st = PpaFlushStats{};
}

uint8_t* ppa_flush_glass() {
    if (!s_flush.dbl) return s_flush.fb;
    // The last complete frame: a swap to it may still be on its way, but it is what shows next.
    return s_flush.fbs[s_flush.shown];
}

uint8_t* ppa_flush_idle_back() {
    if (!s_flush.dbl) return nullptr;
    if (s_flush.flip) {
        // Wait for the swap the last frame asked for; the semaphore stays given for begin_frame.
        for (int i = 0; i < 40 && s_flush.swap_pending; i++) vTaskDelay(1);
        return s_flush.swap_pending ? nullptr : s_flush.fbs[s_flush.back ^ 1];
    }
    return s_flush.fbs[s_flush.back];
}

bool ppa_flush_frame_buffers(PpaFlushFbs* out) {
    if (!out || !s_flush.panel) return false;
    void* a = nullptr;
    void* b = nullptr;
    if (esp_lcd_dpi_panel_get_frame_buffer(s_flush.panel, 2, &a, &b) != ESP_OK) {
        b = nullptr;
        if (esp_lcd_dpi_panel_get_frame_buffer(s_flush.panel, 1, &a) != ESP_OK) return false;
    }
    out->fb[0] = static_cast<uint8_t*>(a);
    out->fb[1] = static_cast<uint8_t*>(b);
    out->w = s_flush.panel_w;
    out->h = s_flush.panel_h;
    return true;
}

uint32_t ppa_flush_refused() { return s_flush.refused.load(std::memory_order_relaxed); }
uint32_t ppa_flush_split()   { return s_flush.split.load(std::memory_order_relaxed); }
uint32_t ppa_flush_cpu()     { return s_flush.cpu.load(std::memory_order_relaxed); }
