/**
 * @file userdata_http.cpp
 * @brief GET and POST /api/userdata — the user's files, backed up and restored from the web.
 *
 * GET sends every user file that exists (UserStore::FILES: the station list, the EQ and
 * "Свій", the remote devices, the track to resume) as one JSON document to keep
 * (components/user_store/include/user_backup.h). POST takes such a document back.
 *
 * A restore is not written over the files in use. Each file belongs to a module that holds
 * its own copy in memory — the EQ in the audio module, the list in the radio module — and
 * would write that copy back over the restored one. So the files are staged beside them as
 * <name>.restore, and UserStore::begin() puts them in place on the next boot, before any
 * module reads anything. The web restarts the panel straight after (POST /api/restart).
 *
 * Here and not in components/user_store: the HTTP server depends on that component, so it
 * cannot depend on the server. Every file is read and checked before any is staged, so a
 * damaged backup leaves nothing half restored.
 */

#include "userdata_http.h"

#include "user_backup.h"
#include "user_store.h"
#include "safe_file.h"
#include "modesp/net/http_routes.h"
#include "modesp/net/json_escape.h"

#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include <cstdio>
#include <cstring>

namespace {

const char* TAG = "UserData";

/// The largest body a restore may be: the five files at their largest, escaped. Each file
/// is held to its own reader's limit as well (user_backup::max_bytes).
constexpr size_t BODY_MAX = 192 * 1024;

/// A buffer in PSRAM, freed with its scope. The files are up to tens of kilobytes, and the
/// HTTP server's task has 8 KB of stack and internal RAM is the scarce kind.
struct Buf {
    char*  p = nullptr;
    size_t n = 0;
    explicit Buf(size_t bytes) : n(bytes) {
        p = (char*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!p) p = (char*)heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
    }
    ~Buf() { heap_caps_free(p); }
    Buf(const Buf&) = delete;
    Buf& operator=(const Buf&) = delete;
};

using modesp::http_routes::send_json;

/// The whole of `path`, NUL-terminated, into `out` (made here). False when there is no such
/// file or it is larger than safe_file copies.
bool read_file(const char* path, Buf*& out, size_t& len) {
    out = nullptr;
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0 || (size_t)size > safe_file::MAX_COPY_BYTES) { fclose(f); return false; }
    out = new Buf((size_t)size + 1);
    if (!out->p) { fclose(f); delete out; out = nullptr; return false; }
    len = fread(out->p, 1, (size_t)size, f);
    fclose(f);
    out->p[len] = '\0';
    return true;
}

/// GET /api/userdata → {"modesp_userdata":1,"files":{"<name>":"<text>",…}}, as a download.
esp_err_t userdata_get(httpd_req_t* req) {
    if (!modesp::http_routes::authorized(req)) return ESP_OK;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"p4-player-userdata.json\"");
    char head[48];
    snprintf(head, sizeof(head), "{\"modesp_userdata\":%d,\"files\":{", modesp::user_backup::VERSION);
    if (httpd_resp_send_chunk(req, head, HTTPD_RESP_USE_STRLEN) != ESP_OK) return ESP_FAIL;
    bool first = true;
    for (const char* const* name = modesp::UserStore::FILES; *name; ++name) {
        char path[160];
        modesp::UserStore::read_path(*name, path, sizeof(path));
        Buf* text = nullptr;
        size_t len = 0;
        if (!read_file(path, text, len)) continue;
        Buf esc(modesp::net::json_escaped_capacity(len));
        size_t w = 0;
        const bool ok = esc.p && modesp::net::json_escape(esc.p, esc.n, text->p, &w);
        delete text;
        if (!ok) {
            ESP_LOGW(TAG, "%s left out of the backup: no memory to send it", path);
            continue;
        }
        char key[48];
        snprintf(key, sizeof(key), "%s\"%s\":\"", first ? "" : ",", *name);
        first = false;
        if (httpd_resp_send_chunk(req, key, HTTPD_RESP_USE_STRLEN) != ESP_OK ||
            httpd_resp_send_chunk(req, esc.p, (ssize_t)w) != ESP_OK ||
            httpd_resp_send_chunk(req, "\"", 1) != ESP_OK) return ESP_FAIL;
    }
    if (httpd_resp_send_chunk(req, "}}", 2) != ESP_OK) return ESP_FAIL;
    return httpd_resp_send_chunk(req, nullptr, 0);
}

