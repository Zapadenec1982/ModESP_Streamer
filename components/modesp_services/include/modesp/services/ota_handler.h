/**
 * @file ota_handler.h
 * @brief Cloud OTA handler — download firmware via HTTP, flash, reboot
 *
 * Receives OTA command from cloud via MQTT (cmd/_ota with JSON payload),
 * downloads firmware binary over HTTP, validates (magic byte, board match,
 * SHA256 checksum), writes to OTA partition, and restarts.
 *
 * Runs in a separate FreeRTOS task to avoid blocking MQTT event handler.
 * Only one OTA operation at a time (atomic guard), shared with every other writer of the
 * update partition through try_begin()/end().
 *
 * Status feedback via SharedState:
 *   _ota.status   — "idle" | "downloading" | "verifying" | "rebooting" | "error"
 *   _ota.progress — 0..100 (download percentage)
 *   _ota.error    — error description or ""
 */

#pragma once

namespace modesp {

class SharedState;

namespace ota_handler {

/// Parameters extracted from _ota JSON payload
struct OtaParams {
    char url[256];       ///< HTTP URL to firmware binary
    char version[32];    ///< Target firmware version (e.g., "1.2.3")
    char checksum[80];   ///< SHA256 checksum ("sha256:<64 hex chars>")
};

/**
 * Start OTA download+flash in a separate FreeRTOS task.
 *
 * @param params  OTA parameters (url, version, checksum)
 * @param state   SharedState pointer for status feedback
 * @return true if task was started, false if OTA already in progress
 */
bool start_ota(const OtaParams& params, SharedState* state);

/**
 * Check if OTA is currently in progress.
 */
bool is_in_progress();

/**
 * Claim the update partition for one update written by someone else — the HTTP upload.
 *
 * esp_ota_begin() hands out a second handle on a partition that already has one, and two
 * writers then erase each other's sectors, so every writer claims the same guard start_ota()
 * does. A writer that made the partition bootable keeps the claim until the restart.
 *
 * @return true if claimed, false if an update is already running
 */
bool try_begin();

/**
 * Release a claim from try_begin(): the update failed or was abandoned.
 */
void end();

} // namespace ota_handler
} // namespace modesp
