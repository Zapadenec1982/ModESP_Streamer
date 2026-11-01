/**
 * @file ui_screenshot.cpp
 * @brief GET /api/screenshot — what the glass is showing, as a 24-bit BMP.
 *
 * Read out of the DPI frame buffer, not re-rendered with lv_snapshot. The question this
 * answers is "what does the panel show", and a re-render in whatever format the snapshot is
 * asked for shows what LVGL could draw instead: at 24 bits it would have hidden exactly the
 * RGB565 banding that prompted this route. The frame buffer is the panel's own pixels, at its
 * own depth, after rotation — so a 16-bit panel comes out with its steps intact.
 *
 * Frame buffer 0 is the one on the glass. esp_lvgl_port hands the DPI driver draw buffers of
 * its own, the driver copies them into fbs[cur_fb_index], and that index only moves when a
 * caller draws INTO a frame buffer (esp_lcd_panel_dpi.c, dpi_panel_draw_bitmap). Nothing in
 * this firmware does.
 *
 * The copy into it is DMA2D, behind the CPU's back, so the cache is invalidated over the
 * buffer before it is read. A frame being drawn while the rows go out can tear the picture;
 * for a debugging aid on a mostly still screen that is an acceptable price for not stopping
 * LVGL.
 *
 * Behind the web UI's own login (modesp::http_routes::authorized), unless the build says
 * CONFIG_UI_SCREENSHOT_OPEN — the bench panel's switch, see this module's Kconfig.
 */

#include "ui_screenshot.h"
#include "ui_fit.h"
#include "ui_frame_meter.h"
#include "ui_common.h"
#include "ppa_flush.h"
#include "modesp/net/http_routes.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

static const char* TAG = "UiShot";