/// POST /api/userdata ← a backup → {"ok":true,"staged":N}; the panel takes it at its next boot.
esp_err_t userdata_post(httpd_req_t* req) {
    if (!modesp::http_routes::authorized(req)) return ESP_OK;
    if (req->content_len <= 0 || (size_t)req->content_len > BODY_MAX) {
        return send_json(req, "413 Content Too Large", "{\"error\":\"size\"}");
    }
    Buf body((size_t)req->content_len + 1);
    if (!body.p) return send_json(req, "503 Service Unavailable", "{\"error\":\"no_memory\"}");
    if (!modesp::http_routes::read_body(req, body.p)) return ESP_FAIL;
    const size_t got = req->content_len;

    constexpr size_t MAX = 8;
    modesp::user_backup::Entry e[MAX];
    size_t n = 0;
    if (!modesp::user_backup::parse(body.p, got, e, MAX, n)) {
        return send_json(req, "400 Bad Request", "{\"error\":\"not_a_backup\"}");
    }

    // Every file read and checked first; only then is anything staged.
    Buf* text[MAX] = {};
    size_t len[MAX] = {};
    const char* bad = nullptr;           // the file that failed its check
    bool no_memory = false;
    for (size_t i = 0; i < n && !bad && !no_memory; i++) {
        text[i] = new Buf(e[i].len + 1);       // unescaping never makes a string longer
        if (!text[i]->p) { no_memory = true; break; }
        // A file over what its reader takes is refused, not staged: it would be read cut
        // short or never installed, after a reply that said ok (deep review CO-3).
        const bool json = modesp::user_backup::is_json_file(e[i].name);
        if (!modesp::user_backup::unescape(e[i].text, e[i].len, text[i]->p, text[i]->n, len[i]) ||
            (json && !modesp::user_backup::is_json_object(text[i]->p, len[i])) ||
            len[i] > modesp::user_backup::max_bytes(e[i].name)) {
            bad = e[i].name;
        }
    }

    // Staged all together or not at all: a half-staged backup would go in half at the next boot.
    static char staged_path[MAX][176];       // one caller, the server's task; not its stack
    memset(staged_path, 0, sizeof(staged_path));
    size_t staged = 0;
    const char* failed = nullptr;
    for (size_t i = 0; i < n && !bad && !no_memory; i++) {
        char path[160];
        modesp::UserStore::write_path(e[i].name, path, sizeof(path));
        snprintf(staged_path[i], sizeof(staged_path[i]), "%s%s", path, modesp::UserStore::STAGED);
        if (!safe_file::save(staged_path[i], text[i]->p, len[i])) { failed = e[i].name; break; }
        staged++;
    }
    if (failed) {
        for (size_t i = 0; i < staged; i++) remove(staged_path[i]);
        staged = 0;
    }
    for (size_t i = 0; i < n; i++) delete text[i];

    char reply[96];
    if (no_memory) return send_json(req, "503 Service Unavailable", "{\"error\":\"no_memory\"}");
    if (bad) {
        snprintf(reply, sizeof(reply), "{\"error\":\"bad_file\",\"file\":\"%s\"}", bad);
        return send_json(req, "400 Bad Request", reply);
    }
    if (failed) {
        ESP_LOGE(TAG, "restore not staged: %s could not be written", failed);
        snprintf(reply, sizeof(reply), "{\"error\":\"write\",\"file\":\"%s\"}", failed);
        return send_json(req, "500 Internal Server Error", reply);
    }
    ESP_LOGW(TAG, "%u file(s) from a backup staged; they go in at the next boot", (unsigned)staged);
    snprintf(reply, sizeof(reply), "{\"ok\":true,\"staged\":%u}", (unsigned)staged);
    return send_json(req, "200 OK", reply);
}

void register_userdata_routes(httpd_handle_t server, void*) {
    httpd_uri_t get{};
    get.uri = "/api/userdata";
    get.method = HTTP_GET;
    get.handler = userdata_get;
    httpd_uri_t post{};
    post.uri = "/api/userdata";
    post.method = HTTP_POST;
    post.handler = userdata_post;
    if (httpd_register_uri_handler(server, &get) != ESP_OK ||
        httpd_register_uri_handler(server, &post) != ESP_OK) {
        ESP_LOGW(TAG, "/api/userdata unavailable: no backup from the web");
    }
}

}  // namespace

void userdata_http_add_routes() {
    if (!modesp::http_routes::add(register_userdata_routes)) {
        ESP_LOGW(TAG, "no room in the route table — /api/userdata will 404");
    }
}
