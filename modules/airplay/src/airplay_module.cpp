/**
 * @file airplay_module.cpp
 * @brief AirPlay 2 as a source: the receiver's lifecycle, its events in SharedState, and the
 *        source contract.
 *
 * Three rules from docs/CONCEPT.md, "SharedState contract: sources" decide everything here:
 *  - whoever produces the sound switches audio.source to itself — a phone that starts
 *    playing makes AirPlay the source;
 *  - a producer whose source is no longer selected is not heard — the receiver's ring output
 *    follows audio.source, and the session itself is left alone;
 *  - the bit in audio.sources means the source exists — airplay_offered() below.
 *
 * The receiver's events arrive on its own tasks and are only recorded there; every
 * SharedState write happens here, on the main loop.
 */

#include "airplay_module.h"

// airplay.mode_status and the stopped receiver's airplay.active_mode_label are messages of the
// web's catalog ("@<key>"): the panel and each browser
// show them in their own language. tools/i18n/gen_panel_strings.py checks every key.

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "airplay2.h"
#include "airplay_meta.h"
#include "cover.h"
#include "state_enums.h"   // audio.source values, generated from the manifests
#include "modesp/services/device_id.h"
#include "modesp/net/mdns_guard.h"
#include "modesp/services/nvs_helper.h"
#include "device_name.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

/// The audio core offers source 6 only when this is linked: it holds a weak reference to it
/// (modules/audio, ring_sources_offered), so a build without this module has no AirPlay card.
extern "C" bool airplay_offered(void) { return true; }

