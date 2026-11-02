/**
 * @file transport_route.h
 * @brief `audio.transport`: one transport command, taken by whatever plays.
 *
 * Play, pause and next mean a different module and a different code for every source: the card
 * takes `player.cmd`, a DLNA controller's track `dlna.cmd`, a station `radio.cmd`, the phone
 * `bt.cmd`, an AirPlay sender `airplay.cmd`. Every surface that has a transport — the glass, the
 * web, the phone app — had to know that table. Now they send `audio.transport` in `player.cmd`'s
 * vocabulary, the audio module routes it with transport_route(), and `audio.transport_caps` says
 * which codes the source takes now, so a surface shows only the buttons that do something.
 *
 * Pure: the audio module reads the state into TransportInputs and posts what this returns.
 */

#pragma once

#include <cstdint>

namespace audio_core {

/// `audio.transport` codes: `player.cmd`'s vocabulary, which `bt.cmd` and `airplay.cmd` share.
enum Transport : int32_t {
    T_NONE   = 0,
    T_PLAY   = 1,
    T_PAUSE  = 2,
    T_STOP   = 3,
    T_NEXT   = 4,
    T_PREV   = 5,
    T_TOGGLE = 6,
};

/// `audio.transport_caps`: bit (1 << code) for every code the source takes now, and this bit
/// when the position can be moved (`player.seek_s`, `radio.seek_s`).
constexpr int32_t TRANSPORT_SEEK = 1 << 7;

/// `audio.source` values this cares about (modesp::gen::AUDIO_SOURCE_*; the audio module checks
/// they still match).
constexpr int32_t TS_BT = 3, TS_FILE = 4, TS_RADIO = 5, TS_AIRPLAY = 6;

struct TransportInputs {
    int32_t source       = 0;      ///< audio.source
    bool    radio_push   = false;  ///< radio.mode "push": a DLNA controller's track, not a station
    bool    sounding     = false;  ///< the station plays or is on its way to (radio.state)
    bool    dlna_playing = false;  ///< dlna.state "PLAYING"
    bool    seekable     = false;  ///< radio.seekable, for a DLNA track
    bool    bt_peer      = false;  ///< a phone is connected
    bool    ap_remote    = false;  ///< the AirPlay sender gave a remote (DACP)
};

/// Where a code goes: the module's command key and the code in its own vocabulary. `module` is
/// null when the source does not take the code now.
struct TransportRoute {
    const char* module = nullptr;
    const char* key    = nullptr;
    int32_t     code   = 0;
};

constexpr int32_t bit(int32_t code) { return int32_t(1) << code; }

/// Play, pause, stop, next, previous and the toggle.
constexpr int32_t TRANSPORT_ALL = bit(T_PLAY) | bit(T_PAUSE) | bit(T_STOP) | bit(T_NEXT) |
                                  bit(T_PREV) | bit(T_TOGGLE);

inline int32_t transport_caps(const TransportInputs& in) {
    switch (in.source) {
    case TS_FILE:
        return TRANSPORT_ALL | TRANSPORT_SEEK;
    case TS_RADIO:
        if (in.radio_push) return TRANSPORT_ALL | (in.seekable ? TRANSPORT_SEEK : 0);
        // A station does not pause: its toggle starts and stops the stream.
        return bit(T_PLAY) | bit(T_STOP) | bit(T_NEXT) | bit(T_PREV) | bit(T_TOGGLE);
    case TS_BT:
        return in.bt_peer ? TRANSPORT_ALL : 0;
    case TS_AIRPLAY:
        // A sender that gave no remote (a phone in AirPlay 2 mode) takes nothing; a press must
        // not fall through to the card either, which would take the output away from the phone.
        return in.ap_remote ? TRANSPORT_ALL : 0;
    default:
        return 0;   // the tone, AUX, USB (the computer is its own remote), off
    }
}

inline TransportRoute transport_route(const TransportInputs& in, int32_t code) {
    if (code <= T_NONE || code > T_TOGGLE || !(transport_caps(in) & bit(code))) return {};
    switch (in.source) {
    case TS_FILE:
        return {"player", "player.cmd", code};
    case TS_RADIO:
        if (in.radio_push) {
            // dlna.cmd: 1 play, 2 stop, 3 pause, 4 next, 5 previous
            static constexpr int32_t DLNA[] = {0, 1, 3, 2, 4, 5, 0};
            return {"dlna", "dlna.cmd", code == T_TOGGLE ? (in.dlna_playing ? 3 : 1) : DLNA[code]};
        } else {
            // radio.cmd: 1 play, 2 stop, 3 next, 4 previous
            static constexpr int32_t RADIO[] = {0, 1, 0, 2, 3, 4, 0};
            return {"radio", "radio.cmd", code == T_TOGGLE ? (in.sounding ? 2 : 1) : RADIO[code]};
        }
    case TS_BT:
        // bt.cmd is spelled in player.cmd's vocabulary; the bt module resolves the toggle, being
        // the side that knows what the phone is doing.
        return {"bt", "bt.cmd", code};
    case TS_AIRPLAY:
        return {"airplay", "airplay.cmd", code};
    default:
        return {};
    }
}

}  // namespace audio_core
