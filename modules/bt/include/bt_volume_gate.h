/**
 * @file bt_volume_gate.h
 * @brief When a volume the phone sets becomes the room's volume. Pure, so the host tests reach it.
 *
 * The satellite is the AVRCP absolute-volume target and forwards every level the phone sets
 * (VOLUME_STATE, source 1); the panel applies it. Two of those levels are not somebody turning
 * a knob, and taking them made the room jump — up to full scale — while the amplifier was being
 * listened to at a much lower level:
 *
 *  - the level a phone restores as it connects. It is whatever the phone last used with this
 *    device, sent unasked: a Windows host sent 0, 13 and 15 within 1.7 s of connecting and 0
 *    again at 5.4 s. For HOLD_MS after a connection none is taken;
 *  - any level while Bluetooth is not the selected source. A phone left connected must not move
 *    the radio's volume — the rule AirPlay already keeps (airplay_module.cpp, mirror_events).
 *
 * A level the connection brings is answered with the panel's own, so the phone's slider starts at
 * what the room is playing at. One set while another source plays is left alone: answering every
 * press there threw the phone's slider back each time, which reads as volume that lags and crawls.
 * The phone is told the room's level once instead, when Bluetooth is picked. Main task only.
 */
#pragma once

#include <cstdint>

namespace bt_volume {

class Gate {
public:
    static constexpr uint32_t HOLD_MS = 8000;

    /// BT_STATE arrived. True on a fresh connection: the panel should push its own level.
    bool on_state(bool connected) {
        const bool fresh = connected && !connected_;
        connected_ = connected;
        hold_ms_ = fresh ? HOLD_MS : (connected ? hold_ms_ : 0);
        return fresh;
    }
    /// The link to the satellite went down: the next BT_STATE is a connection again.
    void link_lost() { connected_ = false; hold_ms_ = 0; }
    void tick(uint32_t dt_ms) { hold_ms_ = dt_ms >= hold_ms_ ? 0 : hold_ms_ - dt_ms; }

    /// A level the phone set: take it as the room's volume.
    bool take(bool bt_selected) const { return bt_selected && settled(); }
    /// A level not taken: answer it with the room's (true), or leave the phone's slider alone.
    bool answer() const { return !settled(); }
    /// Connected, and past the levels a connection brings. A level that arrives before
    /// BT_STATE says connected (AVRCP can come up first) is part of the connection too.
    bool settled() const { return connected_ && hold_ms_ == 0; }
    /// Whether Bluetooth is the selected source now. True when it has just become it: the panel
    /// then tells the phone the room's level, so the next press starts from there.
    bool on_selected(bool selected) {
        const bool picked = selected && !selected_;
        selected_ = selected;
        return picked;
    }

private:
    bool     connected_ = false;
    bool     selected_  = false;
    uint32_t hold_ms_   = 0;
};

} // namespace bt_volume