namespace {

const char* TAG = "AirPlay";

constexpr int32_t SRC_AIRPLAY = modesp::gen::AUDIO_SOURCE_AIRPLAY;
/// What the phone lists: the panel's one name (device_name.h), as it stood when the module
/// started — the same the DLNA renderer and Bluetooth go by. A new one reaches AirPlay at the next
/// start.
char s_name[device_name::CAP] = "";
/// The start guard, as the DLNA renderer has one: a start that never returned must not be
/// retried on the next boot, or it is a boot loop. Not the receiver's own namespace
/// ("airplay", which holds the pairing keys).
constexpr const char* NVS_NS  = "airplay_mod";
constexpr const char* NVS_TRY = "starting";
/// The per-turn (un)advertise does not queue behind a DACP query, which holds the mDNS guard
/// for up to 2 s (mdns_query_guard.cpp) from tasks that outrank main: it retries next turn.
constexpr uint32_t MDNS_TURN_WAIT_MS = 20;
/// hap_init() generates and stores the pairing keys on the first boot — a flash write, and
/// a flash write on a PSRAM stack is a crash here (docs/CONCEPT.md, "Board constraints"). So
/// the one-shot task that starts the receiver gets an internal stack, and gives it back when
/// it is done.
constexpr uint32_t START_STACK = 8 * 1024;

enum Start : int { NOT_YET = 0, STARTING = 1, UP = 2, FAILED = 3 };

using player_core::CoverSource;
constexpr auto COVER = CoverSource::AirPlay;
/// Nothing bigger can arrive: upstream's RTSP buffer stops at 256 KB (rtsp_server.c).
constexpr size_t   COVER_MAX = 256 * 1024;
/// How far apart a new title and its cover may land and still belong together. Senders send
/// the names first and the cover after, but not every one promises the order.
constexpr int64_t  COVER_PAIRING_US = 5 * 1000 * 1000;

/// One cover to show, or none (jpg null). Everything that changes the AirPlay tile goes
/// through this one queue and this one worker, so a clear and a set can never overtake
/// each other.
struct CoverJob { uint8_t* jpg; size_t len; };
QueueHandle_t s_cover_q = nullptr;

/// What the receiver's tasks tell the main loop. Flags are atomics; the strings and times are
/// copied under the spinlock, held for no longer than three short copies.
struct Shared {
    portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
    char     title[64]  = "";
    char     artist[64] = "";
    char     album[64]  = "";
    uint32_t dur_s = 0;
    uint32_t pos_s = 0;          ///< position as of pos_at_us
    int64_t  pos_at_us = 0;
    std::atomic<uint32_t> meta_gen{0};
    std::atomic<bool>     connected{false};
    std::atomic<bool>     playing{false};
    std::atomic<bool>     paused{false};
    std::atomic<bool>     play_edge{false};   ///< started playing since the main loop looked
    std::atomic<int32_t>  volume_pct{-1};     ///< the phone's last volume, -1 when none is new
    std::atomic<uint32_t> session{0};         ///< bumped per sender connection
    std::atomic<int>      start{NOT_YET};
    std::atomic<int32_t>  mode_target{0};
    std::atomic<bool>     stop_target{false}; ///< the worker stops listening instead of switching
    std::atomic<int>      mode_result{ESP_OK};
    std::atomic<bool>     mode_done{false};
    std::atomic<int64_t>  cover_at{0};        ///< when the last cover (or "none") arrived
    std::atomic<int64_t>  title_at{0};        ///< when the title last changed, 0 once settled
    /// A station's interlude and an empty cover wait before they are shown (airplay_meta.h).
    /// Under the spinlock, with the title and the cover they hold.
    airplay_meta::Smoother meta;
    char     held_title[64] = "";
    CoverJob held_cover{nullptr, 0};
};
Shared sh;
airplay2_config_t s_cfg;                      ///< outlives the start task

/// Hand the worker the latest cover, dropping one still waiting: only the newest matters.
void post_cover(CoverJob job) {
    if (!s_cover_q) { heap_caps_free(job.jpg); return; }
    CoverJob stale;
    if (xQueueReceive(s_cover_q, &stale, 0) == pdTRUE) heap_caps_free(stale.jpg);
    if (xQueueSend(s_cover_q, &job, 0) != pdTRUE) heap_caps_free(job.jpg);
}

void cover_worker(void*) {
    CoverJob job;
    for (;;) {
        if (xQueueReceive(s_cover_q, &job, portMAX_DELAY) != pdTRUE) continue;
        if (!job.jpg) {
            player_core::cover_clear(COVER);
            continue;
        }
        const bool ok = player_core::cover_set_jpeg(job.jpg, job.len, COVER);
        ESP_LOGI(TAG, "cover %u bytes: %s", (unsigned)job.len, ok ? "shown" : "not decoded");
        heap_caps_free(job.jpg);
    }
}

/// The phone's slider is -30..0 dB, and -144 is its mute. Mapped onto audio.volume by
/// position, not by decibel: the phone's half-way is the panel's half-way. Matching decibels
/// instead would put the phone's whole travel into the top half of audio.volume, which is
/// 80 dB wide, and leave the bottom half out of the phone's reach.
int32_t pct_from_db(float db) {
    if (db <= -30.0f) return 0;
    if (db >= 0.0f) return 100;
    return (int32_t)lroundf((db + 30.0f) * (100.0f / 30.0f));
}

float db_from_pct(int32_t pct) {
    if (pct <= 0) return -144.0f;
    if (pct >= 100) return 0.0f;
    return -30.0f + 30.0f * (float)pct / 100.0f;
}

/// Where the phone is in the track now: the last reported position, moved on by the time
/// since, while it plays. Call with sh.lock held.
uint32_t position_now_locked(int64_t now) {
    uint32_t pos = sh.pos_s;
    if (sh.playing.load(std::memory_order_relaxed) && sh.pos_at_us)
        pos += (uint32_t)((now - sh.pos_at_us) / 1000000);
    if (sh.dur_s && pos > sh.dur_s) pos = sh.dur_s;
    return pos;
}

void on_event(airplay2_event_t ev, const airplay2_event_data_t* d, void*) {
    const int64_t now = esp_timer_get_time();
    switch (ev) {
    case AIRPLAY2_EV_CONNECTED:
        sh.connected.store(true);
        sh.session.fetch_add(1);            // its first volume is its own (volume_follow.h)
        break;
    case AIRPLAY2_EV_PLAYING:
        sh.connected.store(true);
        sh.paused.store(false);
        if (!sh.playing.load()) {
            portENTER_CRITICAL(&sh.lock);
            sh.pos_at_us = now;                     // the position counts on from here
            portEXIT_CRITICAL(&sh.lock);
            sh.playing.store(true);
            sh.play_edge.store(true);
        }
        break;
    case AIRPLAY2_EV_PAUSED:
        if (sh.playing.load()) {
            portENTER_CRITICAL(&sh.lock);
            sh.pos_s = position_now_locked(now);    // and stops where it got to
            sh.pos_at_us = now;
            portEXIT_CRITICAL(&sh.lock);
        }
        sh.playing.store(false);
        sh.paused.store(true);
        sh.meta_gen.fetch_add(1);
        break;
    case AIRPLAY2_EV_DISCONNECTED:
        sh.play_edge.store(false);
        sh.volume_pct.store(-1);
        sh.connected.store(false);
        sh.playing.store(false);
        sh.paused.store(false);
        {
            portENTER_CRITICAL(&sh.lock);
            sh.title[0] = sh.artist[0] = sh.album[0] = '\0';
            sh.dur_s = sh.pos_s = 0;
            sh.pos_at_us = 0;
            sh.meta.reset();                        // nothing held is shown
            sh.held_title[0] = '\0';
            const CoverJob held = sh.held_cover;
            sh.held_cover = CoverJob{nullptr, 0};
            portEXIT_CRITICAL(&sh.lock);
            heap_caps_free(held.jpg);
        }
        sh.meta_gen.fetch_add(1);
        sh.title_at.store(0);
        post_cover(CoverJob{nullptr, 0});           // the session's cover goes with it
        break;
    case AIRPLAY2_EV_METADATA: {
        // Each record carries part of the picture — the names, or the progress — with the
        // rest zeroed (upstream's handle_set_parameter), so it replaces only what it carries.
        const bool names = d->title[0] || d->artist[0] || d->album[0];
        const bool times = d->duration_s || d->position_s;
        CoverJob stale{nullptr, 0};
        portENTER_CRITICAL(&sh.lock);
        bool new_title = false;
        bool shown = false;
        if (names) {
            // A station's name between a track's records waits (airplay_meta.h).
            bool drop_cover = false;
            const auto verdict = sh.meta.on_names(!d->artist[0] && !d->album[0],
                                                  sh.artist[0] != '\0',
                                                  strcmp(sh.title, d->title) == 0, now,
                                                  drop_cover);
            if (drop_cover) {
                stale = sh.held_cover;
                sh.held_cover = CoverJob{nullptr, 0};
            }
            if (verdict == airplay_meta::Smoother::Names::HOLD) {
                strlcpy(sh.held_title, d->title, sizeof(sh.held_title));
            } else {
                if (strcmp(sh.title, d->title) != 0) {  // another track: its clock starts at 0
                    sh.pos_s = 0;
                    sh.pos_at_us = now;
                    new_title = true;
                }
                strlcpy(sh.title, d->title, sizeof(sh.title));
                strlcpy(sh.artist, d->artist, sizeof(sh.artist));
                strlcpy(sh.album, d->album, sizeof(sh.album));
                shown = true;
            }
        }
        if (times) {
            if (d->duration_s) sh.dur_s = d->duration_s;
            sh.pos_s = d->position_s;
            sh.pos_at_us = now;
            shown = true;
        }
        portEXIT_CRITICAL(&sh.lock);
        heap_caps_free(stale.jpg);
        if (new_title) sh.title_at.store(now);
        if (shown) sh.meta_gen.fetch_add(1);
        break;
    }
    case AIRPLAY2_EV_VOLUME:
        sh.volume_pct.store(pct_from_db(d->volume_db));
        break;
    case AIRPLAY2_EV_ARTWORK: {
        // Copied here, on the RTSP task: the bytes live in its buffer, which the next
        // request reuses. Decoded on the worker, because a decode is milliseconds this task
        // should spend answering the phone.
        CoverJob job{nullptr, 0};
        if (d->image_len) {
            if (d->image_len > COVER_MAX) break;
            job.jpg = (uint8_t*)heap_caps_malloc(d->image_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!job.jpg) break;
            memcpy(job.jpg, d->image, d->image_len);
            job.len = d->image_len;
        }
        // An empty cover, or one that comes with a held interlude, waits (airplay_meta.h).
        CoverJob stale{nullptr, 0};
        portENTER_CRITICAL(&sh.lock);
        bool drop_held = false;
        const auto verdict = sh.meta.on_cover(job.jpg == nullptr, now, drop_held);
        if (drop_held) {
            stale = sh.held_cover;
            sh.held_cover = CoverJob{nullptr, 0};
        }
        if (verdict == airplay_meta::Smoother::Cover::HOLD) sh.held_cover = job;
        portEXIT_CRITICAL(&sh.lock);
        heap_caps_free(stale.jpg);
        if (verdict == airplay_meta::Smoother::Cover::POST) {
            sh.cover_at.store(now);
            post_cover(job);
        }
        break;
    }
    }
}

void start_task(void*) {
    const esp_err_t err = airplay2_start(&s_cfg);
    if (err != ESP_OK) ESP_LOGE(TAG, "receiver did not start: %s", esp_err_to_name(err));
    sh.start.store(err == ESP_OK ? UP : FAILED);
    vTaskDelete(nullptr);
}

} // namespace

