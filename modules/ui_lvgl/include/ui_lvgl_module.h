/**
 * @file ui_lvgl_module.h
 * @brief LVGL UI module — main screen: clock, Wi-Fi, source, volume, meters, source keys.
 *
 * Threading contract:
 *   - on_update() runs in the 100 Hz main loop and NEVER blocks: it reads the encoder /
 *     button drivers, the clock and the audio/Wi-Fi state into a POD snapshot under a
 *     spinlock, mirrors inputs to SharedState and applies requests the LVGL task left
 *     (source key pressed) — SharedState is written from the main loop only.
 *   - All LVGL calls happen in the esp_lvgl_port task: a 100 ms lv_timer applies the
 *     snapshot to the widgets; touch/button events write request fields of the snapshot.
 *   - on_init() builds the screen once under lvgl_port_lock().
 *
 * Backends come from bindings.json via DriverRegistry (category "lcd"): the panel
 * (ILcdPanel) and, optionally, the touch controller (ITouchPort). No panel binding →
 * the module still publishes encoder/button state and drives audio.volume / audio.mute.
 * The audio core is optional too: without `audio.*` keys the screen shows "—".
 */

#pragma once

#include "modesp/base_module.h"
#include "modesp/hal/lcd_port.h"
#include "modesp/hal/driver_interfaces.h"
#include "freertos/FreeRTOS.h"
#include <atomic>
#include <cstdint>

namespace modesp { class DriverManager; struct BindingTable; class HAL; }

struct UiScreen;   // LVGL objects — defined in the .cpp so LVGL stays private to the module

class UiLvglModule : public modesp::BaseModule {
public:
    UiLvglModule();

    void on_bind(modesp::DriverManager& drivers, const modesp::BindingTable& bindings,
                 modesp::HAL& hal) override;
    bool on_init() override;
    void on_update(uint32_t dt_ms) override;
    void on_stop() override;

