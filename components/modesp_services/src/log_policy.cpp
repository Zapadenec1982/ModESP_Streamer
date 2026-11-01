/**
 * @file log_policy.cpp
 * @brief The shipping log policy and its runtime switch. See log_policy.h for the evidence.
 */

#include "modesp/log_policy.h"

#include "sdkconfig.h"

#include <cstring>

static const char* TAG = "LogPolicy";

namespace modesp {
namespace log_policy {

// Each line is a tag whose INFO output has already earned its place in a diagnosis. Anything
// added here should come with the same kind of note: which fault it settled.
const Kept KEPT[] = {
    {"main",      ESP_LOG_INFO,
     "boot banner, running partition, heap by capability (log_heap) - the first thing anyone "
     "asks a panel that misbehaves"},
    {"ConfigSvc", ESP_LOG_INFO,
     "which board.json loaded and how many bindings; an over-size board.json costs a flashed "
     "panel and the size is only visible here"},
    {"UserStore", ESP_LOG_INFO,
     "the one-time migration summary - the answer to 'where did my EQ go'"},
    {"I2sStd",    ESP_LOG_INFO,
     "'[port_bt] now reading N Hz, M-bit slots' - the repeated park/widen on every format "
     "announcement was visible only as this pair repeating"},
    {"Audio",     ESP_LOG_INFO,
     "retunes, adopted formats and the EQ actually loaded; the audio core's own account of "
     "what rate it is running at"},
    {"Radio",     ESP_LOG_INFO,
     "'stream heap: internal N, DMA M' and 'stream: buffer P%, net ...' - the two lines that "
     "showed 2535 bytes of DMA heap and half-speed playback"},
    {"Bt",        ESP_LOG_INFO,
     "the satellite's OWN log, relayed over the link as 'Bt: sat: ...'. It is kept for a "
     "reason none of the others have: COM13 is the satellite's console and the bench rule "
     "forbids opening it, so these relayed lines are the only window into a second processor "
     "running firmware we also wrote. They are what proved the satellite healthy and "
     "feeding real samples ('peak 17668/32768, wire 17668') while the panel read digital "
     "silence - without them that fault would have been attributed to the wrong board"},
    {"sdmmc_card", ESP_LOG_INFO,
     "'mounted /sd: SD, 29818 MB, 4-bit @ 20000 kHz' - the one line that says the card came "
     "up. It matters more here than the sentence suggests: this slot shares the SDMMC host "
     "with esp_hosted's link to the C6, which is why storage_module keeps a deliberate no-op "
     "in host.init (esp_hosted CHANGELOG 2.12.11). A boot where the card is silent and a boot "
     "where it failed must not look the same"},
    {nullptr, ESP_LOG_NONE, nullptr},
};

esp_log_level_t ceiling() {
    return static_cast<esp_log_level_t>(CONFIG_LOG_MAXIMUM_LEVEL);
}

esp_log_level_t shipping_default() {
    return static_cast<esp_log_level_t>(CONFIG_LOG_DEFAULT_LEVEL);
}

const char* name_of(esp_log_level_t level) {
    switch (level) {
    case ESP_LOG_NONE:    return "none";
    case ESP_LOG_ERROR:   return "error";
    case ESP_LOG_WARN:    return "warn";
    case ESP_LOG_INFO:    return "info";
    case ESP_LOG_DEBUG:   return "debug";
    case ESP_LOG_VERBOSE: return "verbose";
    default:              return "?";
    }
}

bool parse(const char* level_name, esp_log_level_t& out) {
    if (!level_name) return false;
    struct { const char* name; esp_log_level_t level; } table[] = {
        {"none", ESP_LOG_NONE}, {"error", ESP_LOG_ERROR}, {"warn", ESP_LOG_WARN},
        {"warning", ESP_LOG_WARN}, {"info", ESP_LOG_INFO}, {"debug", ESP_LOG_DEBUG},
        {"verbose", ESP_LOG_VERBOSE},
    };
    for (auto& e : table) {
        if (strcmp(level_name, e.name) == 0) { out = e.level; return true; }
    }
    return false;
}

static void apply_kept() {
    for (const Kept* k = KEPT; k->tag; ++k) {
        // Never above the ceiling: the strings are not in the binary above it, so raising a
        // tag past it would change a number and nothing else.
        esp_log_level_set(k->tag, k->level > ceiling() ? ceiling() : k->level);
    }
}

void apply() {
    esp_log_level_set("*", shipping_default());
    apply_kept();
    int kept = 0;
    for (const Kept* k = KEPT; k->tag; ++k) kept++;
    // At WARN so it prints under its own policy. One line per boot, and it is the line that
    // stops someone concluding a quiet panel is a dead one.
    ESP_LOGW(TAG, "log: default %s, ceiling %s, %d tag(s) kept higher; "
                  "POST /api/loglevel {\"tag\":\"*\",\"level\":\"info\"} to open it up",
             name_of(shipping_default()), name_of(ceiling()), kept);
}

bool set(const char* tag, const char* level_name) {
    esp_log_level_t level;
    if (!tag || !*tag || !parse(level_name, level)) return false;
    if (level > ceiling()) {
        // Reporting success here would be the worst outcome: someone would go looking for
        // DEBUG output that this binary cannot produce and conclude the code never runs.
        ESP_LOGW(TAG, "%s: %s is above this build's ceiling (%s) - not applied",
                 tag, level_name, name_of(ceiling()));
        return false;
    }
    esp_log_level_set(tag, level);
    if (strcmp(tag, "*") == 0) apply_kept();   // "*" wipes the per-tag entries; see the header
    ESP_LOGW(TAG, "log level %s -> %s", tag, name_of(level));
    return true;
}

} // namespace log_policy
} // namespace modesp