// ── the module ────────────────────────────────────────────────────────────────────────────

namespace {
void mode_task(void*) {
    // The receiver's one lifecycle worker: a profile switch, or AirPlay turned off.
    const esp_err_t err = sh.stop_target.load() ? airplay2_stop_listening()
                                                : airplay2_set_legacy_mode(sh.mode_target.load() == 1);
    sh.mode_result.store(err);
    sh.mode_done.store(true, std::memory_order_release);
    vTaskDelete(nullptr);
}
}

AirplayModule::AirplayModule() : modesp::BaseModule("airplay") {}

bool AirplayModule::on_init() {
    bool crashed_last_time = false;
    modesp::nvs_helper::read_bool(NVS_NS, NVS_TRY, crashed_last_time);
    if (crashed_last_time) {
        // Cleared now, so the boot after this one tries again: one bad start costs one boot.
        modesp::nvs_helper::erase_key(NVS_NS, NVS_TRY);
        sh.start.store(FAILED);
        ESP_LOGE(TAG, "previous receiver start did not return; disabled for this boot");
    }
    // PersistService has restored settings before product modules initialise.
    active_mode_ = read_int("airplay.mode", 0) == 1 ? 1 : 0;
    state_set("airplay.active_mode", active_mode_);
    state_set("airplay.active_mode_label", active_mode_ == 1 ? "AirPlay 1 · Windows" : "AirPlay 2");
    state_set("airplay.switching", false);
    state_set("airplay.mode_error", "");
    state_set("airplay.mode_status", "@airplay.st_starting");
    device_name::get(s_name, sizeof s_name);
    state_set("airplay.name", s_name);
    state_set("airplay.state", "off");
    state_set("airplay.remote", false);
    state_set("airplay.title", "");
    state_set("airplay.artist", "");
    state_set("airplay.album", "");
    state_set("airplay.pos_s", (int32_t)0);
    state_set("airplay.dur_s", (int32_t)0);
    state_ = "off";

    // Covers: a queue of one and a worker on a PSRAM stack — the DLNA browser decodes its
    // covers the same way. Without either, AirPlay simply has no cover; nothing else depends
    // on it, so a failure here is logged, not fatal.
    s_cover_q = xQueueCreate(1, sizeof(CoverJob));
    if (!s_cover_q ||
        xTaskCreatePinnedToCoreWithCaps(cover_worker, "ap_cover", 6144, nullptr, tskIDLE_PRIORITY, nullptr, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "cover worker not started - no AirPlay covers");
        if (s_cover_q) vQueueDelete(s_cover_q);
        s_cover_q = nullptr;
    }
    return true;
}