namespace {

struct Source {
    esp_lcd_panel_handle_t panel = nullptr;
    uint16_t w = 0, h = 0;      // the frame buffer's own layout: the panel, unrotated
    uint8_t  bits = 0;          // 16 (RGB565) or 24 (LVGL's RGB888: blue, green, red)
    uint16_t rotation = 0;      // degrees, as board.json gives it
};
Source s_src;

/// Rows per HTTP chunk: 16 rows of an 800-wide picture is 38 KB, in PSRAM.
constexpr uint32_t ROWS_PER_CHUNK = 16;

void put16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
void put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/// The UI's pixel (x, y) to where the port put it in the frame buffer. The same arithmetic
/// as lvgl_port_ppa_rotate() in esp_lvgl_port, angle for angle.
inline uint32_t fb_index(uint32_t x, uint32_t y, const Source& s) {
    uint32_t px, py;
    switch (s.rotation) {
    case 90:  px = y;           py = s.h - 1u - x; break;
    case 180: px = s.w - 1u - x; py = s.h - 1u - y; break;
    case 270: px = s.w - 1u - y; py = x;           break;
    default:  px = x;           py = y;           break;
    }
    return py * s.w + px;
}

/// `?screen=<id>`: show that screen first, then capture it. The redesign is accepted screen
/// by screen from these pictures, and the person taking them is not always in front of the
/// glass. The switch happens under the LVGL lock, the way a palette change does, and the
/// capture waits two refresh ticks so the frame buffer holds the new screen, not the old one.
/// An unknown id is a 404 with nothing changed.
#if CONFIG_UI_SCREENSHOT_OPEN
// ── a finger from the bench ──
//
// `tap=x,y` / `hold=x,y` press the screen at a point of the picture, through a pointer
// device of LVGL's own, so the press goes through exactly the path a touch does — hit test,
// click, long press — and the screenshot that follows shows what the tap did. It lets the
// redesign be walked from the bench, page by page, without a hand on the panel. Bench builds
// only, behind the same switch as the open route.
struct FakeTouch {
    lv_indev_t* indev = nullptr;
    volatile int32_t  x = 0, y = 0;
    volatile uint32_t t0 = 0, press_ms = 0;
    volatile uint32_t reads = 0;
    volatile bool     active = false;
};
FakeTouch s_fake;

void fake_read(lv_indev_t*, lv_indev_data_t* d) {
    // The point is asked for in the picture's coordinates, but a pointer device reports the
    // panel's own: LVGL rotates what a device says (indev_pointer_proc) the way it rotates
    // the touch controller's, so the bench point goes through that rotation backwards.
    lv_display_t* disp = lv_display_get_default();
    const int32_t W = lv_display_get_original_horizontal_resolution(disp);
    const int32_t H = lv_display_get_original_vertical_resolution(disp);
    int32_t rx = s_fake.x, ry = s_fake.y;
    switch (lv_display_get_rotation(disp)) {
    case LV_DISPLAY_ROTATION_90:  rx = s_fake.y;         ry = H - 1 - s_fake.x; break;
    case LV_DISPLAY_ROTATION_180: rx = W - 1 - s_fake.x; ry = H - 1 - s_fake.y; break;
    case LV_DISPLAY_ROTATION_270: rx = W - 1 - s_fake.y; ry = s_fake.x;         break;
    default: break;
    }
    d->point.x = rx;
    d->point.y = ry;
    // The press is timed from the FIRST read, not from the request: a page switch in the
    // same request can keep the LVGL task drawing for longer than a tap lasts, and a press
    // that had expired before it was ever read is no press at all. Two reads at least, so
    // LVGL sees a press and then a release at the same point — which is what a click is.
    if (s_fake.active) {
        if (s_fake.t0 == 0) s_fake.t0 = lv_tick_get();
        if (s_fake.reads < 2 || lv_tick_elaps(s_fake.t0) < s_fake.press_ms) {
            s_fake.reads = s_fake.reads + 1;
            d->state = LV_INDEV_STATE_PRESSED;
            return;
        }
        s_fake.active = false;
    }
    d->state = LV_INDEV_STATE_RELEASED;
}

/// Arm one press. Under the LVGL lock: the device is made on first use.
bool fake_touch(int32_t x, int32_t y, uint32_t press_ms) {
    if (x < 0 || y < 0 || x >= lv_display_get_horizontal_resolution(nullptr) ||
        y >= lv_display_get_vertical_resolution(nullptr)) return false;
    if (!s_fake.indev) {
        s_fake.indev = lv_indev_create();
        if (!s_fake.indev) return false;
        lv_indev_set_type(s_fake.indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(s_fake.indev, fake_read);
    }
    s_fake.x = x;
    s_fake.y = y;
    s_fake.press_ms = press_ms;
    s_fake.t0 = 0;
    s_fake.reads = 0;
    s_fake.active = true;
    return true;
}
#endif

/// `peek=drawer|standby` lays that overlay over the page for the picture; `peeked` tells the
/// caller to take it away again once the picture is sent. `tap=x,y` and `hold=x,y` press
/// the screen there (bench builds) before the capture. False once an error is sent: the request
/// is answered, and the caller stops. httpd_resp_send_err() returns ESP_OK when the error went
/// out, so its result cannot say that.
bool switch_screen_if_asked(httpd_req_t* req, bool& peeked) {
    peeked = false;
    char query[96];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) return true;
    char id[16], peek[16], at[24];
    const bool has_screen = httpd_query_key_value(query, "screen", id, sizeof(id)) == ESP_OK;
    const bool has_peek   = httpd_query_key_value(query, "peek", peek, sizeof(peek)) == ESP_OK;
    uint32_t press_ms = 0;
    if (httpd_query_key_value(query, "tap", at, sizeof(at)) == ESP_OK)       press_ms = 80;
    else if (httpd_query_key_value(query, "hold", at, sizeof(at)) == ESP_OK) press_ms = 900;
    if (!has_screen && !has_peek && !press_ms) return true;
    if (!lvgl_port_lock(200)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "LVGL busy");
        return false;
    }
    bool known = true;
    if (has_screen) known = ui_shell_go(id);
    if (known && has_peek) { known = ui_shell_peek(peek); peeked = known; }
    bool pressed = true;
    if (known && press_ms) {
#if CONFIG_UI_SCREENSHOT_OPEN
        int x = 0, y = 0;
        pressed = sscanf(at, "%d,%d", &x, &y) == 2 && fake_touch(x, y, press_ms);
#else
        pressed = false;
#endif
    }
    lvgl_port_unlock();
    if (!known) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No such screen or peek");
        return false;
    }
    if (!pressed) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "tap=x,y inside the picture, bench builds only");
        return false;
    }
    // the refresh timer runs every 40 ms; a press needs its first read, its release and the
    // refresh after it — and the page it lands on may take a moment to say what it did
    vTaskDelay(pdMS_TO_TICKS(120 + press_ms + (press_ms ? 500 : 0)));
    return true;
}

/// Undoes a peek on every way out of the handler.
struct Unpeek {
    bool on = false;
    ~Unpeek() {
        if (on && lvgl_port_lock(200)) { ui_shell_peek(nullptr); lvgl_port_unlock(); }
    }
};

