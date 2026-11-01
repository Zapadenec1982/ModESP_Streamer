/**
 * @file usb_audio_module.h
 * @brief USB audio as audio.source 7: the panel as a computer's sound card (components/usb_uac).
 *
 * The player's side of the device, the way modules/airplay is AirPlay's: whether the producer
 * writes the ring (audio.source is 7), when the source switches to USB by itself — the computer
 * starts sending sound while the panel plays nothing — and how the computer's volume slider moves
 * the room (volume_follow.h). It publishes usb_audio.state, .rate, .bits for the panel's faces
 * and a diagnostic line for the bench.
 *
 * Not "usb": that name is ESP-IDF's own component, the one with the USB PHY driver.
 */

#pragma once

#include "modesp/base_module.h"
#include "volume_follow.h"
#include <cstdint>

class UsbAudioModule : public modesp::BaseModule {
public:
    UsbAudioModule();

    bool on_init() override;
    void on_update(uint32_t dt_ms) override;

private:
    bool panel_playing(int32_t source) const;
    void follow_volume(uint32_t dt_ms, bool mine, bool streaming);
    void publish();

    audio_core::VolumeFollow vol_;   ///< how the computer's slider moves the room's level
    uint32_t since_ms_    = 0;
    uint32_t losses_seen_ = 0;   ///< loss events already in usb_audio.losses
    bool     was_signal_  = false;
    bool     was_streaming_ = false;
    int16_t  host_seen_   = 0x7fff;   ///< the host's level as last applied, 1/256 dB
    int32_t  room_seen_   = -1;       ///< audio.volume as of the last update, or as we set it
};
