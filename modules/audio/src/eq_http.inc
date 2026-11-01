// eq_http.inc — the EQ for the web UI: GET and POST /api/eq.
//
// Included into audio_module.cpp's anonymous namespace (as radio's stations_http.inc is into
// its module).
//
// The document is audio_dsp's EqStore, and the bodies are audio_dsp's eq_web (host-tested).
// These handlers run on the HTTP server's task and only put edits into the store; the module
// hears them on the main task, hands the bands to the audio core and writes the files
// (AudioModule::own_eq). An edit names the revision it was made from, and one made from an
// older revision is refused with 409 and the current one — an edit on the panel meanwhile is
// never silently undone.
//
// The scratch below is static: the HTTP server runs its handlers one at a time on one task,
// and a document is over a kilobyte of that task's 8 KB stack.

/// A whole band set is about 800 bytes; room for the envelope and stray whitespace.
constexpr size_t EQ_HTTP_MAX = 2048;

using modesp::http_routes::send_json;

/// GET /api/eq → {"rev","fs","preset","presets","bands"} (eq_web.h)
esp_err_t eq_get(httpd_req_t* req) {
    if (!modesp::http_routes::authorized(req)) return ESP_OK;
    audio_core::EqStore& st = audio_core::eq_store();
    if (!st.loaded()) return send_json(req, "503 Service Unavailable", "{\"error\":\"not_loaded\"}");
    static audio_core::EqDoc doc;
    static char json[EQ_HTTP_MAX];
    const uint32_t rev = st.get(doc);
    if (!audio_core::eq_web_json(doc, rev, audio_core::fs_hz(), json, sizeof(json))) {
        return send_json(req, "500 Internal Server Error", "{\"error\":\"too_large\"}");
    }
    return send_json(req, "200 OK", json);
}

/// POST /api/eq ← {"rev",bands} | {"rev",preset} | {"keep":true}  → {"rev":N}, or 409
/// An edit with "keep":true is written at once, like a keep after it: the page's last word.
esp_err_t eq_post(httpd_req_t* req) {
    if (!modesp::http_routes::authorized(req)) return ESP_OK;
    if (req->content_len <= 0 || (size_t)req->content_len >= EQ_HTTP_MAX) {
        return send_json(req, "413 Content Too Large", "{\"error\":\"size\"}");
    }
    static char body[EQ_HTTP_MAX];
    if (!modesp::http_routes::read_body(req, body)) return ESP_FAIL;
    const size_t got = req->content_len;

    static audio_core::EqWebEdit edit;
    if (!audio_core::eq_web_parse(body, got, edit)) {
        return send_json(req, "400 Bad Request", "{\"error\":\"bad_edit\"}");
    }
    audio_core::EqStore& st = audio_core::eq_store();
    if (!st.loaded()) return send_json(req, "503 Service Unavailable", "{\"error\":\"not_loaded\"}");

    uint32_t rev = 0;
    switch (edit.kind) {
    case audio_core::EqWebEdit::KEEP:   st.keep(); rev = st.revision(); break;
    case audio_core::EqWebEdit::BANDS:  rev = st.put_bands(edit.bands, edit.count, edit.rev); break;
    case audio_core::EqWebEdit::PRESET: rev = st.choose(edit.preset, edit.rev); break;
    default: break;
    }
    char reply[64];
    if (!rev) {
        snprintf(reply, sizeof(reply), "{\"error\":\"changed\",\"rev\":%lu}", (unsigned long)st.revision());
        return send_json(req, "409 Conflict", reply);
    }
    // a drag sends an edit per step: the write waits for it to stop (own_eq)
    if (edit.kind != audio_core::EqWebEdit::KEEP) {
        st.web_touched(esp_timer_get_time() / 1000);
        if (edit.keep) st.keep();
    }
    snprintf(reply, sizeof(reply), "{\"rev\":%lu}", (unsigned long)rev);
    return send_json(req, "200 OK", reply);
}

/// Called by HttpService in the window before the static wildcard (modesp/net/http_routes.h).
void register_eq_routes(httpd_handle_t server, void*) {
    httpd_uri_t get{};
    get.uri = "/api/eq";
    get.method = HTTP_GET;
    get.handler = eq_get;
    httpd_uri_t post{};
    post.uri = "/api/eq";
    post.method = HTTP_POST;
    post.handler = eq_post;
    if (httpd_register_uri_handler(server, &get) != ESP_OK ||
        httpd_register_uri_handler(server, &post) != ESP_OK) {
        ESP_LOGW(TAG, "/api/eq unavailable: the web cannot edit the EQ");
    }
}
