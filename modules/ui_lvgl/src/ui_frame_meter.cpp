/**
 * @file ui_frame_meter.cpp
 * @brief GET /api/ui/frames — how long the glass takes to draw a frame of what it shows.
 *
 * S5: the Player's frame with the spectrum must cost no more
 * with the cover's gradient behind it than without. Nothing here is scripted, unlike the scroll
 * bench (ui_scroll_bench.cpp, development panels only): the meter times the frames the screen
 * draws by itself, for `ms` (1–20 s, 5 s if not said), and changes nothing that persists.
 *
 * A frame is LV_EVENT_REFR_START to LV_EVENT_REFR_READY, split the way the bench splits it:
 *   flush  — FLUSH_START to FLUSH_FINISH, summed: the port's flush, the PPA rotation;
 *   wait   — FLUSH_WAIT_START to FLUSH_WAIT_FINISH, summed: LVGL waiting for the last copy;
 *   render — the rest: layout and drawing.
 * Only frames that flushed something count; a refresh that found nothing to draw is not a
 * frame of anything. The first 300 ms are not counted either: the page a run starts with may
 * be repainting whole.
 *
 * `?plain=1` takes the Player's gradient away for the run — the page as it is with no cover —
 * and gives it back after (ui_now_plain), so two runs on the same music differ by the gradient
 * alone. `cover` in the answer says whether there was a cover's colour to take away.
 *
 * The HTTP task waits out the run, so the web UI's other requests wait with it, as they do for
 * the bench. Behind the web UI's login like /api/screenshot (open on a bench build the same way).
 */

#include "ui_frame_meter.h"
#include "ui_common.h"
#include "modesp/net/http_routes.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static const char* TAG = "UiFrames";

namespace {

constexpr int     HIST_MS   = 100;      ///< 1 ms buckets; the last one holds everything longer
constexpr int64_t SETTLE_US = 300000;   ///< not counted: the first paint after a change

struct Acc {
    uint32_t n   = 0;
    uint64_t sum = 0;
    uint32_t max = 0;
    void add(uint32_t v) { n++; sum += v; if (v > max) max = v; }
    double avg_ms() const { return n ? (double)sum / (double)n / 1000.0 : 0.0; }
    double max_ms() const { return (double)max / 1000.0; }
};

/// Written by the LVGL task while a run is active, read by the HTTP task after it.
struct Run {
    int64_t  t_count = 0;                ///< frames that start before this are not counted
    bool     timing = false;
    int64_t  t_frame = 0, t_flush = 0, t_wait = 0;
    uint32_t flush_us = 0, wait_us = 0, flushes = 0;
    uint32_t refreshes = 0;              ///< every refresh counted, drawn or not
    Acc      frame, flush, wait, render;
    uint16_t hist[HIST_MS] = {};
};

std::atomic<bool> s_active{false};
std::atomic<bool> s_busy{false};
Run  s_run;
bool s_hooked = false;

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

void disp_cb(lv_event_t* e) {
    if (!s_active.load(std::memory_order_acquire)) return;
    const int64_t now = esp_timer_get_time();
    switch (lv_event_get_code(e)) {
    case LV_EVENT_REFR_START:
        s_run.timing = now >= s_run.t_count;
        s_run.t_frame = now;
        s_run.flush_us = s_run.wait_us = s_run.flushes = 0;
        break;
    case LV_EVENT_FLUSH_START:
        if (s_run.timing) { s_run.t_flush = now; s_run.flushes++; }
        break;
    case LV_EVENT_FLUSH_FINISH:
        if (s_run.timing && s_run.t_flush) { s_run.flush_us += (uint32_t)(now - s_run.t_flush); s_run.t_flush = 0; }
        break;
    case LV_EVENT_FLUSH_WAIT_START:
        if (s_run.timing) s_run.t_wait = now;
        break;
    case LV_EVENT_FLUSH_WAIT_FINISH:
        if (s_run.timing && s_run.t_wait) { s_run.wait_us += (uint32_t)(now - s_run.t_wait); s_run.t_wait = 0; }
        break;
    case LV_EVENT_REFR_READY: {
        if (!s_run.timing) break;
        s_run.timing = false;
        s_run.refreshes++;
        if (!s_run.flushes) break;
        const uint32_t f = (uint32_t)(now - s_run.t_frame);
        s_run.frame.add(f);
        uint32_t ms = f / 1000;
        if (ms >= (uint32_t)HIST_MS) ms = HIST_MS - 1;
        if (s_run.hist[ms] < UINT16_MAX) s_run.hist[ms]++;
        s_run.flush.add(s_run.flush_us);
        s_run.wait.add(s_run.wait_us);
        const uint32_t known = s_run.flush_us + s_run.wait_us;
        s_run.render.add(f > known ? f - known : 0);
        break;
    }
    default:
        break;
    }
}

using modesp::http_routes::send_json;

esp_err_t handle_frames(httpd_req_t* req) {
#if !CONFIG_UI_SCREENSHOT_OPEN
    if (!modesp::http_routes::authorized(req)) return ESP_OK;
#endif
    int32_t ms = 5000;
    bool plain = false;
    char query[48];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char v[12];
        if (httpd_query_key_value(query, "ms", v, sizeof(v)) == ESP_OK) ms = atoi(v);
        if (httpd_query_key_value(query, "plain", v, sizeof(v)) == ESP_OK) plain = atoi(v) != 0;
    }
    if (ms < 1000) ms = 1000; else if (ms > 20000) ms = 20000;
    if (s_busy.exchange(true)) return send_json(req, "409 Conflict", "{\"error\":\"a run is in progress\"}");

