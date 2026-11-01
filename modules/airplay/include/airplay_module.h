/**
 * @file airplay_module.h
 * @brief AirPlay 2 as a source of its own: audio.source 6
 *        (docs/CONCEPT.md, "`audio.source` values").
 *
 * The receiver is components/airplay2, upstream code behind a narrow seam. This module is
 * the player's side of it: when the receiver runs, what the phone's events mean in
 * SharedState, and the source contract — whoever produces the sound switches audio.source
 * to itself, and a producer whose source is no longer selected stops being heard.
 */

#pragma once

#include "modesp/base_module.h"
#include "volume_follow.h"
#include <cstdint>

class AirplayModule : public modesp::BaseModule {
public:
    AirplayModule();

    bool on_init() override;
    void on_update(uint32_t dt_ms) override;

private:
    void start_receiver();
    void switch_mode(int32_t selected);
    void stop_listening();
    void publish_mode();
    void follow_source(int32_t source);
    void mirror_events(uint32_t dt_ms, bool mine);
    void apply_remote_cmds();

    int32_t  active_mode_ = 0;       ///< profile used by the current listener
    int32_t  attempted_mode_ = -1;   ///< no automatic retry loop on a failed selection
    bool     mode_in_flight_ = false;
    bool     advertise_dirty_ = true;
    bool     stopping_    = false;   ///< the worker in flight stops listening (airplay.enabled off)
    bool     stop_tried_  = false;   ///< this "off" has had its one attempt
    bool     guard_set_   = false;   ///< the NVS start guard is written and not yet cleared
    bool     was_up_      = false;   ///< Wi-Fi in station mode, as of the last update
    bool     was_enabled_ = true;
    bool     was_mine_    = false;   ///< audio.source was AirPlay, as of the last update
    bool     remote_      = false;   ///< what airplay.remote says now
    audio_core::VolumeFollow vol_;   ///< how the phone's volume moves the room's
    uint32_t session_seen_ = 0;      ///< sh.session as of the last update
    int32_t  room_seen_   = -1;      ///< audio.volume as of the last update, or as we set it
    uint32_t meta_seen_   = 0;
    uint32_t tick_ms_     = 0;
    const char* state_    = "";      ///< what airplay.state says now
};
