#pragma once
#include <stdbool.h>
#include <stdint.h>

/* One boot-selected profile shared by mDNS, /info and the RTSP transport. */
static inline uint16_t airplay_mode_rtsp_port(bool legacy) {
    return legacy ? 5000 : 7000;
}
static inline const char *airplay_mode_version(bool legacy) {
    return legacy ? "105.1" : "377.40.00";
}
static inline const char *airplay_mode_model(bool legacy) {
    return legacy ? "Shairport" : "AudioAccessory5,1";
}

/* Classic metadata - DMAP text, progress, and the cover as the body of an image SET_PARAMETER -
 * as an AirPlay 2 sender is asked for it: by these feature bits, not by the RAOP record's
 * md, which an AirPlay 1 sender reads instead. 17 text, 16 progress, 15 artwork, set the
 * way shairport-sync sets them (shairport.c, airplay_features). Upstream advertises none of
 * the three. Measured with md=0,1,2: an iPhone (AirPlay/620.8.2) on the AirPlay 2 profile
 * sent the text and the progress, and over three track changes never a cover. */
#define AIRPLAY_FEATURE_META_ARTWORK  (UINT64_C(1) << 15)
#define AIRPLAY_FEATURE_META_PROGRESS (UINT64_C(1) << 16)
#define AIRPLAY_FEATURE_META_TEXT     (UINT64_C(1) << 17)

/* `artwork`: whether covers are asked for (airplay2_config_t::artwork), as md says too.
 * The classic profile's features stay upstream's: its senders read md. */
static inline uint64_t airplay_mode_features(bool legacy, bool artwork) {
    if (legacy) return UINT64_C(0x5C4A00);
    uint64_t features = UINT64_C(0x1C340405C4A00) | AIRPLAY_FEATURE_META_TEXT |
                        AIRPLAY_FEATURE_META_PROGRESS;
    if (artwork) features |= AIRPLAY_FEATURE_META_ARTWORK;
    return features;
}