void AirplayModule::start_receiver() {
    s_cfg = airplay2_config_t{};
    s_cfg.name = s_name;
    s_cfg.legacy_mode = active_mode_ == 1;
    memcpy(s_cfg.mac, modesp::device_mac(), sizeof(s_cfg.mac));
    s_cfg.volume_db = db_from_pct(read_int("audio.volume", 30));
    s_cfg.artwork = s_cover_q != nullptr;       // ask for covers only if they can be shown
    s_cfg.listener = on_event;
    s_cfg.ctx = nullptr;

    // Written here, on the main loop's internal stack, and cleared once the task answers.
    modesp::nvs_helper::write_bool(NVS_NS, NVS_TRY, true);
    guard_set_ = true;
    sh.start.store(STARTING);
    if (xTaskCreatePinnedToCore(start_task, "ap_start", START_STACK, nullptr, 3, nullptr, 0) !=
        pdPASS) {
        ESP_LOGE(TAG, "start task not created");
        sh.start.store(FAILED);
    }
}

void AirplayModule::publish_mode() {
    state_set("airplay.active_mode", active_mode_);
    state_set("airplay.active_mode_label", airplay2_started()
        ? (active_mode_ == 1 ? "AirPlay 1 · Windows" : "AirPlay 2") : "@airplay.st_stopped");
}