    /// Values exchanged between the main loop and the LVGL task (POD, spinlock-guarded).
    struct Snapshot {
        // main loop → LVGL
        char     clock[12] = "--:--:--";
        char     date[12]  = "--.--.----";
        int32_t  encoder_pos = 0;
        bool     encoder_sw  = false;
        int32_t  volume      = -1;      ///< audio.volume mirror, −1 = no audio core
        bool     mute        = false;
        bool     leveler     = false;   ///< audio.leveler: «Вирівнювання гучності» (the Player's moon)
        int32_t  source      = -1;      ///< audio.source (0 off, 1 tone, 2 aux, 3 bt), −1 = none
        int32_t  sources     = 0;       ///< audio.sources bitmask — unavailable keys are greyed out
        char     audio_state[16] = "";  ///< audio.state
        float    level_l = -120.0f;     ///< dBFS
        float    level_r = -120.0f;
        bool     eq_enabled = false;
        int32_t  bass_db    = 0;        ///< audio.bass_db, −6…+6 (the «Звук» sheet)
        int32_t  treble_db  = 0;        ///< audio.treble_db, −6…+6
        int32_t  loudness   = 0;        ///< audio.loudness, 0…100 (0 = off)
        // the speakers (Колонки, screen_speakers.cpp): what lib/speakers.js reads in the web
        int32_t  spk_type   = 0;        ///< audio.spk_type: 0 not set, 1 ported, 2 closed
        int32_t  port_hz    = 0;        ///< audio.port_hz, 0 = none
        int32_t  port_src   = 0;        ///< audio.port_src: 0 not set, 1 by hand, 3 measured
        int32_t  balance_db10 = 0;      ///< audio.balance_db10, −60…+60: tenths of a dB, − is left
        int32_t  underruns  = 0;
        int32_t  cpu_pct    = 0;
        int32_t  in_rate    = 0;        ///< measured input sample rate, 0 = none
        int32_t  tone_hz    = 0;
        char     play_title[96] = "";   ///< player.title mirror; as wide as the tag field
                                    ///  and as a file name, which is the fallback
        char     play_artist[96] = "";  ///< player.artist mirror; empty when the file has no tag
        char     play_album[96]  = "";  ///< player.album mirror
        char     play_fmt[32]   = "";   ///< player.format mirror
        char     play_state[12] = "";   ///< player.state mirror: idle | playing | paused
        int32_t  play_pos_s = 0;
        int32_t  play_dur_s = 0;
        int32_t  play_index = -1;       ///< player.index — where the queue cursor is
        int32_t  play_count = 0;
        int32_t  repeat     = 0;
        bool     shuffle    = false;
        /// A phone is attached to the satellite. The transport row is drawn from the play_*
        /// fields like any other source, but with nothing on the other end the buttons have
        /// nobody to command, so the screen greys them out rather than swallowing presses.
        bool     bt_connected = false;
        /// A sender has a session open with the AirPlay receiver. Its play_* fields come
        /// from airplay.* the way Bluetooth's come from bt.*.
        bool     ap_connected = false;
        /// The sender gave the receiver a DACP remote (airplay.remote): only then do the
        /// buttons have someone to command.
        bool     ap_remote = false;
        /// A computer has the panel as its sound card (usb_audio.state ready or playing). With
        /// USB the source, play_state says whether it sends sound and play_fmt what it sends.
        bool     usb_connected = false;
        // radio mirrors
        bool     network_push = false;
        bool     network_seekable = false;
        char     radio_name[64]  = "";
        char     radio_now[128]  = "";   ///< the ICY StreamTitle, which is what is playing
        char     radio_fmt[32]   = "";
        char     radio_state[16] = "";   ///< idle | connecting | buffering | playing | reconnecting
        int32_t  radio_station   = 0;
        int32_t  radio_buffer    = 0;    ///< radio.buffer — the compressed buffer's fill, %
        bool     wifi_ok = false;
        char     wifi_ssid[33] = "";    ///< a whole SSID: the Wi-Fi page marks the list row by it
        char     wifi_ip[16]   = "";
        int32_t  rssi = 0;
        // ── service readout ──
        //
        // Diagnostics the service screen draws. They are mirrored by mirror_diag() at 2 Hz,
        // NOT by mirror_state() at 20 Hz: SystemMonitor recomputes them every 5 s
        // (system_monitor.h:39) and `system.uptime` once a second, so reading them ten times
        // per published value would be ten SharedState mutex round trips per value for a
        // number that cannot have moved. The fields still ride in this struct, which is
        // copied whole under the spinlock at 25 Hz — 60 bytes of POD, against the ~700 the
        // snapshot already carries.
        int32_t  loop_max_ms   = 0;     ///< system.main_loop_max_ms
        int32_t  stack_min     = 0;     ///< system.main_stack_min, BYTES on ESP-IDF
        int32_t  heap_free     = 0;     ///< system.heap_free — whole heap, PSRAM included
        int32_t  heap_min      = 0;     ///< system.heap_min — its minimum since boot
        int32_t  heap_internal = 0;     ///< system.heap_internal — the scarce one
        int32_t  heap_dma      = 0;     ///< system.heap_dma
        int32_t  heap_largest  = 0;     ///< system.heap_largest — largest INTERNAL block
        int32_t  heap_psram    = 0;     ///< system.heap_psram
        int32_t  uptime_s      = 0;     ///< system.uptime
        bool     safe_mode     = false; ///< system.safe_mode
        char     boot_reason[12] = "";  ///< system.boot_reason — longest token is deep_sleep
        char     stalled[16]     = "";  ///< system.stalled — a module name, or empty
        /// Satellite link health. bt_connected above says whether a PHONE is attached;
        /// these say whether the LINK to the satellite itself is alive and clean.
        bool     bt_link     = false;   ///< bt.link
        /// A satellite has identified itself at least once since boot (`bt.sat` carries its
        /// build string, written on HELLO). Without it, a board with no satellite fitted
        /// would show a link fault for ever, and a warning that is always on is a warning
        /// nobody reads.
        bool     bt_seen     = false;
        int32_t  bt_rtt_ms   = 0;       ///< bt.rtt_ms
        int32_t  bt_uart_ovf = 0;       ///< bt.uart_ovf
        int32_t  bt_rx_drops = 0;       ///< bt.rx_drops
        int32_t  bt_seq_gaps = 0;       ///< bt.seq_gaps
        // ── the System page (2 Hz, with the diagnostics) ──
        char     bt_peer[32]  = "";     ///< bt.peer — the phone's name
        char     bt_codec[16] = "";     ///< bt.codec
        bool     ap_enabled   = false;  ///< airplay.enabled — announced on the network
        int32_t  ap_mode      = 0;      ///< airplay.mode — 0 AirPlay 2, 1 AirPlay 1 / Windows
        bool     ap_switching = false;  ///< receiver is applying the selected mode
        bool     ap_mode_error = false; ///< a mode switch failed; details are on the web page
        char     ap_name[32]  = "";     ///< airplay.name
        bool     dlna_enabled = false;  ///< dlna.enabled — the renderer announced by SSDP
        bool     dlna_built   = false;  ///< the key exists: this build has the renderer
        char     dlna_name[40] = "";    ///< dlna.name
        int32_t  standby_min  = 5;      ///< ui.standby_min — 0 is never
        int32_t  out_caps     = 1;      ///< audio.out_caps — the output schemes that play (out_caps.h)
        // ── the Wi-Fi page (2 Hz) ──
        bool     wifi_ap      = false;  ///< wifi.mode is "ap": the fallback access point is up
        bool     wifi_saved   = false;  ///< a network is stored (WiFiService::has_credentials)
        char     wifi_try[12] = "";     ///< wifi.try: "" | connecting | ok | fail
        char     wifi_try_ssid[33] = "";///< wifi.try_ssid
        int32_t  wifi_reason  = 0;      ///< wifi.reason — the last disconnect's reason code
        // LVGL → main loop
        int16_t  touch_x = 0;
        int16_t  touch_y = 0;
        bool     touch_down = false;
        uint32_t touch_seq = 0;         ///< bumps on every touch event → cheap change detection
        int32_t  theme = 0;             ///< ui.theme mirror — which palette is active
        int32_t  backlight = 80;        ///< ui.backlight mirror
        int32_t  standby_bl = 25;       ///< ui.standby_bl mirror — the standby face while music plays
        /// Music is playing, as the standby face should see it: set the moment something plays,
        /// cleared only after a few seconds of silence, so a track change or a radio rebuffer
        /// does not move the face back and forth. The backlight follows the same flag.
        bool     sb_music = false;
        float    lim_gr_db = 0.0f;      ///< audio.lim_gr mirror
        // Encoder events. The position is monotonic and the UI keeps its own last value,
        // so no cross-task clearing is needed; click/hold use a sequence for the same
        // reason. Input SEMANTICS belong to the UI — the main loop only reports what the
        // hardware did, or the quick menu and the EQ band focus would have nowhere to live.
        uint8_t  enc_event = 0;         ///< 1 = short click, 2 = long press
        uint32_t enc_event_seq = 0;
        bool     standby = false;       ///< screen asleep (auto or from the quick menu)
    };

