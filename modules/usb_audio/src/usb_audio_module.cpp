/**
 * @file usb_audio_module.cpp
 * @brief USB audio as audio.source 7 (usb_audio_module.h).
 */

#include "usb_audio_module.h"

#include "device_name.h"
#include "modesp/services/device_id.h"
#include "state_enums.h"   // audio.source values, generated from the manifests
#include "usb_uac.h"
#include "usb_volume.h"

#include "esp_log.h"

#include <cstdio>
#include <cstring>

static const char* TAG = "USB";

namespace {

constexpr int32_t SRC_OFF     = modesp::gen::AUDIO_SOURCE_OFF;
constexpr int32_t SRC_TONE    = modesp::gen::AUDIO_SOURCE_TONE;
constexpr int32_t SRC_BT      = modesp::gen::AUDIO_SOURCE_BT;
constexpr int32_t SRC_FILE    = modesp::gen::AUDIO_SOURCE_FILE;
constexpr int32_t SRC_RADIO   = modesp::gen::AUDIO_SOURCE_RADIO;
constexpr int32_t SRC_AIRPLAY = modesp::gen::AUDIO_SOURCE_AIRPLAY;
constexpr int32_t SRC_USB     = modesp::gen::AUDIO_SOURCE_USB;

}  // namespace

/// The audio module offers source 7 while this is true (its weak reference, audio_module.cpp).
/// Defined here, beside the module class, so the link pulls it in whenever the module is built.
extern "C" bool usb_audio_offered(void) { return usb_uac::started(); }

UsbAudioModule::UsbAudioModule()
    : BaseModule("usb_audio", modesp::ModulePriority::LOW)
{}