void AirplayModule::switch_mode(int32_t selected) {
    attempted_mode_ = selected;
    // Wi-Fi's event task may rebuild mDNS. Hold its lifecycle guard for the whole
    // removal. The receiver takes it only around a DACP query (mdns_query_guard.cpp): the
    // RTSP grace loop for up to 2 s, dacp_wk for up to 1.5 s, both above main's priority, so
    // this rare, user-made switch may wait ~3.5 s — inside the 5 s watchdog. The worker that
    // joins them never holds it.
    {
        modesp::MdnsGuard guard;
        if (!guard.locked()) {
            state_set("airplay.mode_error", "ESP_ERR_NO_MEM");
            state_set("airplay.mode_status", "@airplay.st_not_applied");
            return;
        }
        airplay2_unadvertise();
    }
    advertise_dirty_ = true;
    mode_in_flight_ = true;
    sh.mode_done.store(false);
    sh.stop_target.store(false);
    sh.mode_target.store(selected);
    state_set("airplay.switching", true);
    state_set("airplay.mode_error", "");
    state_set("airplay.mode_status", "@airplay.st_switching");
    if (xTaskCreatePinnedToCore(mode_task, "ap_mode", START_STACK, nullptr, 3, nullptr, 0) != pdPASS) {
        sh.mode_result.store(ESP_ERR_NO_MEM);
        sh.mode_done.store(true, std::memory_order_release);
    }
}