    /// What a screen can ask the main loop to do. SharedState is written from the main loop
    /// only, so every touch that changes something posts one of these instead of writing.
    enum class Req : uint8_t {
        SOURCE,       ///< value = audio.source
        VOLUME,       ///< value = 0..100
        MUTE,         ///< value = 1 set, 0 clear, -1 toggle
        EQ_ENABLED,   ///< value = 0/1
        THEME,        ///< value = UiThemeId
        EQ_APPLY,     ///< value = the EQ screen's revision; persist /data/eq.json
        BACKLIGHT,    ///< value = 0..100
        STANDBY,      ///< value = 1 sleep, 0 wake
        PLAY_QUEUE,   ///< play entry N of the card's index
        SEEK,         ///< value = the second of the track to jump to
        DLNA_BROWSE,
        RADIO_CMD,    ///< value = a radio.cmd code: 1 play, 2 stop, 3 next, 4 previous
        RADIO_TUNE,   ///< value = the station to tune to
        RADIO_STORE,  ///< value = the preset slot to put the playing station in
        TRANSPORT,    ///< value = a player.cmd code: 2 pause, 3 stop, 4 next, 5 prev, 6 toggle
        REPEAT,       ///< value = 0 off / 1 one / 2 all, −1 = step to the next mode
        SHUFFLE,      ///< value = 0/1, −1 = toggle
        WAKE,         ///< any input: restart the idle timer, leave standby
        STANDBY_MIN,  ///< value = minutes of idle before standby, 0 = never
        AIRPLAY_ON,   ///< value = 0/1: announce the AirPlay receiver on the network
        AIRPLAY_MODE, ///< value = 0 AirPlay 2 / 1 AirPlay 1 (Windows), applied automatically
        DLNA_ON,      ///< value = 0/1: announce the DLNA renderer on the network
        WIFI_FORGET,  ///< forget the stored network: the access point comes up
        STANDBY_BL,   ///< value = 5..100: the standby face's backlight while music plays
        LANG,         ///< value = ui.lang: 0 Українська, 1 English, 2 Deutsch, 3 Polski
        BASS,         ///< value = audio.bass_db, −6…+6 (the «Звук» sheet, S3)
        TREBLE,       ///< value = audio.treble_db, −6…+6
        LOUDNESS,     ///< value = audio.loudness, 0…100 (0 = off)
        BALANCE,      ///< value = audio.balance_db10, −60…+60 in steps of 5 (Колонки)
        LEVELER,      ///< value = audio.leveler 0/1, −1 = toggle (the Player's moon)
    };


