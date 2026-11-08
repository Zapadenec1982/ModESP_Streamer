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

#include <cstdint>

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

/**
 * Keeps an update's write loop from holding its core away from that core's idle task.
 *
 * Writing an image is bound by the flash: each 4 KB sector is erased with the core
 * busy-polling and the scheduler held, and when the write returns the next chunk is already
 * waiting, so the loop never blocks. Every task below the writer on its core then waits, the
 * idle task last, and the task watchdog resets the board mid-image. A one-tick sleep per
 * sector does not cure it (IDF's own erase yield is one): the tasks between the writer and
 * idle (main loop, glass, analyser) have all woken during the erase, they take the tick, and
 * idle runs only once all of them are done.
 *
 * So pace(), called after each write, sleeps one tick at a time until the idle task of this
 * core has had a turn, whenever it has not had one for half a second. The loop then gives up
 * as much CPU as the tasks above idle need to catch up, and no more.
 *
 * One at a time — the update claim (try_begin()) already makes it so.
 */
class IdlePacer {
public:
    IdlePacer();    ///< starts counting the idle tasks' turns
    ~IdlePacer();   ///< stops
    IdlePacer(const IdlePacer&) = delete;
    IdlePacer& operator=(const IdlePacer&) = delete;

    /// After each write: returns at once, or after the idle task of this core has run.
    void pace();

    static constexpr int MAX_CORES = 2;

private:
    uint32_t seen_[MAX_CORES] = {};        ///< idle turns counted at the last look
    int64_t  seen_at_us_[MAX_CORES] = {};  ///< when they last changed
    uint32_t waits_ = 0;                   ///< times pace() slept, for the log at the end
    int64_t  waited_us_ = 0;               ///< and for how long in all
};

} // namespace ota_handler
} // namespace modesp
