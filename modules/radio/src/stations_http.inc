// stations_http.inc — the station list for the web UI: GET and POST /api/stations.
//
// Included into radio_module.cpp's anonymous namespace (as dlna_module.cpp includes its
// .inc files): it needs the task state `s`, save_stations() and cat_ask(), and nothing
// outside this module needs it.
//
// Who writes what. The list belongs to this module and is changed on the main task —
// add_station(), the screen's preset store, and now an edit from the web. The handlers below
// run on the HTTP server's task, so they never touch the list: GET copies the shared store
// (StationStore::snapshot), POST parses the new list into a mailbox and leaves it for
// apply_station_edit(), which on_update() calls on the main task.
//
// An edit says which generation of the list it was made from. If the list moved since — a
// preset stored on the panel, a station kept from the catalog — the edit is refused (409 at
// once, or dropped on the main task if the move came in between), and the web reloads the list
// rather than write over somebody else's change. radio.list_rev publishes the generation, so
// the web learns of every change, its own included, without polling.

/// One edit, waiting for the main task. In PSRAM: a full list is 21 KB.
struct StationEdit {
    radio_core::StationList list;
    uint32_t base = 0;            ///< generation the edit was made from
};
enum : uint8_t { EDIT_IDLE, EDIT_POSTED };
std::atomic<uint8_t> s_edit_state{EDIT_IDLE};