    if (!lvgl_port_lock(500)) {
        s_busy.store(false);
        return send_json(req, "503 Service Unavailable", "{\"error\":\"LVGL busy\"}");
    }
    lv_display_t* disp = lv_display_get_default();
    if (!s_hooked && disp) {
        for (lv_event_code_t code : {LV_EVENT_REFR_START, LV_EVENT_REFR_READY, LV_EVENT_FLUSH_START,
                                     LV_EVENT_FLUSH_FINISH, LV_EVENT_FLUSH_WAIT_START, LV_EVENT_FLUSH_WAIT_FINISH})
            lv_display_add_event_cb(disp, disp_cb, code, nullptr);
        s_hooked = true;
    }
    char screen[16];
    snprintf(screen, sizeof(screen), "%s", ui_shell_current());
    // Both runs set the page's style once, so both start with the same full repaint, which the
    // settling time leaves out.
    const bool cover = ui_now_plain(plain);
    s_run = Run{};
    s_run.t_count = esp_timer_get_time() + SETTLE_US;
    s_active.store(true, std::memory_order_release);
    lvgl_port_unlock();

    vTaskDelay(pdMS_TO_TICKS(SETTLE_US / 1000 + ms));

    // The gradient goes back whether or not the lock comes: a Player left plain would be the
    // meter's doing. Without the lock it is put back on the next try, below.
    bool locked = lvgl_port_lock(1000);
    s_active.store(false, std::memory_order_release);
    if (plain && locked) ui_now_plain(false);
    const Run r = s_run;
    if (locked) lvgl_port_unlock();
    if (plain && !locked && lvgl_port_lock(5000)) { ui_now_plain(false); lvgl_port_unlock(); }
    s_busy.store(false);

    char body[512];
    const double secs = ms / 1000.0;
    snprintf(body, sizeof(body),
             "{\"screen\":\"%s\",\"plain\":%s,\"cover\":%s,\"ms\":%ld,\"frames\":%lu,\"refreshes\":%lu,"
             "\"fps\":%.1f,\"frame_ms\":{\"avg\":%.2f,\"p95\":%lu,\"max\":%.2f},"
             "\"render_ms_avg\":%.2f,\"flush_ms_avg\":%.2f,\"wait_ms_avg\":%.2f}",
             screen, plain ? "true" : "false", cover ? "true" : "false", (long)ms,
             (unsigned long)r.frame.n, (unsigned long)r.refreshes, r.frame.n / secs,
             r.frame.avg_ms(), (unsigned long)p95_ms(r.hist, r.frame.n), r.frame.max_ms(),
             r.render.avg_ms(), r.flush.avg_ms(), r.wait.avg_ms());
    ESP_LOGI(TAG, "%s", body);
    return send_json(req, "200 OK", body);
}

}  // namespace

void ui_frames_register(httpd_handle_t server) {
    httpd_uri_t uri{};
    uri.uri = "/api/ui/frames";
    uri.method = HTTP_GET;
    uri.handler = handle_frames;
    if (httpd_register_uri_handler(server, &uri) != ESP_OK) ESP_LOGW(TAG, "/api/ui/frames not registered");
}
