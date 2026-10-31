/**
 * @file catalog.cpp
 * @brief The station catalog on the device: a task that asks Radio Browser, and the
 *        RadioModule side that turns states into requests and pages into states.
 *
 * Plain HTTP, never TLS. The panel holds one TLS session at a time, and while the radio
 * plays it belongs to the station (docs/NETWORK.md, "Internet radio"); what comes back here is a page
 * of metadata, not audio. Measured 04.10.2026: the service answers on port 80 with no
 * redirect, and it is down to one server, so the list of servers is asked for rather than
 * written down — two other ESP32 radios still point at mirrors that no longer exist.
 *
 * The request side runs whatever the source is: browsing does not need the radio to be
 * playing, and choosing a row is what makes it the source.
 *
 * Stage 20а.
 */

#include "radio_module.h"
#include "radio_catalog.h"
#include "radio_catalog_task.h"
#include "radio_catalog_ui.h"
#include "state_enums.h"   // audio.source values, generated from the manifests
#include "stations.h"

#include "esp_heap_caps.h"
#include "radio_http.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>

namespace rc = radio_catalog;

namespace {

const char* TAG = "Radio";   // the module's tag: it is on the INFO allowlist (log_policy.cpp)

constexpr const char* BOOTSTRAP_HOST = "all.api.radio-browser.info";
/// A 24-station page is ~27 KB (05.10.2026); room for twice that and a margin.
constexpr size_t  BODY_MAX     = 64 * 1024;
constexpr int     MAX_HOSTS    = 8;
constexpr int     PATH_LEN     = 320;
constexpr int     CACHE_PAGES  = 6;
constexpr int64_t HOSTS_TTL_US = 24LL * 3600 * 1000000;
constexpr int64_t PAGE_TTL_US  = 10LL * 60 * 1000000;
/// Not more than two requests a second: a double tap or "back, forward" must not be a
/// new request each time. The cache answers most of those before this is needed.
constexpr int64_t MIN_GAP_US   = 500 * 1000;
/// Most stations first, so a list cut at MAX_COUNTRIES loses the emptiest countries.
constexpr const char* COUNTRIES_PATH = "/json/countrycodes?hidebroken=true&order=stationcount&reverse=true";
constexpr int64_t COUNTRIES_TTL_US = HOSTS_TTL_US;
/// In the web bundle's folder since 20д, where the browser's catalog page reads the same file;
/// a data partition from before that has it one level up.
constexpr const char* GENRES_FILE  = "/data/www/radio_genres.json";
constexpr const char* GENRES_OLD   = "/data/radio_genres.json";
constexpr size_t  GENRES_MAX   = 8 * 1024;

struct Request {
    rc::Query q;
    uint32_t  serial = 0;
    int       page   = 0;
};

struct Result {
    uint32_t serial   = 0;
    bool     ok       = false;
    char     error[64] = "";
    uint32_t consumed = 0;   ///< server rows the page used up: the next page starts after them
    uint32_t ms       = 0;
    // What the states need about the page, so the main task never copies the page itself:
    // it is ~4 KB, and that task's stack is 8 KB.
    int      count    = 0;
    int      page     = 0;
    bool     more     = false;
};

struct CacheEntry {
    char     path[PATH_LEN];
    int64_t  at_us;
    rc::Page page;
    uint32_t consumed;
};

struct CatalogTask {
    QueueHandle_t     q    = nullptr;
    QueueHandle_t     clicks = nullptr;           // uuids whose play is to be counted
    SemaphoreHandle_t lock = nullptr;
    Result            result;                     // under lock
    char              hosts[MAX_HOSTS][rc::HOST_LEN] = {};
    int               host_count  = 0;
    int               host_next   = 0;
    int64_t           hosts_at_us = 0;
    int64_t           last_request_us = 0;
    char*             body  = nullptr;            // PSRAM, BODY_MAX
    rc::Page*         work  = nullptr;            // PSRAM: ~4 KB, too big for a task stack
    CacheEntry*       cache = nullptr;            // PSRAM, CACHE_PAGES
    int               cache_next = 0;
    rc::Countries*    countries = nullptr;        // PSRAM: the list being built
    int64_t           countries_at_us = 0;        // when the published list was fetched
    std::atomic<bool> want_countries{false};      // the screen opened its countries
};
CatalogTask t;

void publish(const Result& r) {
    xSemaphoreTake(t.lock, portMAX_DELAY);
    t.result = r;
    xSemaphoreGive(t.lock);
}

/// GET http://host/path into t.body (see radio_http.h). Returns the body length, or −1.
int http_get(const char* host, const char* path, char* why, size_t why_sz) {
    return radio_http::get(host, 80, path, t.body, BODY_MAX, radio_http::user_agent(), why, why_sz);
}

void* scratch_for(size_t len) {
    const size_t need = rc::scratch_bytes(t.body, len);
    return need ? heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : nullptr;
}

/// The servers to ask, from the service itself, at most once a day. When the list cannot
/// be had, the bootstrap name stands in: all.api resolves to the same servers.
void ensure_hosts() {
    const int64_t now = esp_timer_get_time();
    if (t.host_count && now - t.hosts_at_us < HOSTS_TTL_US) return;
    char why[64] = "";
    const int n = http_get(BOOTSTRAP_HOST, "/json/servers", why, sizeof(why));
    int got = 0;
    if (n > 0) {
        const size_t need = rc::scratch_bytes(t.body, (size_t)n);
        if (void* s = need ? heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : nullptr) {
            got = rc::parse_servers(t.body, (size_t)n, s, need, t.hosts, MAX_HOSTS);
            heap_caps_free(s);
        }
    }
    if (got <= 0) {
        snprintf(t.hosts[0], rc::HOST_LEN, "%s", BOOTSTRAP_HOST);
        got = 1;
        t.hosts_at_us = now - HOSTS_TTL_US + 60LL * 1000000;   // ask for the list again in a minute
        ESP_LOGW(TAG, "catalog: no server list (%s), asking %s", n > 0 ? "unreadable" : why, BOOTSTRAP_HOST);
    } else {
        t.hosts_at_us = now;
        ESP_LOGI(TAG, "catalog: %d server(s), %s first", got, t.hosts[0]);
    }
    t.host_count = got;
    t.host_next  = (int)(esp_random() % (uint32_t)got);   // spread clients over the servers
}

/// GET `path` into t.body from the servers in turn, three at most, staying with the one that
/// answers; never two requests within MIN_GAP_US. The body length, or −1 with `why` set.
int ask_servers(const char* path, char* why, size_t why_sz) {
    const int64_t gap = esp_timer_get_time() - t.last_request_us;
    if (t.last_request_us && gap < MIN_GAP_US) vTaskDelay(pdMS_TO_TICKS((MIN_GAP_US - gap) / 1000) + 1);
    ensure_hosts();
    int n = -1;
    for (int attempt = 0; attempt < t.host_count && attempt < 3 && n < 0; attempt++) {
        const int h = (t.host_next + attempt) % t.host_count;
        t.last_request_us = esp_timer_get_time();
        n = http_get(t.hosts[h], path, why, why_sz);
        if (n < 0) ESP_LOGW(TAG, "catalog: %s", why);
        else       t.host_next = h;                       // stay with the one that answered
    }
    return n;
}

void serve(const Request& rq) {
    Result res;
    res.serial = rq.serial;
    const int64_t t0 = esp_timer_get_time();

    char path[PATH_LEN];
    if (!rc::search_path(rq.q, rc::FETCH_ROWS, path, sizeof(path))) {
        snprintf(res.error, sizeof(res.error), "request does not fit");
        publish(res);
        return;
    }

    for (int i = 0; i < CACHE_PAGES; i++) {
        const CacheEntry& e = t.cache[i];
        if (e.path[0] && !strcmp(e.path, path) && t0 - e.at_us < PAGE_TTL_US) {
            rc::Page& p = *t.work;
            p = e.page;
            p.index  = rq.page;
            p.serial = rq.serial;
            rc::catalog().set(p);
            res.ok       = true;
            res.consumed = e.consumed;
            res.count    = p.count;
            res.page     = p.index;
            res.more     = p.more;
            ESP_LOGI(TAG, "catalog: page %d from the cache, %d rows", rq.page, p.count);
            publish(res);
            return;
        }
    }

    char why[64] = "";
    const int n = ask_servers(path, why, sizeof(why));
    if (n < 0) {
        snprintf(res.error, sizeof(res.error), "%s", why);
        publish(res);
        return;
    }

    void* scratch = scratch_for((size_t)n);
    rc::Page& p = *t.work;
    p.count = 0;
    p.more  = false;
    rc::PageStats st;
    const int rows = scratch ? rc::parse_page(t.body, (size_t)n, scratch,
                                              rc::scratch_bytes(t.body, (size_t)n),
                                              rq.q.quality, p.row, rc::PAGE_ROWS, &st)
                             : -1;
    heap_caps_free(scratch);
    if (rows < 0) {
        snprintf(res.error, sizeof(res.error), "not a station list (%d B)", n);
        publish(res);
        return;
    }
    p.count  = rows;
    p.index  = rq.page;
    p.serial = rq.serial;
    p.more   = st.consumed < st.seen || st.seen >= rc::FETCH_ROWS;
    rc::catalog().set(p);

    CacheEntry& e = t.cache[t.cache_next];
    t.cache_next = (t.cache_next + 1) % CACHE_PAGES;
    snprintf(e.path, sizeof(e.path), "%s", path);
    e.at_us    = t0;
    e.page     = p;
    e.consumed = (uint32_t)st.consumed;

    res.ok       = true;
    res.consumed = (uint32_t)st.consumed;
    res.count    = p.count;
    res.page     = p.index;
    res.more     = p.more;
    res.ms       = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    ESP_LOGI(TAG, "catalog: page %d, %d of %d rows (unplayable %d, below the floor %d, "
                  "repeats %d), %d B in %u ms; internal heap %u",
             rq.page, rows, st.seen, st.unplayable, st.below, st.repeats, n, (unsigned)res.ms,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    publish(res);
}

/// The countries the screen offers: Radio Browser's list with a station count each, asked for
/// when the screen opens it and kept for a day. A failure is published as such and the next
/// opening asks again.
void serve_countries() {
    rc::Countries& c = *t.countries;
    if (!rc::countries().get(c)) return;              // this task is the only writer: never
    const int64_t t0 = esp_timer_get_time();
    if (c.state == rc::State::READY && t0 - t.countries_at_us < COUNTRIES_TTL_US) return;
    c.state = rc::State::LOADING;
    rc::countries().set(c);

    char why[64] = "";
    const int n = ask_servers(COUNTRIES_PATH, why, sizeof(why));
    int got = -1;
    if (n > 0) {
        if (void* scratch = scratch_for((size_t)n)) {
            got = rc::parse_countrycodes(t.body, (size_t)n, scratch, rc::scratch_bytes(t.body, (size_t)n),
                                         c.item, rc::MAX_COUNTRIES);
            heap_caps_free(scratch);
        }
    }
    if (got <= 0) {
        c.state = rc::State::ERROR;
        c.count = 0;
        ESP_LOGW(TAG, "catalog: no countries: %s", n > 0 ? "unreadable" : why);
    } else {
        c.count = rc::order_countries(c.item, got, "UA");
        c.state = rc::State::READY;
        t.countries_at_us = t0;
        ESP_LOGI(TAG, "catalog: %d countries of %d, %d B in %u ms", c.count, got, n,
                 (unsigned)((esp_timer_get_time() - t0) / 1000));
    }
    rc::countries().set(c);
}

struct Click {
    char uuid[rc::UUID_LEN];
};

/// GET /json/url/<uuid>: the service counts the play and answers with the stream's URL,
/// which is not needed here. A failure is logged and forgotten — a missed count is not
/// worth a retry loop.
void count_click(const Click& c) {
    ensure_hosts();
    char path[64];
    snprintf(path, sizeof(path), "/json/url/%s", c.uuid);
    char why[64] = "";
    const int n = http_get(t.hosts[t.host_next % t.host_count], path, why, sizeof(why));
    if (n < 0) ESP_LOGW(TAG, "catalog: click for %s not counted: %s", c.uuid, why);
    else       ESP_LOGI(TAG, "catalog: play counted for %s", c.uuid);
}

void catalog_task(void*) {
    Request rq;
    Click   ck;
    for (;;) {
        // Pages first; the countries and a click wait a quarter of a second at most.
        if (xQueueReceive(t.q, &rq, pdMS_TO_TICKS(250)) == pdTRUE) serve(rq);
        if (t.want_countries.exchange(false)) serve_countries();
        while (t.clicks && xQueueReceive(t.clicks, &ck, 0) == pdTRUE) count_click(ck);
    }
}

/// Queue, buffers and task, once. The task's stack is in PSRAM: it never writes flash, so
/// the cache-off restriction does not apply, and internal RAM is ESP-Hosted's.
bool catalog_start() {
    t.lock  = xSemaphoreCreateMutex();
    t.q     = xQueueCreate(1, sizeof(Request));
    t.clicks = xQueueCreate(4, sizeof(Click));
    t.body  = (char*)heap_caps_malloc(BODY_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    void* w = heap_caps_calloc(1, sizeof(rc::Page), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    t.work  = w ? new (w) rc::Page() : nullptr;
    t.cache = (CacheEntry*)heap_caps_calloc(CACHE_PAGES, sizeof(CacheEntry),
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    void* c = heap_caps_calloc(1, sizeof(rc::Countries), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    t.countries = c ? new (c) rc::Countries() : nullptr;
    if (!t.lock || !t.q || !t.clicks || !t.body || !t.work || !t.cache || !t.countries) return false;
    TaskHandle_t h = nullptr;
    return xTaskCreatePinnedToCoreWithCaps(catalog_task, "radio_cat", 8 * 1024, nullptr,
                                           tskIDLE_PRIORITY + 2, &h, 0,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS;
}

/// Only the latest request matters: a newer one replaces one not yet started.
void catalog_post(const rc::Query& q, uint32_t serial, int page) {
    Request rq;
    rq.q = q;
    rq.serial = serial;
    rq.page = page;
    xQueueOverwrite(t.q, &rq);
}

bool catalog_result(Result& out) {
    if (!t.lock) return false;
    xSemaphoreTake(t.lock, portMAX_DELAY);
    out = t.result;
    xSemaphoreGive(t.lock);
    return true;
}

/// The page in one line for the web UI and MQTT: "1. Hit FM · MP3 128 | 2. …".
void summarise(int count, char* out, size_t out_sz) {
    size_t n = 0;
    out[0] = '\0';
    for (int i = 0; i < count && n + 1 < out_sz; i++) {
        rc::Row r;                       // one row at a time: nothing page-sized on the stack
        if (!rc::catalog().at(i, r)) break;
        char rate[12] = "";
        if (r.bitrate) snprintf(rate, sizeof(rate), " %u", (unsigned)r.bitrate);
        // At most 28 bytes of the name, cut before a character rather than inside one:
        // "%.28s" left half a Cyrillic letter on the bench ("Хіт FM Сучасні х\xD1").
        char name[32];
        size_t k = strlen(r.name);
        if (k > 28) {
            k = 28;
            while (k && ((unsigned char)r.name[k] & 0xC0) == 0x80) k--;
        }
        memcpy(name, r.name, k);
        name[k] = '\0';
        const int w = snprintf(out + n, out_sz - n, "%s%d. %s · %s%s",
                               i ? " | " : "", i, name, r.codec, rate);
        if (w < 0) break;
        n += (size_t)w;
    }
    if (n >= out_sz) {
        // Cut at a character, not inside one, and say that it was cut.
        size_t k = out_sz - 4;
        while (k && ((unsigned char)out[k] & 0xC0) == 0x80) k--;
        memcpy(out + k, "…", 3);
        out[k + 3] = '\0';
    }
}

/// data/radio_genres.json, once, for the screen: the genres are data, edited without a build.
/// A missing or broken file leaves the screen's genres empty, and says so.
void load_genres() {
    char* buf = (char*)heap_caps_malloc(GENRES_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    void* mem = heap_caps_calloc(1, sizeof(rc::Genres), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    rc::Genres* g = mem ? new (mem) rc::Genres() : nullptr;
    size_t n = 0;
    const char* path = GENRES_FILE;
    if (buf && g) {
        FILE* f = fopen(GENRES_FILE, "rb");
        if (!f) {
            path = GENRES_OLD;
            f = fopen(GENRES_OLD, "rb");
        }
        if (f) {
            n = fread(buf, 1, GENRES_MAX - 1, f);
            fclose(f);
        }
        buf[n] = '\0';
    }
    int got = -1;
    if (n) {
        const size_t need = rc::scratch_bytes(buf, n);
        if (void* s = need ? heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : nullptr) {
            got = rc::parse_genres(buf, n, s, need, g->item, rc::MAX_GENRES);
            heap_caps_free(s);
        }
    }
    if (got > 0) {
        g->count = got;
        rc::genres().set(*g);
        ESP_LOGI(TAG, "catalog: %d genres from %s", got, path);
    } else {
        ESP_LOGW(TAG, "catalog: no genres: %s %s", GENRES_FILE, n ? "is not a genre list" : "is missing");
    }
    heap_caps_free(mem);
    heap_caps_free(buf);
}

/// What the screen is told about the request, kept beside the states it mirrors.
rc::Status s_view;

}  // namespace

void RadioModule::catalog_update(uint32_t /*dt_ms*/) {
    // radio.cat_state and radio.cat_error, and the screen's copy of them.
    const auto set_state = [this](rc::State st, const char* error) {
        static const char* const NAMES[] = {"idle", "loading", "ready", "error"};
        state_set("radio.cat_state", NAMES[(int)st]);
        s_view.state = st;
        if (error) {
            state_set("radio.cat_error", error);
            snprintf(s_view.error, sizeof(s_view.error), "%s", error);
        }
    };

    if (!cat_started_) {
        cat_started_ = true;
        state_set("radio.cat_query", "");
        state_set("radio.cat_next", false);
        state_set("radio.cat_prev", false);
        state_set("radio.cat_play", (int32_t)-1);
        state_set("radio.cat_fav", (int32_t)-1);
        state_set("radio.fav_add", false);
        state_set("radio.cat_page", (int32_t)0);
        state_set("radio.cat_count", (int32_t)0);
        state_set("radio.cat_more", false);
        state_set("radio.cat_list", "");
        state_set("radio.cat_error", "");
        const bool ok = catalog_start();
        set_state(ok ? rc::State::IDLE : rc::State::ERROR, ok ? nullptr : "no memory for the catalog");
        if (!ok) ESP_LOGE(TAG, "catalog: could not start");
        load_genres();
    }

    // ── the panel's screen: its asks become the states the web page writes, so both go the
    // same way from here (radio_catalog_ui.h). A row is acted on only if the page the screen
    // showed is still the one in the store. ──
    rc::Ask ask;
    while (rc::asks().take(ask)) {
        rc::Row  r;
        uint32_t serial = 0;
        const bool row_ok = (ask.kind == rc::AskKind::PLAY || ask.kind == rc::AskKind::KEEP) &&
                            rc::catalog().at(ask.value, r, serial) && serial == ask.serial;
        switch (ask.kind) {
        case rc::AskKind::QUERY:
            // Asked again is asked again: after an error, or to see a fresher page. The page
            // cache answers a repeat within ten minutes without the network.
            if (!strcmp(ask.spec, cat_spec_)) cat_spec_[0] = '\0';
            state_set("radio.cat_query", ask.spec);
            break;
        case rc::AskKind::SORT:    state_set("radio.cat_sort", ask.value); break;
        case rc::AskKind::QUALITY: state_set("radio.cat_quality", ask.value); break;
        case rc::AskKind::NEXT:    state_set("radio.cat_next", true); break;
        case rc::AskKind::PREV:    state_set("radio.cat_prev", true); break;
        case rc::AskKind::PLAY:
            if (row_ok) {
                ESP_LOGI(TAG, "catalog: play row %d, %s (%s %u)", (int)ask.value, r.name, r.codec,
                         (unsigned)r.bitrate);
                play_catalog(r.name, r.url, r.uuid, r.favicon);
            }
            break;
        case rc::AskKind::KEEP:
            if (row_ok && add_station(r.name, r.url, r.uuid, ask.slot) < 0) {
                state_set("radio.cat_error", "the station list is full (64)");
            }
            break;
        case rc::AskKind::KEEP_PLAYING:
            if (keep_playing_station(ask.slot) < 0) ESP_LOGI(TAG, "catalog: nothing to keep, or the list is full");
            break;
        case rc::AskKind::COUNTRIES:
            t.want_countries.store(true);
            break;
        }
    }

    // ── requests ──
    char spec[sizeof(cat_spec_)] = "";
    read_string("radio.cat_query", spec, sizeof(spec));
    const int32_t sort    = read_int("radio.cat_sort", 0);
    const int32_t quality = read_int("radio.cat_quality", 1);
    bool post = false;
    int  page = cat_page_;
    if (strcmp(spec, cat_spec_) || sort != cat_sort_ || quality != cat_quality_) {
        snprintf(cat_spec_, sizeof(cat_spec_), "%s", spec);
        cat_sort_    = sort;
        cat_quality_ = quality;
        page = 0;
        cat_offset_[0] = 0;
        post = spec[0] != '\0';
        if (!post) set_state(rc::State::IDLE, nullptr);
    }
    if (read_bool("radio.cat_next", false)) {
        state_set("radio.cat_next", false);
        if (cat_spec_[0] && read_bool("radio.cat_more", false) && cat_page_ + 1 < CAT_PAGES &&
            cat_offset_[cat_page_ + 1] > cat_offset_[cat_page_]) {
            page = cat_page_ + 1;
            post = true;
        }
    }
    if (read_bool("radio.cat_prev", false)) {
        state_set("radio.cat_prev", false);
        if (cat_spec_[0] && cat_page_ > 0) {
            page = cat_page_ - 1;
            post = true;
        }
    }
    if (post) {
        rc::Query q;
        if (!rc::parse_spec(cat_spec_, q)) {
            set_state(rc::State::ERROR, "unknown request; try top:UA, tag:jazz, name:kiss");
        } else {
            q.sort    = (rc::Sort)(sort < 0 ? 0 : sort > 3 ? 3 : sort);
            q.quality = (rc::Quality)(quality < 0 ? 0 : quality > 4 ? 4 : quality);
            q.offset  = cat_offset_[page];
            cat_page_ = page;
            catalog_post(q, ++cat_serial_, page);
            set_state(rc::State::LOADING, nullptr);
            state_set("radio.cat_page", (int32_t)page);
            ESP_LOGI(TAG, "catalog: ask \"%s\", sort %d, quality %d, page %d (offset %u)",
                     cat_spec_, (int)q.sort, (int)q.quality, page, (unsigned)q.offset);
        }
    }

    // ── results: only the one for the latest request ──
    Result res;
    if (catalog_result(res) && res.serial && res.serial != cat_seen_ && res.serial == cat_serial_) {
        cat_seen_ = res.serial;
        if (res.ok) {
            if (cat_page_ + 1 < CAT_PAGES) cat_offset_[cat_page_ + 1] = cat_offset_[cat_page_] + res.consumed;
            char list[256];
            summarise(res.count, list, sizeof(list));
            state_set("radio.cat_count", (int32_t)res.count);
            state_set("radio.cat_more", res.more);
            state_set("radio.cat_page", (int32_t)res.page);
            state_set("radio.cat_list", list);
            set_state(rc::State::READY, "");
            s_view.more = res.more;
        } else {
            set_state(rc::State::ERROR, res.error);
            ESP_LOGW(TAG, "catalog: %s", res.error);
        }
    }

    // ── a row chosen: played as a station — it reconnects, it has a name — and takes the
    // output. Not as a pushed URL, which gives up on the first dropout. ──
    const int32_t row = read_int("radio.cat_play", -1);
    if (row >= 0) {
        state_set("radio.cat_play", (int32_t)-1);
        rc::Row r;
        if (rc::catalog().at(row, r)) {
            ESP_LOGI(TAG, "catalog: play row %d, %s (%s %u)", (int)row, r.name, r.codec,
                     (unsigned)r.bitrate);
            play_catalog(r.name, r.url, r.uuid, r.favicon);
        }
    }

    // ── a row kept: into the list, with its uuid ──
    const int32_t fav = read_int("radio.cat_fav", -1);
    if (fav >= 0) {
        state_set("radio.cat_fav", (int32_t)-1);
        rc::Row r;
        if (rc::catalog().at(fav, r)) {
            const int idx = add_station(r.name, r.url, r.uuid);
            if (idx < 0) state_set("radio.cat_error", "the station list is full (64)");
        }
    }

    // ── a station chosen on the browser's catalog page (data/www/radio.html, stage 20д). The
    // page asks Radio Browser itself; all that reaches the panel is the name, the stream and
    // the uuid, written in that order before the act, in one request. The logo is looked up
    // by the uuid, as for a station from the list. ──
    const int32_t web = read_int("radio.web_act", -1);
    if (web >= 0) {
        state_set("radio.web_act", (int32_t)-1);
        char name[rc::NAME_LEN] = "";
        char url[MODESP_MAX_LONG_STRING_LENGTH] = "";
        char uuid[rc::UUID_LEN] = "";
        read_string("radio.web_name", name, sizeof(name));
        read_string("radio.web_url", url, sizeof(url));
        read_string("radio.web_uuid", uuid, sizeof(uuid));
        radio_core::tidy_name(name);   // one line, as a row of the panel's own catalog
        const bool usable = (!strncmp(url, "http://", 7) || !strncmp(url, "https://", 8)) &&
                            strlen(url) < (size_t)rc::URL_LEN;
        const char* label = name[0] ? name : url;
        if (!usable) {
            state_set("radio.cat_error", "the browser page sent no usable stream URL");
            ESP_LOGW(TAG, "catalog: browser page: no usable URL for \"%s\"", name);
        } else if (web == 0) {
            ESP_LOGI(TAG, "catalog: browser page: play %s", label);
            play_catalog(label, url, uuid, "");
        } else if (add_station(label, url, uuid, web >= 2 ? (int)web - 2 : -1) < 0) {
            state_set("radio.cat_error", "the station list is full (64)");
        }
    }

    // ── the catalog station that is playing, kept ──
    if (read_bool("radio.fav_add", false)) {
        state_set("radio.fav_add", false);
        if (keep_playing_station() < 0) ESP_LOGI(TAG, "catalog: nothing to keep, or the list is full");
    }

    // ── the screen's copy of the request, published when it changed ──
    rc::Status v = s_view;
    snprintf(v.spec, sizeof(v.spec), "%s", cat_spec_);
    v.sort    = (rc::Sort)(cat_sort_ < 0 ? 0 : cat_sort_ > 3 ? 3 : cat_sort_);
    v.quality = (rc::Quality)(cat_quality_ < 0 ? 0 : cat_quality_ > 4 ? 4 : cat_quality_);
    v.page    = cat_page_;
    v.serial  = cat_serial_;
    if (v.state != rc::State::READY) v.more = false;
    static rc::Status published;
    static bool       once = false;
    if (!once || memcmp(&v, &published, sizeof(v)) != 0) {
        once = true;
        published = v;
        rc::status().set(v);
    }
}

namespace radio_catalog_task {

void click(const char* uuid) {
    if (!t.clicks || !uuid || !uuid[0]) return;
    Click c;
    snprintf(c.uuid, sizeof(c.uuid), "%s", uuid);
    xQueueSend(t.clicks, &c, 0);
}

}  // namespace radio_catalog_task