bool UsbAudioModule::on_init() {
    state_set("usb_audio.state", usb_uac::state_name(usb_uac::State::off));
    state_set("usb_audio.rate", (int32_t)0);
    state_set("usb_audio.bits", (int32_t)0);
    state_set("usb_audio.diag", "");
    state_set("usb_audio.losses", "");

    // The computer lists the panel under its one name (device_name.h), as phones do.
    char name[device_name::CAP];
    device_name::get(name, sizeof(name));
    state_set("usb_audio.name", name);           // what the faces tell to pick on the computer
    const uint8_t* mac = modesp::device_mac();
    char serial[13];
    snprintf(serial, sizeof(serial), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    usb_uac::Config cfg;
    cfg.product = name;
    cfg.serial  = serial;
    // Core 1. The controller interrupts every microframe and must re-arm the endpoint before
    // the next one, 125 us later, or that packet is lost. On core 0, next to LVGL and the PPA
    // compositor, the interrupt was held off 250-600 us several times in five minutes — ten
    // losses, ten audible breaks at 96 kHz; on core 1 the same run lost nothing, its longest
    // gap 131 us. usb_audio.core = 0 puts it back, for comparison.
    cfg.core     = read_int("usb_audio.core", 1) == 0 ? 0 : 1;
    if (!usb_uac::start(cfg)) {
        // Not fatal: the player plays everything else without it.
        ESP_LOGW(TAG, "USB audio did not start");
    }
    return true;
}

/// Whether the selected source is playing — the one thing that keeps the computer's sound from
/// taking the output by itself. A pause is not playing; a station connecting or buffering is.
bool UsbAudioModule::panel_playing(int32_t source) const {
    char s[24];
    switch (source) {
        case SRC_TONE:    return true;   // someone is testing the speakers
        case SRC_BT:
            // A phone may stream with no play status at all (the key then reads "idle"), so
            // past the two things it can say for itself the stream is the answer.
            read_string("bt.play_state", s, sizeof(s));
            if (!strcmp(s, "playing")) return true;
            if (!strcmp(s, "paused")) return false;
            read_string("bt.state", s, sizeof(s));
            return !strcmp(s, "streaming");
        case SRC_FILE:    read_string("player.state", s, sizeof(s));  return !strcmp(s, "playing");
        case SRC_AIRPLAY: read_string("airplay.state", s, sizeof(s)); return !strcmp(s, "playing");
        case SRC_RADIO:
            // A pushed track that is paused keeps the engine's state at "playing": the pause is
            // the ring's, and only the renderer's own state says so.
            read_string("radio.mode", s, sizeof(s));
            if (!strcmp(s, "push")) {
                char d[24];
                read_string("dlna.state", d, sizeof(d));
                if (!strcmp(d, "PAUSED_PLAYBACK")) return false;
            }
            read_string("radio.state", s, sizeof(s));
            return !strcmp(s, "playing") || !strcmp(s, "buffering") || !strcmp(s, "connecting") ||
                   !strcmp(s, "reconnecting");
        default:          return false;
    }
}

void UsbAudioModule::follow_volume(uint32_t dt_ms, bool mine, bool streaming) {
    // Read directly: the stats are refreshed once a second, and a drag of the computer's
    // slider then reached the room as one jump a second.
    const int16_t host_volume = usb_uac::host_volume();
    const int32_t room = read_int("audio.volume", 30);
    const int32_t host = usb_volume::position(host_volume);

    if (streaming && !was_streaming_) {
        vol_.session_start(room, host);   // the room keeps its level; the slider is not a press
        host_seen_ = host_volume;
        room_seen_ = room;
    }
    vol_.tick(dt_ms);

    if (room_seen_ >= 0 && room != room_seen_) vol_.room_moved(room);   // the knob, the web
    room_seen_ = room;

    if (host_volume != host_seen_) {
        host_seen_ = host_volume;
        const int32_t to = vol_.on_level(host, room, mine && streaming);
        if (to >= 0 && to != room) {
            state_set("audio.volume", to);
            room_seen_ = to;
        }
    }
}

void UsbAudioModule::on_update(uint32_t dt_ms) {
    if (!usb_uac::started()) return;

    const int32_t source = read_int("audio.source", SRC_OFF);
    const bool mine = source == SRC_USB;
    usb_uac::set_output(mine);

    // The computer started sending sound: take the output, unless the panel is playing something.
    const bool signal = usb_uac::signal();
    if (signal && !was_signal_ && !mine && !panel_playing(source)) {
        ESP_LOGI(TAG, "the computer is playing and the panel is not: source -> USB");
        state_set("audio.source", SRC_USB);
    }
    was_signal_ = signal;

    const bool streaming = usb_uac::stats().state == usb_uac::State::playing;
    follow_volume(dt_ms, mine, streaming);
    was_streaming_ = streaming;

    since_ms_ += dt_ms;
    if (since_ms_ < 1000) return;
    since_ms_ = 0;
    publish();
}

void UsbAudioModule::publish() {
    const usb_uac::Stats s = usb_uac::stats();
    state_set("usb_audio.state", usb_uac::state_name(s.state));
    state_set("usb_audio.rate", (int32_t)s.rate);
    state_set("usb_audio.bits", (int32_t)s.bits);

    char diag[256];
    snprintf(diag, sizeof(diag),
             "%s%s · rx %lu/s wr %lu/s · fill %lu..%lu ms (%lu) fb %+ld ppm · drop %lu over %lu · "
             "pkts %lu/s lost %lu in %lu, gap %lu us · peak %.1f/%.1f dB · tone %.1f Hz · "
             "glitches %lu · vol %.1f dB%s · core %d",
             usb_uac::phase_name(s.phase), s.output ? "" : " (not source)",
             (unsigned long)s.rx_frames, (unsigned long)s.written,
             (unsigned long)s.fill_min_ms, (unsigned long)s.fill_max_ms, (unsigned long)s.target_ms,
             (long)s.fb_ppm, (unsigned long)s.dropped, (unsigned long)s.overflows,
             (unsigned long)s.pkts, (unsigned long)s.lost, (unsigned long)s.loss_events,
             (unsigned long)s.gap_max_us, (double)s.peak_db[0], (double)s.peak_db[1],
             (double)s.tone_hz, (unsigned long)s.glitches, s.host_volume / 256.0,
             s.host_mute ? " muted" : "", s.core);
    state_set("usb_audio.diag", diag);

    if (s.loss_events != losses_seen_) {
        losses_seen_ = s.loss_events;
        char log[256];
        usb_uac::loss_log(log, sizeof(log), 3);
        state_set("usb_audio.losses", log);
    }
}