/// airplay.enabled off: no sender, old or new, until it is turned on again. Taking only the
/// mDNS record away left the receiver listening, and a sender with a stored address connected
/// and took the output. The listener comes back through switch_mode(), which starts it with
/// the selected profile.
void AirplayModule::stop_listening() {
    attempted_mode_ = -1;                 // turned on again, the selected profile is started
    {
        modesp::MdnsGuard guard;          // rare and user-made, as switch_mode()
        if (!guard.locked()) {
            state_set("airplay.mode_error", "ESP_ERR_NO_MEM");
            return;
        }
        airplay2_unadvertise();
    }
    stopping_ = true;
    mode_in_flight_ = true;
    sh.mode_done.store(false);
    sh.stop_target.store(true);
    if (xTaskCreatePinnedToCore(mode_task, "ap_mode", START_STACK, nullptr, 3, nullptr, 0) != pdPASS) {
        sh.mode_result.store(ESP_ERR_NO_MEM);
        sh.mode_done.store(true, std::memory_order_release);
    }
}

void AirplayModule::follow_source(int32_t source) {
    // The phone started playing: the producer switches the source to itself
    // (docs/CONCEPT.md, "Who switches the source"). Only on the start, not on every PLAYING —
    // a pick of another source on the panel sticks until the phone is paused and played again,
    // which is the phone's owner asking.
    const bool started = sh.play_edge.exchange(false);
    if (started) vol_.play_start();   // a sender restates its level as it starts
    // Turned off, AirPlay takes nothing — not even from a sender that starts before the
    // listener has finished stopping.
    if (started && source != SRC_AIRPLAY && read_bool("airplay.enabled", true)) {
        ESP_LOGI(TAG, "sender started playing — source -> airplay");
        state_set("audio.source", SRC_AIRPLAY);
        source = SRC_AIRPLAY;
    }
    const bool mine = source == SRC_AIRPLAY;
    if (mine == was_mine_) return;
    was_mine_ = mine;
    airplay2_set_output(mine);
    if (!mine && sh.playing.load()) {
        // Picked away from on the panel while the phone plays. An AirPlay 1 sender can be
        // asked to pause; an AirPlay 2 one cannot (upstream implements no MRP) and keeps
        // sending into a ring that no longer listens, which is silent and harmless.
        if (!airplay2_pause_sender())
            ESP_LOGI(TAG, "source changed; the sender cannot be paused from here");
    }
}