esp_err_t handle_screenshot(httpd_req_t* req) {
#if !CONFIG_UI_SCREENSHOT_OPEN
    if (!modesp::http_routes::authorized(req)) return ESP_OK;
#endif
    Unpeek unpeek;
    if (!switch_screen_if_asked(req, unpeek.on)) return ESP_OK;   // the error is already sent

    const Source s = s_src;
    void* fb = nullptr;
    void* still = nullptr;       // the picture, held still while it is sent
    const size_t fb_bytes = (size_t)s.w * s.h * s.bits / 8;
    if (ppa_flush_double() && s.panel) {
        // With two frame buffers the one on the glass is the next frame's to compose into two
        // frames later, and sending the picture takes many more than two: read straight from
        // it, a moving screen came out as two frames spliced. So the glass is copied out first,
        // under the LVGL lock, while no frame can be swapped in (~25 ms).
        still = heap_caps_malloc(fb_bytes, MALLOC_CAP_SPIRAM);
        if (!still) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
        if (!lvgl_port_lock(1000)) {
            heap_caps_free(still);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "LVGL busy");
        }
        void* glass = ppa_flush_glass();
        if (esp_cache_msync(glass, fb_bytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C) != ESP_OK) {
            ESP_LOGW(TAG, "cache invalidate refused; the picture may be a frame old");
        }
        memcpy(still, glass, fb_bytes);
        lvgl_port_unlock();
        fb = still;
    }
    if (!s.panel || (s.bits != 16 && s.bits != 24) ||
        (!fb && esp_lcd_dpi_panel_get_frame_buffer(s.panel, 1, &fb) != ESP_OK) || !fb) {
        heap_caps_free(still);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No frame buffer");
    }
    if (!still && esp_cache_msync(fb, fb_bytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C) != ESP_OK) {
        ESP_LOGW(TAG, "cache invalidate refused; the picture may be a frame old");
    }

    const bool quarter = (s.rotation == 90 || s.rotation == 270);
    const uint32_t out_w = quarter ? s.h : s.w;
    const uint32_t out_h = quarter ? s.w : s.h;
    const uint32_t row_bytes = out_w * 3;
    const uint32_t stride = (row_bytes + 3u) & ~3u;         // BMP rows pad to four bytes

    uint8_t hdr[54] = {};
    hdr[0] = 'B'; hdr[1] = 'M';
    put32(hdr + 2, 54 + stride * out_h);                    // file size
    put32(hdr + 10, 54);                                    // pixel data offset
    put32(hdr + 14, 40);                                    // BITMAPINFOHEADER
    put32(hdr + 18, out_w);
    put32(hdr + 22, (uint32_t)(-(int32_t)out_h));           // negative: top row first
    put16(hdr + 26, 1);                                     // planes
    put16(hdr + 28, 24);                                    // bits per pixel
    put32(hdr + 34, stride * out_h);                        // image size

    auto* buf = static_cast<uint8_t*>(heap_caps_malloc(stride * ROWS_PER_CHUNK, MALLOC_CAP_SPIRAM));
    if (!buf) {
        heap_caps_free(still);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
    }

    httpd_resp_set_type(req, "image/bmp");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=\"screen.bmp\"");

    const auto* px8  = static_cast<const uint8_t*>(fb);
    const auto* px16 = static_cast<const uint16_t*>(fb);
    esp_err_t err = httpd_resp_send_chunk(req, reinterpret_cast<const char*>(hdr), sizeof(hdr));
    for (uint32_t y0 = 0; err == ESP_OK && y0 < out_h; y0 += ROWS_PER_CHUNK) {
        const uint32_t rows = std::min(ROWS_PER_CHUNK, out_h - y0);
        for (uint32_t r = 0; r < rows; ++r) {
            uint8_t* o = buf + r * stride;
            for (uint32_t x = 0; x < out_w; ++x, o += 3) {
                const uint32_t i = fb_index(x, y0 + r, s);
                if (s.bits == 24) {
                    memcpy(o, px8 + i * 3u, 3);             // blue, green, red: BMP's order too
                } else {
                    // Widened by repeating the top bits, so full scale stays full scale and
                    // every one of the 32 or 64 steps is still a step.
                    const uint32_t v = px16[i];
                    const uint32_t r5 = v >> 11, g6 = (v >> 5) & 0x3Fu, b5 = v & 0x1Fu;
                    o[0] = (uint8_t)((b5 << 3) | (b5 >> 2));
                    o[1] = (uint8_t)((g6 << 2) | (g6 >> 4));
                    o[2] = (uint8_t)((r5 << 3) | (r5 >> 2));
                }
            }
            memset(o, 0, stride - row_bytes);
        }
        err = httpd_resp_send_chunk(req, reinterpret_cast<const char*>(buf), rows * stride);
    }
    heap_caps_free(buf);
    heap_caps_free(still);
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, nullptr, 0);
    return err;
}

void register_route(httpd_handle_t server, void*) {
    httpd_uri_t uri{};
    uri.uri = "/api/screenshot";
    uri.method = HTTP_GET;
    uri.handler = handle_screenshot;
    if (httpd_register_uri_handler(server, &uri) != ESP_OK) {
        ESP_LOGW(TAG, "/api/screenshot not registered");
    }
    ui_fit_register(server);         // GET /api/i18n/overflow, the glass's text (ui_fit.cpp)
    ui_frames_register(server);      // GET /api/ui/frames, the glass's frame time (ui_frame_meter.cpp)
#if CONFIG_UI_SCREENSHOT_OPEN
    /* Said at WARN so it survives the shipping log level: a panel that leaves the bench
     * with this on should say so at every boot. */
    ESP_LOGW(TAG, "/api/screenshot answers WITHOUT login (CONFIG_UI_SCREENSHOT_OPEN) — "
                  "development build");
#endif
}

} // namespace

void ui_screenshot_init(esp_lcd_panel_handle_t panel, uint16_t w, uint16_t h,
                        uint8_t color_bits, uint16_t rotation) {
    s_src = Source{panel, w, h, color_bits, rotation};
    if (!modesp::http_routes::add(register_route)) {
        ESP_LOGW(TAG, "no room in the route table — /api/screenshot will 404");
    }
}
