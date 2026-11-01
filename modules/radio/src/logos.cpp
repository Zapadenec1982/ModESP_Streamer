/**
 * @file logos.cpp
 * @brief The presets' logos, kept on the panel. See radio_logos.h; the rules are
 *        components/radio_catalog/include/station_logo.h.
 *
 * The main task's side: which stations are on presets, their files — read at start and when the
 * presets change, written for a logo that arrived, removed for a station that left the presets —
 * and the shelf the Player draws from. Files are the main task's because a LittleFS read or write
 * stops the flash cache, and only a task whose stack is in internal RAM may run then. The shelf
 * takes a logo at once; writing and removing wait until nothing has been heard for
 * station_logo::QUIET_MS (persist), since with music playing that cache stop is a click.
 *
 * The fetching is a task of its own, `radio_logo`, made for one batch and gone when it is done:
 * one Radio Browser request for the favicons of every station that has no logo yet, then each
 * favicon, decoded into the disc's 40 x 40 pixels by components/cover. Idle priority, as the art
 * task: the software decoder takes its caller's priority, and above idle a PNG starved the
 * stream. It is started only while the panel's one TLS session is free and no stream is starting
 * (radio_module.cpp, logos_may_fetch), and it looks again before every favicon: most are HTTPS.
 * A logo it cannot have — none listed, a dead address, ICO, SVG, WebP — is asked for again after
 * station_logo::RETRY_LATE_US, or at the next boot.
 */

#include "radio_logos.h"
#include "radio_catalog.h"
#include "radio_catalog_ui.h"
#include "radio_http.h"
#include "station_logo.h"
#include "stations.h"
#include "cover.h"
#include "safe_file.h"
#include "user_store.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <new>
#include <sys/stat.h>