void AirplayModule::mirror_events(uint32_t dt_ms, bool mine) {
    // The room's level is announced to senders (initialVolume), and the phone's volume moves the
    // room's only while AirPlay is what is heard: the room keeps its level when a phone starts,
    // a phone left connected must not move the radio's volume, and the phone reaches the whole
    // range (volume_follow.h). In AirPlay 2 mode the panel cannot move the phone's slider (that
    // would take MRP, which upstream does not implement); a sender that honours initialVolume
    // starts its slider at the room's level and sends nothing until somebody moves it, so the
    // phone's level is the room's and the rule is the identity. What it maps is a sender that
    // restates its own level, and the room moved by another hand mid-session.
    const int32_t room = read_int("audio.volume", -1);
    const uint32_t session = sh.session.load();
    if (session != session_seen_ && room >= 0) {
        session_seen_ = session;
        vol_.session_start(room, room);   // told the room's level: the sender's slider is there
    }
    vol_.tick(dt_ms);
    if (room >= 0 && room != room_seen_) {
        if (room_seen_ >= 0) vol_.room_moved(room);   // the knob or the web: counts from here
        room_seen_ = room;
        airplay2_report_volume(db_from_pct(room));
    }
    const int32_t pct = sh.volume_pct.exchange(-1);
    if (pct >= 0 && room >= 0) {
        const int32_t to = vol_.on_level(pct, room, mine);
        if (to < 0) {
            ESP_LOGI(TAG, "volume %ld%% on the phone: the room stays at %ld%%", (long)pct, (long)room);
        } else if (to != room) {
            state_set("audio.volume", to);
            room_seen_ = to;
            airplay2_report_volume(db_from_pct(to));
            ESP_LOGI(TAG, "volume %ld%% on the phone -> %ld%%", (long)pct, (long)to);
        }
    }

    // A held title or cover that nothing has overtaken in its window was a real change after all.
    {
        const int64_t now = esp_timer_get_time();
        CoverJob cover{nullptr, 0};
        bool new_title = false;
        portENTER_CRITICAL(&sh.lock);
        const auto due = sh.meta.due(now);
        if (due.title) {
            if (strcmp(sh.title, sh.held_title) != 0) {
                sh.pos_s = 0;
                sh.pos_at_us = now;
                new_title = true;
            }
            strlcpy(sh.title, sh.held_title, sizeof(sh.title));
            sh.artist[0] = sh.album[0] = '\0';
        }
        if (due.cover) {
            cover = sh.held_cover;
            sh.held_cover = CoverJob{nullptr, 0};
        }
        portEXIT_CRITICAL(&sh.lock);
        if (due.title) {
            if (new_title) sh.title_at.store(now);
            sh.meta_gen.fetch_add(1);
        }
        if (due.cover) {
            sh.cover_at.store(now);
            post_cover(cover);
        }
    }

    const uint32_t gen = sh.meta_gen.load();
    tick_ms_ += dt_ms;
    if (gen != meta_seen_ || tick_ms_ >= 1000) {
        tick_ms_ = 0;
        char title[64], artist[64], album[64];
        uint32_t dur, pos;
        const int64_t now = esp_timer_get_time();
        portENTER_CRITICAL(&sh.lock);
        memcpy(title, sh.title, sizeof(title));
        memcpy(artist, sh.artist, sizeof(artist));
        memcpy(album, sh.album, sizeof(album));
        dur = sh.dur_s;
        pos = position_now_locked(now);
        portEXIT_CRITICAL(&sh.lock);
        if (gen != meta_seen_) {
            meta_seen_ = gen;
            state_set("airplay.title", title);
            state_set("airplay.artist", artist);
            state_set("airplay.album", album);
            state_set("airplay.dur_s", (int32_t)dur);
        }
        state_set("airplay.pos_s", (int32_t)pos);
    }

    // A new title with no cover near it: the old track's picture must not stay next to it.
    // "Near" either way, since not every sender sends the names before the cover.
    const int64_t title_at = sh.title_at.load();
    if (title_at) {
        const int64_t now = esp_timer_get_time();
        const int64_t cover_at = sh.cover_at.load();
        const bool paired = cover_at && llabs(cover_at - title_at) < COVER_PAIRING_US;
        if (paired) {
            sh.title_at.store(0);
        } else if (now - title_at > COVER_PAIRING_US) {
            sh.title_at.store(0);
            if (player_core::cover_view(COVER).px) post_cover(CoverJob{nullptr, 0});
        }
    }

    const int st = sh.start.load();
    const char* s = mode_in_flight_         ? "switching"
                  : st != UP || !airplay2_started() ? "off"
                  : sh.playing.load()     ? "playing"
                  : sh.paused.load()      ? "paused"
                  : sh.connected.load()   ? "connected"
                                          : "idle";
    if (s != state_) {
        state_ = s;
        state_set("airplay.state", s);
    }
}

