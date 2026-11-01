// bt_http.inc — the satellite's Bluetooth settings for the web UI: GET and POST /api/bt.
//
// Included into bt_module.cpp's anonymous namespace (as radio's stations_http.inc is into its
// module).
//
// The web asks for what the panel's Bluetooth page asks for, through the same queue
// (bt_settings.h): the handlers run on the HTTP server's task and only read the view and put
// requests in; the module's main-loop tick puts them on the wire. A pairing answer goes in
// with Source::WEB, and the first answer to a request — from here or from the panel — is the
// one sent; a later one gets 409.
//
// The scratch below is static: the HTTP server runs its handlers one at a time on one task,
// and a view is ~700 bytes of that task's 8 KB stack.

/// Sixteen phones with names escaped at worst six bytes a byte would not fit; at their usual
/// ASCII they take ~1.5 KB.
constexpr size_t BT_HTTP_MAX = 4096;

esp_err_t bt_send(httpd_req_t* req, const char* status, const char* body) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

/// GET /api/bt (bt_web.h)
esp_err_t bt_get(httpd_req_t* req) {
    if (!modesp::http_routes::authorized(req)) return ESP_OK;
    static bt_settings::View view;
    static char json[BT_HTTP_MAX];
    bt_settings::snapshot(view);
    bt_settings::Pairing pairing;
    bt_settings::pairing(pairing);
    if (!bt_web::json(view, pairing, bt_settings::confirmer(), json, sizeof(json))) {
        return bt_send(req, "500 Internal Server Error", "{\"error\":\"too_large\"}");
    }
    return bt_send(req, "200 OK", json);
}

/// POST /api/bt ← the switches, a name, a phone to forget, a disconnect, a refresh, or a
/// pairing answer (bt_web.h) → {"ok":true}; 409 for a pairing already answered or moved on.
esp_err_t bt_post(httpd_req_t* req) {
    if (!modesp::http_routes::authorized(req)) return ESP_OK;
    static char body[512];
    if (req->content_len <= 0 || (size_t)req->content_len >= sizeof(body)) {
        return bt_send(req, "413 Content Too Large", "{\"error\":\"size\"}");
    }
    if (!modesp::http_routes::read_body(req, body)) return ESP_FAIL;
    const size_t got = req->content_len;

    static bt_web::Edit e;
    if (!bt_web::parse(body, got, e)) return bt_send(req, "400 Bad Request", "{\"error\":\"bad_request\"}");

    // What the satellite is: the same gates the panel's page draws its rows by.
    static bt_settings::View view;
    bt_settings::snapshot(view);
    if (!view.linked) return bt_send(req, "409 Conflict", "{\"error\":\"no_satellite\"}");
    if (e.needs_settings() && !view.settings) {
        return bt_send(req, "409 Conflict", "{\"error\":\"old_satellite\"}");
    }

    if (e.pair) {
        bt_settings::Pairing p;
        bt_settings::pairing(p);
        if (p.kind == sat::PAIR_PASSKEY) {
            return bt_send(req, "409 Conflict", "{\"error\":\"passkey\"}");   // the phone decides that one
        }
        if (!bt_settings::request_pair_reply(e.seq, e.accept, bt_settings::Source::WEB)) {
            bt_settings::pairing(p);
            char reply[64];
            snprintf(reply, sizeof(reply), "{\"error\":\"answered\",\"by\":%u}", (unsigned)p.by);
            return bt_send(req, "409 Conflict", reply);
        }
    }
    // BT_CONFIG_SET carries the whole state, a rename too: what was not given stays as the
    // satellite last said, so nothing goes before it has said it. SBC is always on
    // (screen_bt.cpp, "SBC, always").
    bt_settings::Config c;
    bt_settings::config(c);
    if ((e.any_switch() || e.rename) && !c.known) {
        return bt_send(req, "409 Conflict", "{\"error\":\"not_known_yet\"}");
    }
    if (e.any_switch()) {
        bt_settings::request_config(e.radio >= 0 ? e.radio != 0 : c.radio,
                                    e.discoverable >= 0 ? e.discoverable != 0 : c.discoverable,
                                    e.auto_reconnect >= 0 ? e.auto_reconnect != 0 : c.auto_reconnect,
                                    (uint8_t)((e.codecs >= 0 ? e.codecs : c.codecs_enabled) | 1u));
    }
    if (e.rename && !bt_settings::request_name(e.name)) {
        return bt_send(req, "400 Bad Request", "{\"error\":\"name\"}");
    }
    if (e.forget) bt_settings::request_forget(e.forget_all ? nullptr : e.forget_addr);
    if (e.disconnect) bt_settings::request_disconnect();
    if (e.refresh) bt_settings::request_refresh();
    return bt_send(req, "200 OK", "{\"ok\":true}");
}

/// Called by HttpService in the window before the static wildcard (modesp/net/http_routes.h).
void register_bt_routes(httpd_handle_t server, void*) {
    httpd_uri_t get{};
    get.uri = "/api/bt";
    get.method = HTTP_GET;
    get.handler = bt_get;
    httpd_uri_t post{};
    post.uri = "/api/bt";
    post.method = HTTP_POST;
    post.handler = bt_post;
    if (httpd_register_uri_handler(server, &get) != ESP_OK ||
        httpd_register_uri_handler(server, &post) != ESP_OK) {
        ESP_LOGW(TAG, "/api/bt unavailable: the web cannot change Bluetooth or answer a pairing");
    }
}
