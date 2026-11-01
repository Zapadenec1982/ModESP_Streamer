/**
 * @file watchdog_service.cpp
 * @brief Software watchdog — watches liveness counters, not the heartbeat it stamps itself.
 */

#include "modesp/watchdog_service.h"
#include "modesp/error_service.h"
#include "modesp/liveness.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char* TAG = "WatchdogSvc";

namespace modesp {

WatchdogService::WatchdogService(ErrorService& error_service, ModuleManager& manager)
    : BaseModule("watchdog", ModulePriority::CRITICAL)
    , error_service_(error_service)
    , module_manager_(manager)
{}

bool WatchdogService::on_init() {
    state_set("system.stalled", "");
    ESP_LOGI(TAG, "WatchdogService initialized (default timeouts: C=%lu H=%lu N=%lu L=%lu ms)",
             timeouts.critical_ms, timeouts.high_ms,
             timeouts.normal_ms, timeouts.low_ms);
    return true;
}

void WatchdogService::on_update(uint32_t dt_ms) {
    uptime_ms_ += dt_ms;
    elapsed_ms_ += dt_ms;
    if (elapsed_ms_ < check_interval_ms) return;
    elapsed_ms_ = 0;

    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    const char*    stalled_name = nullptr;

    for (size_t i = 0; i < liveness::MAX_ENTRIES; i++) {
        liveness::Entry* e = liveness::entry(i);
        if (!e) continue;

        // An entry may name its own timeout; otherwise it inherits the one its owning
        // module's priority implies, which is what the old code did for every module.
        const uint32_t fallback =
            get_timeout_for(e->owner ? e->owner->priority() : ModulePriority::NORMAL);

        switch (liveness::check(*e, now_ms, fallback)) {
        case liveness::Event::STALLED:
            on_stall(*e, e->timeout_ms ? e->timeout_ms : fallback);
            break;
        case liveness::Event::RECOVERED:
            ESP_LOGW(TAG, "'%s' is making progress again", e->name);
            break;
        case liveness::Event::NONE:
            break;
        }

        if (e->stalled && !stalled_name) stalled_name = e->name;
    }

    // One key, naming the first task that is stuck, so a dashboard and a soak log can both
    // see it without reading the console. Published on a change only: the names are static
    // strings, so comparing the pointers is the whole test.
    if (stalled_name != published_stall_) {
        published_stall_ = stalled_name;
        state_set("system.stalled", stalled_name ? stalled_name : "");
    }
}

void WatchdogService::on_stall(liveness::Entry& e, uint32_t timeout_ms) {
    const char* owner = e.owner ? e.owner->name() : e.name;

    ESP_LOGE(TAG, "'%s' (%s) has not made progress for %lu ms — its task is stuck",
             e.name, owner, (unsigned long)timeout_ms);

    // ErrorService makes the stall's one report out of this message (its on_message). A
    // report() of our own beside it counted every stall twice — error.count, the history and
    // MsgSystemError all saw two.
    MsgModuleTimeout msg;
    msg.module_name  = owner;
    msg.last_seen_ms = e.last_change_ms;
    publish(msg);

    // Restarting is opt-in per entry, and nothing opts in today. restart_module() calls
    // on_stop() and then on_init(), and every module here creates its tasks and buffers in
    // on_init() while on_stop() only asks them to wind down — so calling it on a module
    // whose task is wedged makes a second task rather than a working one. The mechanism
    // stays for the module that can honestly support it; the policy until then is to say
    // so loudly and let the hardware watchdog own the case where saying so is not enough.
    if (!e.restart || !e.owner) return;

    StateKey key(owner);
    auto it = restart_counts_.find(key);
    if (it == restart_counts_.end()) {
        RestartInfo info;
        restart_counts_.insert({key, info});
        it = restart_counts_.find(key);
        if (it == restart_counts_.end()) return;   // ETL map full: nothing more we can do
    }
    auto& info = it->second;

    if (info.count < MAX_RESTARTS) {
        info.count++;
        ESP_LOGW(TAG, "Module '%s' restart %d/%d", owner, info.count, MAX_RESTARTS);
        if (module_manager_.restart_module(*e.owner)) {
            ESP_LOGI(TAG, "Module '%s' restarted OK", owner);
        } else {
            ESP_LOGE(TAG, "Module '%s' restart failed", owner);
        }
    } else if (!info.reported) {
        info.reported = true;
        ESP_LOGE(TAG, "Module '%s' exceeded max restarts (%d)", owner, MAX_RESTARTS);
        error_service_.report("watchdog", -200, ErrorSeverity::CRITICAL,
                              "Module exceeded max restarts");
    }
}

void WatchdogService::on_stop() {
    ESP_LOGI(TAG, "WatchdogService stopped");
}

uint32_t WatchdogService::get_timeout_for(ModulePriority priority) const {
    switch (priority) {
        case ModulePriority::CRITICAL: return timeouts.critical_ms;
        case ModulePriority::HIGH:     return timeouts.high_ms;
        case ModulePriority::NORMAL:   return timeouts.normal_ms;
        case ModulePriority::LOW:      return timeouts.low_ms;
        default:                       return timeouts.normal_ms;
    }
}

} // namespace modesp