    // LVGL-task side hooks (called from the timer / event callbacks in the .cpp)
    void lvgl_refresh();
    void lvgl_touch_event(int16_t x, int16_t y, bool down);
    /// Post a request from the LVGL task. Lock-free for the caller, drained by on_update().
    /// Dropping on overflow is correct here: these are user gestures at human rates, and a
    /// full ring means the main loop is wedged, in which case a stale volume is the least
    /// of the problems.
    void lvgl_post(Req kind, int32_t value);

    /// Hand a network and its password to the main loop, which tries it on its next tick. The
    /// strings do not fit a request's int, and the main task's stack is the scarce one, so
    /// they wait in two member buffers the main loop reads in place; false while the previous
    /// pair has not been taken yet.
    bool lvgl_wifi_connect(const char* ssid, const char* password);

private:
    bool start_lvgl();
    void build_screen();
    void poll_inputs(uint32_t dt_ms);
    void mirror_state();
    void mirror_diag();
    void publish_touch();
    void apply_requests();
    void want_theme(int32_t id);
    void want_lang(int32_t index);
    void rebuild_look();
    void set_standby(bool on);
    void persist_eq();
    void apply_backlight(uint32_t dt_ms);

    modesp::lcd::ILcdPanel*  panel_ = nullptr;
    modesp::lcd::ITouchPort* touch_ = nullptr;
    modesp::ISensorDriver*   encoder_ = nullptr;
    modesp::ISensorDriver*   encoder_sw_ = nullptr;

    UiScreen*  ui_ = nullptr;          ///< non-null once the screen is built
    bool       lvgl_running_ = false;

    Snapshot     snap_{};
    portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;

    // main-loop side change detection
    int32_t  last_encoder_pos_ = 0;
    bool     last_encoder_sw_  = false;
    uint32_t last_touch_seq_   = 0;
    bool     last_touch_down_  = false;
    int32_t  last_backlight_   = -1;
    // encoder button: a long press opens the quick menu, a short one is a click
    uint32_t sw_held_ms_   = 0;
    bool     sw_hold_fired_ = false;
    // auto-standby
    uint32_t idle_ms_      = 0;
    bool     standby_      = false;
    bool     shell_sb_dirty_ = false;   ///< the shell's standby has not caught up with standby_
    bool     sb_music_     = false;     ///< Snapshot::sb_music, owned here
    uint32_t sb_quiet_ms_  = 0;         ///< how long nothing has been playing
    /// While the standby level is being set from the System page, the panel shows it for a
    /// moment: a level picked blind would only be judged hours later, in the dark.
    uint32_t sb_preview_ms_ = 0;


    // The palette and the language the UI should be in (ui.theme, ui.lang). A change from the
    // glass or from off it lands here at once; rebuild_look() brings the screen to it when
    // nothing is in the way.
    int32_t  want_theme_ = 0;
    int32_t  want_lang_  = 0;

    // request ring: LVGL task writes at head_, main loop drains at tail_, both under mux_
    struct ReqItem { Req kind; int32_t value; };
    static constexpr uint8_t REQ_CAP = 16;
    ReqItem  req_[REQ_CAP]{};
    uint8_t  req_head_ = 0;
    uint8_t  req_tail_ = 0;
    // lvgl_wifi_connect(): written by the LVGL task while wifi_req_ready_ is false, read and
    // wiped by the main loop while it is true — one owner at a time, no copy on either stack.
    char     wifi_req_ssid_[33] = {};
    char     wifi_req_pass_[65] = {};
    std::atomic<bool> wifi_req_ready_{false};
    // An edit on the EQ page goes into the shared document (audio_dsp/eq_store.h) on the next
    // tick; the audio module hears it and decides when it is written (persist_eq()).
    uint32_t eq_pending_rev_ = 0;   ///< latest revision the EQ screen reports
    uint32_t eq_live_rev_    = 0;   ///< revision already put into the store
    uint32_t clock_ms_         = 1000;   ///< force a clock update on the first tick
    uint32_t mirror_ms_        = 200;    ///< force a state mirror on the first tick
    uint32_t diag_ms_          = 1000;   ///< force a diagnostics mirror on the first tick
    /// Last value published to `ui.screen`. The shell returns a pointer into its own static
    /// table, so comparing the POINTER is the whole test — no strcmp per tick.
    const char* published_screen_ = nullptr;
    int32_t     cover_id_ = -1;        ///< ui.cover as last published (−1: not yet)
};
