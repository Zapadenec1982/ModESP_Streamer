/**
 * @file log_policy.h
 * @brief What the panel prints by default, and how to make it talk without reflashing.
 *
 * The shipping default is WARN (CONFIG_LOG_DEFAULT_LEVEL_WARN in boards/p4_player/
 * sdkconfig.board), with the ceiling left at INFO (CONFIG_LOG_MAXIMUM_LEVEL_INFO) so every
 * ESP_LOGI is still COMPILED IN and can be switched back on at runtime. Dropping the ceiling
 * to WARN as well would save flash and make the switch a lie.
 *
 * ── Why there is an allowlist rather than a plain WARN ─────────────────────────────────
 *
 * Every fault diagnosed on this bench was diagnosed from the log, and not all of the lines
 * that did it are warnings. Two of the three that settled the DMA starvation below are ESP_LOGI:
 *
 *     Radio: stream heap: internal 42095, DMA 2535 (largest 2432)          (radio, INFO)
 *     Radio: stream: buffer 0%, net 147968 B/s (need 40000), audio 0.49x   (radio, INFO)
 *     [port_bt] now reading 48000 Hz, 16-bit slots, clocked from the crystal (i2s, INFO)
 *
 * A blanket WARN would have hidden all three, and the DMA starvation behind them would have
 * looked like "DLNA sometimes plays at half speed". The other two lines of that diagnosis —
 * `cannot take 48000 Hz / 32-bit slots: ESP_ERR_NO_MEM` (E) and `48000 Hz in 32-bit slots
 * was not adopted` (W) — survive WARN on their own.
 *
 * So the default silences the chatter this project does not own — esp_hosted, lwIP, LVGL,
 * the codec libraries, which is where the volume actually is — and keeps at INFO the handful
 * of tags whose INFO lines have each already paid for themselves. Six tags cost six nodes in
 * the log library's tag list and nothing per line that is not printed.
 *
 * ── Runtime ───────────────────────────────────────────────────────────────────────────
 *
 * GET/POST /api/loglevel (components/modesp_net/src/http_service.cpp). Deliberately NOT
 * persisted: a diagnostic switch that survives a reboot is a diagnostic switch someone
 * leaves on. A reboot returns to the policy below.
 */

#pragma once

#include "esp_log.h"

namespace modesp {
namespace log_policy {

/// A tag held above the global default, and the reason it earns that.
struct Kept {
    const char* tag;
    esp_log_level_t level;
    const char* why;
};

/// nullptr-terminated. Public so GET /api/loglevel can show it and say why.
extern const Kept KEPT[];

/// Install the policy: the build's default globally, then the allowlist. Call it as the
/// first thing in app_main — the boot banner is one of the lines it turns back on.
void apply();

/// Move one tag, or the global level when `tag` is "*".
///
/// A "*" set wipes every per-tag entry inside the log library (esp_log_level_set does that
/// by design), so the allowlist is re-applied afterwards: it is policy, not something a
/// caller set by hand and might mean to drop. To silence one of those tags, name it.
///
/// @return false for an unknown level name, or a level above what this build compiled in —
///         asking for DEBUG when the ceiling is INFO would otherwise report success and
///         change nothing.
bool set(const char* tag, const char* level_name);

/// "none" | "error" | "warn" | "info" | "debug" | "verbose".
const char* name_of(esp_log_level_t level);

/// The level, or ESP_LOG_NONE - 1 cast away as an error: use `parse` and check the bool.
bool parse(const char* level_name, esp_log_level_t& out);

/// Highest level this build can emit at all; anything above is not in the binary.
esp_log_level_t ceiling();

/// The level the build boots with, before any runtime change.
esp_log_level_t shipping_default();

} // namespace log_policy
} // namespace modesp