void AirplayModule::on_update(uint32_t dt_ms) {
    const int32_t selected_mode = read_int("airplay.mode", 0) == 1 ? 1 : 0;
    const bool enabled = read_bool("airplay.enabled", true);
    char mode[8] = "";
    read_string("wifi.mode", mode, sizeof(mode));
    const bool up = read_bool("wifi.connected", false) && strcmp(mode, "sta") == 0;

    const int st = sh.start.load();
    if (mode_in_flight_ && sh.mode_done.exchange(false, std::memory_order_acquire)) {
        mode_in_flight_ = false;
        active_mode_ = airplay2_legacy_mode() ? 1 : 0;
        const int32_t room = read_int("audio.volume", 30);
        vol_.session_start(room, room);   // a new profile: a new announcement
        state_set("airplay.switching", false);
        publish_mode();
        const esp_err_t err = sh.mode_result.load();
        state_set("airplay.mode_error", err == ESP_OK ? "" : esp_err_to_name(err));
        state_set("airplay.mode_status", stopping_ || !airplay2_started() ? "@airplay.st_stopped"
            : err == ESP_OK ? "@airplay.st_applied" : "@airplay.st_not_applied");
        stopping_ = false;
        advertise_dirty_ = true;
    }
    if (st == NOT_YET && enabled && up) {
        active_mode_ = selected_mode;
        start_receiver();
    }
    if ((st == UP || st == FAILED) && guard_set_) {
        modesp::nvs_helper::erase_key(NVS_NS, NVS_TRY);   // it returned, either way
        guard_set_ = false;
        publish_mode();
        state_set("airplay.mode_status", st == UP ? "@airplay.st_applied" : "@airplay.st_stopped");
    }
    if (!enabled && was_enabled_) stop_tried_ = false;    // a fresh "off" gets one attempt
    if (st == UP && !mode_in_flight_) {
        if (!enabled) {
            if (airplay2_started() && !stop_tried_) {
                stop_tried_ = true;               // a failed stop does not retry every pass
                stop_listening();
            }
        } else if (selected_mode != active_mode_ || !airplay2_started()) {
            if (selected_mode != attempted_mode_) {
                if (up) switch_mode(selected_mode);
                else state_set("airplay.mode_status", "@airplay.st_wifi");
            }
        } else {
            // Selecting the still-running profile cancels an earlier failed request.
            attempted_mode_ = -1;
            state_set("airplay.mode_error", "");
            state_set("airplay.mode_status", "@airplay.st_applied");
        }
    }
    // Turning AirPlay off takes the record away inside stop_listening(); this only puts it back.
    if (st == UP && !mode_in_flight_ && airplay2_started() && enabled && up &&
        (advertise_dirty_ || !was_up_ || !was_enabled_)) {
        modesp::MdnsGuard guard(MDNS_TURN_WAIT_MS);
        if (guard.locked()) airplay2_advertise();
        advertise_dirty_ = !guard.locked();
    }
    was_up_ = up;
    was_enabled_ = enabled;

    const int32_t source = read_int("audio.source", 0);
    follow_source(source);
    mirror_events(dt_ms, was_mine_);
    apply_remote_cmds();
}

/// The panel's transport row, sent on to the sender's remote.
///
/// airplay.cmd speaks player.cmd's vocabulary, as bt.cmd does, so the screen hands every
/// source the same numbers: 1 play, 2 pause, 3 stop, 4 next, 5 previous, 6 toggle. DACP has
/// one toggle for the first three, so play goes only to a sender that is not playing and
/// pause or stop only to one that is. A sender that gave no remote — a phone in AirPlay 2
/// mode — cannot be commanded from here at all, and the press ends here: passed on, it would
/// reach the card player, which would take the output from the phone.
void AirplayModule::apply_remote_cmds() {
    const bool ready = airplay2_remote_ready();
    if (ready != remote_) {
        remote_ = ready;
        state_set("airplay.remote", ready);
    }
    modesp::BaseModule::Command c;
    while (next_command(c)) {
        const bool playing = sh.playing.load();
        airplay2_remote_t cmd;
        switch (c.value) {
        case 1:  if (playing) continue;  cmd = AIRPLAY2_REMOTE_PLAYPAUSE; break;
        case 2:
        case 3:  if (!playing) continue; cmd = AIRPLAY2_REMOTE_PLAYPAUSE; break;
        case 4:  cmd = AIRPLAY2_REMOTE_NEXT; break;
        case 5:  cmd = AIRPLAY2_REMOTE_PREV; break;
        case 6:  cmd = AIRPLAY2_REMOTE_PLAYPAUSE; break;
        default: continue;
        }
        if (!airplay2_remote(cmd)) {
            ESP_LOGI(TAG, "transport %ld: the sender gave no remote", (long)c.value);
        }
    }
}
