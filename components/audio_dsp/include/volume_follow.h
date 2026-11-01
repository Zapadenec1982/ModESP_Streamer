/**
 * @file volume_follow.h
 * @brief How a sender's volume slider moves the room's level: one rule for AirPlay's phone
 *        (modules/airplay) and USB's computer (modules/usb_audio). Pure, so the host tests
 *        reach it.
 *
 * The customer asked that the room keep its level when a phone starts, and then that the phone
 * still reach the whole range with its slider agreeing with the panel; a laptop is answered the
 * same way. The sender's level maps onto the room's through a reference point: piecewise linear
 * from (0, 0) through (sender_ref, room_ref) to (100, 100). So nothing jumps when the reference
 * is set, and both ends of the sender's travel are the room's ends: the top of the slider is the
 * top of the panel and the bottom is silence. A slider already at its top when the reference is
 * set cannot go higher, and neither can the room through it.
 *  - a session starts with the reference at the room's level and the sender's slider where it
 *    stands (an AirPlay sender was told the room's level, so the two are one);
 *  - every level in the HOLD_MS after a session or playback starts only sets the reference — a
 *    restated level is not a press — as does any level while the sender is not the source (none
 *    is kept for later);
 *  - the room moved by another hand (the knob, the web) becomes the reference with the sender's
 *    last level.
 * Main task only.
 */
#pragma once

#include <cstdint>

namespace audio_core {

class VolumeFollow {
public:
    static constexpr uint32_t HOLD_MS = 3000;

    /// A session started: the room is at `room`, the sender's slider at `sender` (0..100).
    void session_start(int32_t room, int32_t sender) {
        sender_ref_ = sender_last_ = sender;
        room_ref_ = room;
        hold_ms_ = HOLD_MS;
    }
    /// The sender started playing: it may restate its level, which is not a press either.
    void play_start() { hold_ms_ = HOLD_MS; }
    void tick(uint32_t dt_ms) { hold_ms_ = dt_ms >= hold_ms_ ? 0 : hold_ms_ - dt_ms; }

    /// The sender's slider is at `sender` (0..100). `room` is the room's level now, `mine`
    /// whether the sender is the source. Returns the room's new level, or -1 to leave it.
    int32_t on_level(int32_t sender, int32_t room, bool mine) {
        sender_last_ = sender;
        if (hold_ms_ > 0 || !mine) {
            sender_ref_ = sender;
            room_ref_   = room;
            return -1;
        }
        return map(sender);
    }

    /// The room moved by another hand: the slider's next step counts from here.
    void room_moved(int32_t room) {
        room_ref_   = room;
        sender_ref_ = sender_last_;
    }

private:
    int32_t map(int32_t p) const {
        if (p <= sender_ref_) {
            if (sender_ref_ <= 0) return room_ref_;
            return (room_ref_ * p + sender_ref_ / 2) / sender_ref_;
        }
        if (sender_ref_ >= 100) return room_ref_;
        const int32_t span = 100 - sender_ref_;
        return room_ref_ + ((100 - room_ref_) * (p - sender_ref_) + span / 2) / span;
    }

    uint32_t hold_ms_     = 0;
    int32_t  sender_ref_  = 0;
    int32_t  room_ref_    = 0;
    int32_t  sender_last_ = 0;
};

} // namespace audio_core
