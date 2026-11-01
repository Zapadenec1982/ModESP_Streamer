/**
 * @file bt_stall_kick.h
 * @brief One PLAY for a phone that plays into a stream nobody restarts. Pure, so the host tests reach it.
 *
 * The satellite suspends the A2DP stream of a phone that keeps streaming after the host's PAUSE
 * (its bt_av.c, pause_check) and starts it again on the host's next PLAY. Until then the phone
 * holds the stream as suspended by the remote: it goes on "playing", its tracks move on, and no
 * audio arrives. The panel sends that PLAY on the way back to Bluetooth only for a pause it
 * remembers placing (auto_paused_), and that memory is RAM, lost on a restart and dropped with a
 * lost link, while the satellite keeps the suspend. Seen after two restarts of the panel (a
 * backup and a flash): Bluetooth selected, a Pixel 6 Pro saying it played with its tracks
 * moving on, the stream suspended, and silence until the satellite was restarted by hand. The
 * pause that started it was not captured; this is the path in this code that ends there.
 *
 * So: Bluetooth selected, the phone saying it plays, no stream for HOLD_MS → one PLAY. The phone
 * already plays, so the PLAY changes nothing there; on the satellite it starts the stream it
 * suspended. A pause somebody asked for (a button here or on the web) is not undone: it holds
 * until a PLAY goes out or the phone leaves, across a lost link too, since the satellite keeps
 * its suspend across one. Only a restart of the panel forgets it. Main task only.
 */
#pragma once

#include <cstdint>

namespace bt_stall {

class Kick {
public:
    /// Longer than a connecting phone takes to start its stream: a Pixel 6 Pro took 2.0 s
    /// from "a2dp connected" to "stream started".
    static constexpr uint32_t HOLD_MS = 5000;

    /// PAUSE or STOP went out because somebody asked for it.
    void on_user_pause() { user_pause_ = true; }
    /// PLAY went out, asked for or automatic.
    void on_play() { user_pause_ = false; }
    /// The phone left: a suspend belongs to the connection it was placed on.
    void on_phone_gone() {
        user_pause_ = false;
        stalled_ms_ = 0;
        sent_ = false;
    }

    /// Every pass while linked. True once per stall, when the PLAY should go out.
    bool tick(uint32_t dt_ms, bool selected, bool playing, bool streaming) {
        if (!selected || !playing || streaming || user_pause_) {
            stalled_ms_ = 0;
            sent_ = false;
            return false;
        }
        if (sent_) return false;
        stalled_ms_ = dt_ms >= HOLD_MS - stalled_ms_ ? HOLD_MS : stalled_ms_ + dt_ms;
        if (stalled_ms_ < HOLD_MS) return false;
        sent_ = true;
        return true;
    }

private:
    uint32_t stalled_ms_ = 0;
    bool     user_pause_ = false;
    bool     sent_       = false;
};

} // namespace bt_stall