/// Made on the first POST, kept after: the next edit needs it again. Null without memory.
StationEdit* station_edit() {
    static StationEdit* p = [] {
        void* mem = heap_caps_calloc_prefer(1, sizeof(StationEdit), 2,
                                            MALLOC_CAP_SPIRAM  | MALLOC_CAP_8BIT,
                                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        return mem ? new (mem) StationEdit() : nullptr;
    }();
    return p;
}

/// The index `idx` of the old list has in the new one, found by URL — what survives a rename,
/// as StationList::find says. −1 when it is gone; TRANSIENT and −1 pass through.
int32_t moved_to(int32_t idx, const radio_core::StationList& to) {
    const radio_core::Station* st = s.list.at(idx);
    return st ? to.find(st->url) : idx;
}

/// Main task, from on_update(): put a posted edit in place. True when it did; `station` is
/// then what radio.station must say, for the caller to publish.
///
/// The radio task copies a station's URL out of s.list when it tunes or reconnects
/// (station_url), and the indices it holds mean something else once stations move. So the
/// station that is on is first parked in the transient slot — the radio task then reads its
/// URL from there, the same URL — the list is copied, and only then is the station pointed at
/// its new index. The park and the copy happen under list_guard(), so the task's copy is of
/// the old list or the new one and never of half of each; it holds no pointer into either.
/// A station that was deleted while it played stays parked: it goes on playing, as a catalog
/// station would, until something else is tuned.
bool apply_station_edit(int32_t& station) {
    if (s_edit_state.load(std::memory_order_acquire) != EDIT_POSTED) return false;
    StationEdit& e = *station_edit();
    if (radio_core::stations().generation() != e.base) {
        ESP_LOGW(TAG, "station edit from the web dropped: the list changed since it was read");
        s_edit_state.store(EDIT_IDLE, std::memory_order_release);
        return false;
    }

    const int32_t cur = s.station.load(std::memory_order_acquire);
    const int32_t tuned = s.tuned.load(std::memory_order_acquire);
    const int32_t cur_new = moved_to(cur, e.list);
    const int32_t tuned_new = moved_to(tuned, e.list);
    const radio_core::Station* on = s.list.at(cur);

    xSemaphoreTake(list_guard(), portMAX_DELAY);
    if (on && s.up) {
        s.transient = *on;
        s.station.store(TRANSIENT, std::memory_order_release);
        s.tuned.store(-1, std::memory_order_release);
        if (cur_new < 0) cat_ask().shown = *on;      // deleted while on: shown as a catalog station
    }
    s.list = e.list;
    xSemaphoreGive(list_guard());
    station = cur;
    if (cur >= 0) {
        // Not playing, a deleted station is simply not the one to start from any more.
        station = cur_new >= 0 ? cur_new : (s.up ? TRANSIENT : 0);
        s.station.store(station, std::memory_order_release);
        // A tuned station that is gone is no longer "already on it": the next play retunes.
        s.tuned.store(tuned >= 0 ? tuned_new : tuned, std::memory_order_release);
    }
    radio_core::stations().set(s.list);
    save_stations();
    s_edit_state.store(EDIT_IDLE, std::memory_order_release);
    ESP_LOGI(TAG, "station list replaced from the web: %d stations", s.list.count);
    return true;
}

// ── HTTP ──

/// A whole station list is 22 KB at most (STATIONS_JSON_MAX); a little over that for the
/// envelope.
constexpr size_t STATIONS_HTTP_MAX = STATIONS_JSON_MAX + 64;

using modesp::http_routes::send_json;

/// GET /api/stations → {"rev":N,"list":{"stations":[...],"presets":[...]}}
esp_err_t stations_get(httpd_req_t* req) {
    if (!modesp::http_routes::authorized(req)) return ESP_OK;
    StationScratch list_mem(sizeof(radio_core::StationList));
    StationScratch json(STATIONS_HTTP_MAX);
    if (!list_mem.p || !json.p) return send_json(req, "503 Service Unavailable", "{\"error\":\"no_memory\"}");
    auto* list = new (list_mem.p) radio_core::StationList();
    uint32_t gen = 0;
    if (!radio_core::stations().snapshot(*list, gen)) {
        return send_json(req, "503 Service Unavailable", "{\"error\":\"busy\"}");
    }
    const int head = snprintf(json.p, json.n, "{\"rev\":%lu,\"list\":", (unsigned long)gen);
    const size_t w = radio_core::serialize(*list, json.p + head, json.n - head - 2);
    if (!w) return send_json(req, "500 Internal Server Error", "{\"error\":\"too_large\"}");
    json.p[head + w] = '}';
    json.p[head + w + 1] = '\0';
    return send_json(req, "200 OK", json.p);
}

/// POST /api/stations ← {"rev":N,"stations":[...],"presets":[...]}
///
/// The whole list, as the file holds it, with the generation it was made from in front. 202
/// when the main task has it to apply; 409 with the current generation when the list moved.
esp_err_t stations_post(httpd_req_t* req) {
    if (!modesp::http_routes::authorized(req)) return ESP_OK;
    if (req->content_len <= 0 || (size_t)req->content_len >= STATIONS_HTTP_MAX) {
        return send_json(req, "413 Content Too Large", "{\"error\":\"size\"}");
    }
    StationScratch body((size_t)req->content_len + 1);
    StationScratch list_mem(sizeof(radio_core::StationList));
    StationEdit* edit = station_edit();
    if (!body.p || !list_mem.p || !edit) {
        return send_json(req, "503 Service Unavailable", "{\"error\":\"no_memory\"}");
    }
    if (!modesp::http_routes::read_body(req, body.p)) return ESP_FAIL;
    const size_t got = req->content_len;

    // The generation leads the object, so it is read without walking the list: `{"rev":N,`.
    static constexpr char LEAD[] = "{\"rev\":";
    if (strncmp(body.p, LEAD, sizeof(LEAD) - 1) != 0) {
        return send_json(req, "400 Bad Request", "{\"error\":\"rev_first\"}");
    }
    char* end = nullptr;
    const unsigned long base = strtoul(body.p + sizeof(LEAD) - 1, &end, 10);
    if (end == body.p + sizeof(LEAD) - 1 || *end != ',') {
        return send_json(req, "400 Bad Request", "{\"error\":\"rev_first\"}");
    }

    auto* list = new (list_mem.p) radio_core::StationList();
    // An empty list is not accepted: on the next boot it would read as "no list" and the
    // built-in stations would replace it (load_stations). One station at least.
    if (!radio_core::parse(body.p, got, *list)) {
        return send_json(req, "400 Bad Request", "{\"error\":\"no_stations\"}");
    }
    char reply[64];
    const uint32_t gen = radio_core::stations().generation();
    if (gen != (uint32_t)base) {
        snprintf(reply, sizeof(reply), "{\"error\":\"changed\",\"rev\":%lu}", (unsigned long)gen);
        return send_json(req, "409 Conflict", reply);
    }
    // One edit at a time: the HTTP server is one task, and the main task takes a posted edit
    // within a tick, so a second one finding the first still there is answered "busy".
    if (s_edit_state.load(std::memory_order_acquire) != EDIT_IDLE) {
        return send_json(req, "503 Service Unavailable", "{\"error\":\"busy\"}");
    }
    edit->list = *list;
    edit->base = (uint32_t)base;
    s_edit_state.store(EDIT_POSTED, std::memory_order_release);
    return send_json(req, "202 Accepted", "{\"ok\":true}");
}

/// Called by HttpService in the window before the static wildcard (modesp/net/http_routes.h).
void register_station_routes(httpd_handle_t server, void*) {
    httpd_uri_t get{};
    get.uri = "/api/stations";
    get.method = HTTP_GET;
    get.handler = stations_get;
    httpd_uri_t post{};
    post.uri = "/api/stations";
    post.method = HTTP_POST;
    post.handler = stations_post;
    if (httpd_register_uri_handler(server, &get) != ESP_OK ||
        httpd_register_uri_handler(server, &post) != ESP_OK) {
        ESP_LOGW(TAG, "/api/stations unavailable: the web cannot edit the station list");
    }
}
