/**
 * @file watchdog_service.h
 * @brief Software watchdog — watches whether work is actually progressing.
 *
 * It used to compare BaseModule::last_update_ms() against a timeout, and it could never
 * fire: the same main loop that calls on_update() stamps that field immediately afterwards,
 * for every RUNNING module, on every pass. So the only fault it could detect was the main
 * loop itself stopping — which stops this service too, and which the hardware task watchdog
 * already catches. Structurally inert.
 *
 * It now watches liveness counters (modesp/liveness.h) instead. Each is bumped by the task
 * that does the work, so it stops exactly when that task stops, and a module whose own task
 * is wedged while the main loop runs on is finally visible.
 *
 * On a first miss: a log line, MsgModuleTimeout on the bus — which ErrorService turns into
 * the one report of it — and the name in `system.stalled`. A restart is attempted only for an
 * entry that asked for one — see on_stall() for why nothing does yet.
 *
 * The public API is unchanged so this can go upstream as it is.
 */

#pragma once

#include "modesp/base_module.h"
#include "modesp/module_manager.h"
#include "modesp/service_messages.h"
#include "etl/unordered_map.h"

namespace modesp {

class ErrorService;  // Forward declaration

namespace liveness { struct Entry; }

class WatchdogService : public BaseModule {
public:
    WatchdogService(ErrorService& error_service, ModuleManager& manager);

    bool on_init() override;
    void on_update(uint32_t dt_ms) override;
    void on_stop() override;

    // Fallback timeouts by module priority (ms) — used for a liveness entry that does not
    // name a timeout of its own. A task with a known beat should name one instead.
    struct Timeouts {
        uint32_t critical_ms = 5000;    // 5 сек
        uint32_t high_ms     = 10000;   // 10 сек
        uint32_t normal_ms   = 30000;   // 30 сек
        uint32_t low_ms      = 60000;   // 1 хв
    };
    Timeouts timeouts;

    static constexpr uint8_t MAX_RESTARTS = 3;

    // Інтервал перевірки (ms) — не кожен update, а раз на секунду
    uint32_t check_interval_ms = 1000;

private:
    uint32_t get_timeout_for(ModulePriority priority) const;
    void on_stall(liveness::Entry& e, uint32_t timeout_ms);

    ErrorService& error_service_;
    ModuleManager& module_manager_;
    uint32_t elapsed_ms_ = 0;
    uint32_t uptime_ms_ = 0;
    /// What `system.stalled` currently says, as a pointer into the static name it was set
    /// from — so the key is only written when the answer actually changes.
    const char* published_stall_ = nullptr;

    // Лічильники перезапусків по модулях
    struct RestartInfo {
        uint8_t count = 0;
        bool reported = false;  // Чи вже повідомлено ErrorService
    };
    etl::unordered_map<StateKey, RestartInfo, MODESP_MAX_MODULES> restart_counts_;
};

} // namespace modesp
