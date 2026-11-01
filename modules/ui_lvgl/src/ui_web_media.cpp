/**
 * @file ui_web_media.cpp
 * @brief GET /api/library and GET /api/cover — see ui_web_media.h. The bodies are written by
 *        include/web_media.h, which the host suite tests.
 *
 * Both read what the panel's own pages read, and nothing more:
 *   - the library is the card's index (components/playlist), which screen_library.cpp draws —
 *     a page at a time, since a card holds up to 2048 entries. Playing one is the web's
 *     player.track + player.cmd=1, exactly what the panel's PLAY_QUEUE does;
 *   - the cover is the tile screen_now.cpp draws, chosen by the same cover_now() and named by
 *     ui.cover. The tile is all this firmware keeps of a picture, so that is what is sent —
 *     scaled to a thumbnail, as a BMP like GET /api/screenshot. Keeping each source's original
 *     JPEG as well (the plan's first idea) would hold up to five more pictures in PSRAM for a
 *     56-pixel square.
 *
 * Bodies live in PSRAM for the length of a call, never on the 8 KB httpd stack: the internal
 * heap is the scarce one. Both routes are behind the web's login.
 */

#include "ui_web_media.h"
#include "web_media.h"
#include "cover.h"
#include "playlist.h"
#include "modesp/net/http_routes.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static const char* TAG = "UiWeb";

static_assert(web_media::LIB_REL_CAP == (size_t)player_core::Playlist::MAX_REL,
              "web_media's row buffer must hold a whole index entry");

namespace {

std::atomic<int> s_cover_tile{-1};

using modesp::http_routes::send_json;

/// `key` from the request's query as a number, or −1 when absent or not a number.
long query_num(const char* query, const char* key) {
    char v[16];
    if (!query || httpd_query_key_value(query, key, v, sizeof(v)) != ESP_OK || !v[0]) return -1;
    char* end = nullptr;
    const long n = strtol(v, &end, 10);
    return (end && *end == '\0' && n >= 0) ? n : -1;
}

// ── GET /api/library?from=&n= ──

esp_err_t library_get(httpd_req_t* req) {
    if (!modesp::http_routes::authorized(req)) return ESP_OK;
    char query[48] = "";
    const bool has_query = httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK;
    const web_media::Page page = web_media::clamp_page(has_query ? query_num(query, "from") : -1,
                                                       has_query ? query_num(query, "n") : -1);

    char* body = (char*)heap_caps_malloc(web_media::LIB_JSON_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) return send_json(req, "500 Internal Server Error", "{\"error\":\"no_memory\"}");

    const auto& q = player_core::playlist();
    char root[player_core::Playlist::MAX_DIR];
    q.root(root, sizeof(root));
    const auto rel = [&q](int i, char* out, size_t n) { return q.rel_at(i, out, n); };

    // The index is settled while its generation is even and the same before and after the page
    // was read; the player rebuilds it when a card appears. Otherwise the page says busy, with
    // no rows, and the web asks again.
    const uint32_t gen = q.generation();
    int rows = 0;
    size_t len = web_media::library_page(body, web_media::LIB_JSON_CAP, gen, (gen & 1u) != 0,
                                         q.count(), q.cursor(), root, page, rel, &rows);
    if (len && !(gen & 1u) && q.generation() != gen) {
        len = web_media::library_page(body, web_media::LIB_JSON_CAP, q.generation(), true, 0, 0,
                                      root, page, rel, &rows);
    }
    if (!len) {
        heap_caps_free(body);
        return send_json(req, "500 Internal Server Error", "{\"error\":\"too_large\"}");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t err = httpd_resp_send(req, body, len);
    heap_caps_free(body);
    return err;
}

// ── GET /api/cover?px= ──

esp_err_t cover_get(httpd_req_t* req) {
    if (!modesp::http_routes::authorized(req)) return ESP_OK;
    char query[24] = "";
    const long px = httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK
                        ? query_num(query, "px") : -1;
    const int factor = web_media::bmp_factor(player_core::COVER_PX, (int)px);
    const uint32_t side = (uint32_t)(player_core::COVER_PX / factor);

    const int t = s_cover_tile.load(std::memory_order_acquire);
    if (t < 0 || t >= player_core::COVER_SOURCES) {
        return send_json(req, "404 Not Found", "{\"error\":\"no_cover\"}");
    }
    const auto tile = (player_core::CoverSource)t;
    auto view = player_core::cover_view(tile);
    if (!view.px || view.w != player_core::COVER_PX || view.h != player_core::COVER_PX) {
        return send_json(req, "404 Not Found", "{\"error\":\"no_cover\"}");
    }

    // The same tag for the same picture at the same size: a browser that has it gets a 304.
    char etag[32];
    snprintf(etag, sizeof(etag), "\"%ld-%lu\"", (long)player_core::cover_id(view, tile), (unsigned long)side);
    char have[32];
    if (httpd_req_get_hdr_value_str(req, "If-None-Match", have, sizeof(have)) == ESP_OK &&
        strcmp(have, etag) == 0) {
        httpd_resp_set_status(req, "304 Not Modified");
        httpd_resp_set_hdr(req, "ETag", etag);
        httpd_resp_set_hdr(req, "Cache-Control", "private, no-cache");
        return httpd_resp_send(req, nullptr, 0);
    }

    const uint32_t size = web_media::bmp_size(side);
    auto* bmp = (uint8_t*)heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!bmp) return send_json(req, "500 Internal Server Error", "{\"error\":\"no_memory\"}");

    // A producer writes the slot that is NOT current, so the one read here changes only if two
    // new pictures arrive while it is read — a millisecond. The picture is written whole before
    // anything is sent, and written again if its generation moved meanwhile.
    bool still = false;
    for (int attempt = 0; attempt < 3 && !still; ++attempt) {
        web_media::bmp_write(view.px, player_core::COVER_PX, factor, bmp);
        const auto after = player_core::cover_view(tile);
        still = after.gen == view.gen;
        if (!still) {
            view = after;
            if (!view.px || view.w != player_core::COVER_PX || view.h != player_core::COVER_PX) break;
            snprintf(etag, sizeof(etag), "\"%ld-%lu\"", (long)player_core::cover_id(view, tile),
                     (unsigned long)side);
        }
    }
    if (!still) {
        heap_caps_free(bmp);
        return send_json(req, "503 Service Unavailable", "{\"error\":\"changing\"}");
    }
    httpd_resp_set_type(req, "image/bmp");
    httpd_resp_set_hdr(req, "ETag", etag);
    httpd_resp_set_hdr(req, "Cache-Control", "private, no-cache");
    const esp_err_t err = httpd_resp_send(req, (const char*)bmp, size);
    heap_caps_free(bmp);
    return err;
}

void register_routes(httpd_handle_t server, void*) {
    httpd_uri_t lib{};
    lib.uri = "/api/library";
    lib.method = HTTP_GET;
    lib.handler = library_get;
    if (httpd_register_uri_handler(server, &lib) != ESP_OK) ESP_LOGW(TAG, "/api/library not registered");
    httpd_uri_t cover{};
    cover.uri = "/api/cover";
    cover.method = HTTP_GET;
    cover.handler = cover_get;
    if (httpd_register_uri_handler(server, &cover) != ESP_OK) ESP_LOGW(TAG, "/api/cover not registered");
}

}  // namespace

void ui_web_media_init() {
    if (!modesp::http_routes::add(register_routes)) {
        ESP_LOGW(TAG, "no room in the route table — /api/library and /api/cover will 404");
    }
}

void ui_web_media_set_cover(int tile) {
    s_cover_tile.store(tile, std::memory_order_release);
}
