/**
 * @file app.h
 * @brief Application lifecycle — ініціалізація та graceful shutdown
 *
 * App — тонка обгортка, що зв'язує ModuleManager + SharedState.
 * Використовується тільки з main.cpp.
 * main.cpp owns the boot phases, the check of init_all()'s result after each of them, and
 * the main loop; App has no run() of its own.
 * Модулі НЕ повинні залежати від App — вони працюють через BaseModule API.
 *
 * TODO: в майбутньому прибрати singleton, передавати залежності явно.
 */

#pragma once

#include "modesp/module_manager.h"
#include "modesp/shared_state.h"

namespace modesp {

#ifndef MODESP_MAIN_LOOP_HZ
#define MODESP_MAIN_LOOP_HZ 100
#endif

#ifndef MODESP_UPDATE_BUDGET_MS
#define MODESP_UPDATE_BUDGET_MS 8
#endif

#ifndef MODESP_WDT_TIMEOUT_MS
#define MODESP_WDT_TIMEOUT_MS 5000   // 5 секунд HW watchdog
#endif

class App {
public:
    // Singleton (тимчасово, для Phase 1)
    static App& instance();

    // Не копіюється
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    // ── Lifecycle ──
    bool init();
    void stop();  // Graceful shutdown: stops every module in reverse order

    // ── Доступ до підсистем (тільки для main.cpp!) ──
    ModuleManager& modules() { return modules_; }
    SharedState&   state()   { return state_; }

    // ── Стан ──
    uint32_t uptime_sec() const;

private:
    App() = default;

    ModuleManager modules_;
    SharedState   state_;
    uint32_t      boot_time_us_ = 0;
};

} // namespace modesp