namespace {

const char* TAG = "Radio";   // the module's tag: on the INFO allowlist
namespace sl = station_logo;
namespace rc = radio_catalog;

static_assert(sl::WANT == radio_core::PRESETS, "a logo per preset");

constexpr size_t   LOOKUP_MAX = 24 * 1024;    ///< six stations' records are ~10 KB
constexpr size_t   LOGO_MAX   = 128 * 1024;   ///< as the tile's (art.cpp): favicons are small
/// An HTTPS fetch and a JPEG on the engine; a PNG is decoded on the cover component's own task.
/// As radio_next's, which fetches through esp_http_client too.
constexpr uint32_t STACK      = 12 * 1024;
constexpr uint32_t SETTLE_MS  = 20000;        ///< the network up this long before a batch
constexpr uint32_t GAP_MS     = 30000;        ///< from one batch to the next

/// How a station's fetch went.
enum class Outcome : uint8_t {
    OK,         ///< the logo is in `px`
    LATE,       ///< not to be had: none listed, unreachable, not JPEG or PNG, not decodable
    SOON,       ///< Radio Browser did not answer the lookup
    DEFERRED,   ///< the TLS session was taken meanwhile: not tried
};

/// One batch. The main task fills it and starts the task; the task answers station by station,
/// in order, and says when it has let go. The main task frees it once every answer is in.
struct Job {
    int  count = 0;
    char uuid[sl::WANT][sl::UUID_LEN] = {};
    struct Out {
        Outcome  outcome = Outcome::SOON;
        uint16_t px[sl::PIXELS] = {};
    } out[sl::WANT];
    std::atomic<int>  done{0};          ///< answers written: task → main
    std::atomic<bool> running{false};   ///< the task still has the job
    std::atomic<bool> tls_free{false};  ///< main → task, every update
};

/// The task's buffers, one allocation, made and freed by the task.
struct Buffers {
    char        body[LOOKUP_MAX];
    uint8_t     img[LOGO_MAX];
    rc::Favicon fav[sl::WANT];
};

/// The main task's state. In PSRAM: a shelf, a logo and a file are 26 KB.
struct Logos {
    sl::Plan  plan;
    sl::Shelf shelf;                     ///< as last published; also what is still to be written
    sl::Thumb offered;                   ///< the art task's last logo
    uint16_t  px[sl::PIXELS] = {};       ///< a file's pixels, read
    uint8_t   file[sl::FILE_BYTES] = {}; ///< a file's bytes, read or written
    Job*      job = nullptr;
    int       taken = 0;                 ///< answers of `job` handled
    uint32_t  offer_seen = 0;
    uint32_t  list_gen = UINT32_MAX;
    uint32_t  net_ms = 0;                ///< how long the network has been up, up to SETTLE_MS
    uint32_t  gap_ms = GAP_MS;           ///< since the last batch began, up to GAP_MS
    sl::Quiet quiet;                     ///< whether the files may be touched
    bool      have_offer = false;
    bool      looked = false;            ///< the presets have been read once
    bool      sweep_due = false;         ///< files of stations off the presets may be left
    char      dir[48] = "";
};

/// One of each, in PSRAM, made on first use: internal RAM is the Wi-Fi co-processor's and the
/// I2S descriptors'. The offers by whichever task comes first — the art task or main.
template <typename T>
T& single() {
    static T* p = [] {
        void* mem = heap_caps_calloc_prefer(1, sizeof(T), 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        configASSERT(mem != nullptr);
        return new (mem) T();
    }();
    return *p;
}

rc::Published<sl::Thumb>& offers() { return single<rc::Published<sl::Thumb>>(); }

// ── the task ──

/// The favicon `uuid` has in a lookup's answer, or nullptr.
const char* favicon_of(const Buffers& b, int found, const char* uuid) {
    for (int i = 0; i < found; i++) if (!strcmp(b.fav[i].uuid, uuid)) return b.fav[i].url;
    return nullptr;
}

Outcome fetch_one(Job& j, int k, Buffers* b, int found) {
    if (found < 0) return Outcome::SOON;           // no answer to look in, or no memory for one
    const char* uuid = j.uuid[k];
    const char* url = favicon_of(*b, found, uuid);
    if (!url || !url[0]) {
        ESP_LOGI(TAG, "logos: %s lists no logo", uuid);
        return Outcome::LATE;
    }
    if (!j.tls_free.load(std::memory_order_acquire)) return Outcome::DEFERRED;
    char why[64] = "";
    const int len = radio_http::get_logo(url, (char*)b->img, LOGO_MAX, true, why, sizeof(why));
    if (len <= 0) {
        ESP_LOGI(TAG, "logos: no logo from %s: %s", url, why);
        return Outcome::LATE;
    }
    if (rc::image_kind(b->img, (size_t)len) == rc::Image::NONE) {
        ESP_LOGI(TAG, "logos: %s is neither JPEG nor PNG", url);
        return Outcome::LATE;
    }
    if (!player_core::cover_thumb(b->img, (size_t)len, j.out[k].px, sl::SIDE)) {
        ESP_LOGI(TAG, "logos: %s does not decode", url);
        return Outcome::LATE;
    }
    return Outcome::OK;
}

void fetch_task(void* arg) {
    Job& j = *static_cast<Job*>(arg);
    void* mem = heap_caps_calloc(1, sizeof(Buffers), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    Buffers* b = mem ? new (mem) Buffers() : nullptr;
    int found = -1;
    if (b) {
        // One request for every station of the batch: Radio Browser takes a list of uuids.
        const char* ids[sl::WANT];
        for (int k = 0; k < j.count; k++) ids[k] = j.uuid[k];
        char path[320];
        char why[64] = "";
        int n = -1;
        if (rc::byuuid_path(ids, j.count, path, sizeof(path)))
            n = radio_http::get(rc::LOOKUP_HOST, 80, path, b->body, LOOKUP_MAX, radio_http::user_agent(),
                                why, sizeof(why));
        if (n < 0) ESP_LOGW(TAG, "logos: lookup: %s", why);
        const size_t need = n > 0 ? rc::scratch_bytes(b->body, (size_t)n) : 0;
        void* scratch = need ? heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : nullptr;
        if (scratch) found = rc::parse_favicons(b->body, (size_t)n, scratch, need, b->fav, sl::WANT);
        heap_caps_free(scratch);
    }
    for (int k = 0; k < j.count; k++) {
        j.out[k].outcome = fetch_one(j, k, b, found);
        j.done.store(k + 1, std::memory_order_release);
    }
    if (b) {
        b->~Buffers();
        heap_caps_free(b);
    }
    j.running.store(false, std::memory_order_release);   // the last touch: main may free the job
    vTaskDeleteWithCaps(nullptr);
}

// ── the main task's ──

void publish(const Logos& l) { rc::logo_shelf().set(l.shelf); }

/// "<dir>/<uuid>.565".
bool path_of(const Logos& l, const char* uuid, char* out, size_t out_sz) {
    char name[48];
    if (!sl::file_name(uuid, name, sizeof(name))) return false;
    const int n = snprintf(out, out_sz, "%s/%s", l.dir, name);
    return n > 0 && (size_t)n < out_sz;
}

/// A station's logo out of its file, into l.px. False for none, or a file this firmware did not write.
bool read_logo(Logos& l, const char* uuid) {
    char path[96];
    if (!path_of(l, uuid, path, sizeof(path))) return false;
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    const size_t n = fread(l.file, 1, sizeof(l.file), f);
    const bool longer = fgetc(f) != EOF;
    fclose(f);
    return !longer && sl::decode(l.file, n, l.px);
}

void write_logo(Logos& l, const char* uuid, const uint16_t* px) {
    char path[96];
    if (!path_of(l, uuid, path, sizeof(path)) || !sl::encode(px, l.file, sizeof(l.file))) return;
    mkdir(l.dir, 0775);                     // there already, as a rule
    if (!safe_file::save(path, l.file, sl::FILE_BYTES))
        ESP_LOGW(TAG, "logos: cannot write %s; shown until the next boot, fetched again then", path);
}

/// A file of a station no longer on a preset goes, or a candidate an interrupted save left: one,
/// once the folder is closed, so nothing is removed while it is being read. False when there is
/// none left — or no folder, or one that will not go.
bool sweep_one(const Logos& l) {
    const size_t sfx = strlen(safe_file::SUFFIX);
    DIR* d = opendir(l.dir);
    if (!d) return false;                   // no folder: nothing was ever kept
    char path[96] = "";
    while (const dirent* e = readdir(d)) {
        const size_t n = strlen(e->d_name);
        char uuid[sl::UUID_LEN];
        const bool left = n > sfx && !strcmp(e->d_name + n - sfx, safe_file::SUFFIX);
        const bool stale = sl::uuid_of(e->d_name, uuid, sizeof(uuid)) && l.plan.find(uuid) < 0;
        if ((left || stale) && snprintf(path, sizeof(path), "%s/%s", l.dir, e->d_name) < (int)sizeof(path))
            break;
        path[0] = '\0';
    }
    closedir(d);
    if (!path[0] || remove(path) != 0) return false;
    ESP_LOGI(TAG, "logos: %s removed", path);
    return true;
}

/// `px` is the logo of the plan's entry `i`: on the shelf at once, and written down once the
/// panel is quiet (persist) — unless the panel has these very pixels already.
void keep_logo(Logos& l, int i, const uint16_t* px) {
    const char* uuid = l.plan.at(i).uuid;
    const sl::Thumb* have = l.shelf.find(uuid);
    if (have && !memcmp(have->px, px, sizeof(have->px))) {   // the logo it had: nothing to write
        l.plan.set(i, sl::State::HAVE);
        return;
    }
    l.plan.kept(i);
    l.shelf.put(uuid, px);
    publish(l);
    ESP_LOGI(TAG, "logos: %s kept", uuid);
}

/// One change to the files, while the panel is quiet: the next file the sweep finds, else the
/// next logo the shelf has and its file does not. One a pass, so a sound that starts meanwhile
/// holds back the rest. Power lost before a logo is written costs a fetch at the next boot.
void persist(Logos& l) {
    if (l.sweep_due) {
        l.sweep_due = sweep_one(l);
        return;
    }
    const int i = l.plan.to_save();
    if (i < 0) return;
    const char* uuid = l.plan.at(i).uuid;
    if (const sl::Thumb* t = l.shelf.find(uuid)) write_logo(l, uuid, t->px);
    l.plan.saved(i);
}

/// The presets' uuids, read again whenever the list has moved; a change of set prunes the shelf
/// at once and the files once the panel is quiet, and so does the first look.
void read_presets(Logos& l) {
    auto& list = radio_core::stations();
    const uint32_t gen = list.generation();
    if (gen == l.list_gen || (gen & 1u)) return;
    l.list_gen = gen;
    char uuid[sl::WANT][sl::UUID_LEN] = {};
    const char* at[sl::WANT];
    radio_core::Station st;
    for (int i = 0; i < sl::WANT; i++) {
        const int idx = list.preset(i);
        if (idx >= 0 && list.at(idx, st)) snprintf(uuid[i], sizeof(uuid[i]), "%s", st.uuid);
        at[i] = uuid[i];
    }
    if (l.plan.want(at, sl::WANT) || !l.looked) {
        l.looked = true;
        l.sweep_due = true;
        l.shelf.keep(l.plan);
        publish(l);
    }
}

/// The art task's latest logo: kept when its station is on a preset, whatever stood there.
void take_offer(Logos& l) {
    auto& o = offers();
    const uint32_t g = o.generation();
    if (g == l.offer_seen || (g & 1u) || !o.get(l.offered)) return;
    l.offer_seen = g;
    l.have_offer = true;
    const int i = l.plan.find(l.offered.uuid);
    if (i >= 0) keep_logo(l, i, l.offered.px);
}

/// Each station new to the presets: the art task's last logo when it is that station's, else its
/// file; else it is to be fetched.
void place_new(Logos& l) {
    bool loaded = false;
    for (int i = 0; i < l.plan.count(); i++) {
        if (l.plan.at(i).state != sl::State::NEW) continue;
        const char* uuid = l.plan.at(i).uuid;
        if (l.have_offer && !strcmp(l.offered.uuid, uuid)) {
            keep_logo(l, i, l.offered.px);
        } else if (read_logo(l, uuid)) {
            l.shelf.put(uuid, l.px);
            l.plan.set(i, sl::State::HAVE);
            loaded = true;
        } else {
            l.plan.set(i, sl::State::NEED);
        }
    }
    if (loaded) publish(l);
}

void collect(Logos& l, int64_t now) {
    Job* j = l.job;
    if (!j) return;
    const bool running = j->running.load(std::memory_order_acquire);
    const int done = j->done.load(std::memory_order_acquire);
    for (; l.taken < done; l.taken++) {
        const int k = l.taken;
        const int i = l.plan.find(j->uuid[k]);
        // Off the presets meanwhile, or its logo came from the art task first.
        if (i < 0 || l.plan.at(i).state != sl::State::BUSY) continue;
        switch (j->out[k].outcome) {
        case Outcome::OK:       keep_logo(l, i, j->out[k].px); break;
        case Outcome::LATE:     l.plan.wait(i, now, sl::RETRY_LATE_US); break;
        case Outcome::SOON:     l.plan.wait(i, now, sl::RETRY_SOON_US); break;
        case Outcome::DEFERRED: l.plan.set(i, sl::State::NEED); break;
        }
    }
    if (!running && l.taken >= j->count) {
        j->~Job();
        heap_caps_free(j);
        l.job = nullptr;
    }
}

void start(Logos& l, bool may, int64_t now) {
    if (l.job || !may || l.net_ms < SETTLE_MS || l.gap_ms < GAP_MS) return;
    int idx[sl::WANT];
    const int n = l.plan.due(now, idx, sl::WANT);
    if (!n) return;
    void* mem = heap_caps_calloc(1, sizeof(Job), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!mem) return;
    Job* j = new (mem) Job();
    for (int k = 0; k < n; k++) {
        memcpy(j->uuid[k], l.plan.at(idx[k]).uuid, sl::UUID_LEN);
        l.plan.set(idx[k], sl::State::BUSY);
    }
    j->count = n;
    j->tls_free.store(true, std::memory_order_relaxed);
    j->running.store(true, std::memory_order_release);
    l.gap_ms = 0;
    if (xTaskCreatePinnedToCoreWithCaps(fetch_task, "radio_logo", STACK, j, tskIDLE_PRIORITY, nullptr, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGW(TAG, "logos: no memory for the task; trying again in a while");
        for (int k = 0; k < n; k++) l.plan.wait(idx[k], now, sl::RETRY_SOON_US);
        j->~Job();
        heap_caps_free(j);
        return;
    }
    l.job = j;
    l.taken = 0;
    ESP_LOGI(TAG, "logos: fetching %d", n);
}

}  // namespace

namespace radio_logos {

void update(uint32_t elapsed_ms, bool net_up, bool may_fetch, bool silent) {
    Logos& l = single<Logos>();
    if (!l.dir[0]) snprintf(l.dir, sizeof(l.dir), "%s/%s", modesp::UserStore::dir(), sl::DIR);
    if (!net_up) l.net_ms = 0;
    else if (l.net_ms < SETTLE_MS) l.net_ms += elapsed_ms;
    if (l.gap_ms < GAP_MS) l.gap_ms += elapsed_ms;
    const bool may = net_up && may_fetch;
    if (l.job) l.job->tls_free.store(may, std::memory_order_release);
    const bool quiet = l.quiet.tick(elapsed_ms, silent);
    const int64_t now = esp_timer_get_time();
    read_presets(l);
    take_offer(l);
    place_new(l);
    collect(l, now);
    start(l, may, now);
    if (quiet) persist(l);
}

void offer(const station_logo::Thumb& logo) { offers().set(logo); }

}  // namespace radio_logos
