/**
 * @file airplay2_port.h
 * @brief What the files in port/ share with each other. Not for upstream, not for the module.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_mac.h"

#ifdef __cplusplus
extern "C" {
#endif

/// The real side of port/redirect.h: every upstream call to esp_read_mac lands here.
esp_err_t airplay2_read_mac(uint8_t* mac, esp_mac_type_t type);

/// The identity airplay2_start() was given (zeros before it), and the name.
const uint8_t* airplay2_port_mac(void);
const char*    airplay2_port_name(void);
/// Wire profile; changed only after all previous RTSP sessions have stopped.
bool airplay2_port_legacy(void);

/// port/mdns_airplay.c: the counterpart of upstream's mdns_airplay_init(), which it lacks.
void mdns_airplay_remove(void);

/// The sample rates a stream may declare: in ANNOUNCE's SDP (rtpmap, ALAC fmtp) or as an
/// AirPlay 2 stream's `sr` (SETUP). Senders use 44.1 and 48 kHz. The timing engine and the
/// output's pace (port/audio_output_ring.cpp) divide by the rate, so one outside these is
/// refused before it reaches them.
#define AIRPLAY_RATE_MIN_HZ 8000
#define AIRPLAY_RATE_MAX_HZ 192000

/// Whether covers were asked for (airplay2_config_t::artwork), and the way in for one
/// (port/artwork.c): `len` 0 is "this track has none".
bool airplay2_port_artwork_wanted(void);
void airplay2_port_artwork(const uint8_t* image, size_t len, const char* type);

/// port/bplist_builder.c: the binary GET /info, with initialVolume (the room's level, see
/// airplay2_report_volume) — upstream's bplist_build_info_response() has no such entry and its
/// declaration (upstream/plist/plist.h) stays as published.
size_t bplist_build_info_response_vol(uint8_t* out, size_t capacity, const char* device_id,
                                      const char* device_name, const uint8_t* public_key,
                                      size_t public_key_len, uint64_t features,
                                      int64_t protocol_version, double initial_volume);

#ifdef __cplusplus
}
#endif
