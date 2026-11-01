/**
 * @file system_monitor.h
 * @brief Моніторинг системних ресурсів: heap, uptime, boot reason
 *
 * Періодично публікує стан системи в SharedState.
 * Перевіряє порогові значення heap та звертається до ErrorService.
 * Фіксує причину перезавантаження при старті.
 */

#pragma once

#include "modesp/base_module.h"

namespace modesp {

class ErrorService;  // Forward declaration

class SystemMonitor : public BaseModule {
public:
    explicit SystemMonitor(ErrorService& error_service);

    bool on_init() override;
    void on_update(uint32_t dt_ms) override;
    void on_stop() override;

    /// One turn of the main loop, in microseconds, from the task that runs it.
    ///
    /// The loop is supposed to come round every 10 ms. How late it gets is the number that
    /// says whether a producer task is starving it, the fault that ends in a watchdog
    /// reset. The maximum over a report window is what matters: an average hides exactly
    /// the excursion being looked for.
    void note_loop_period_us(uint32_t us) { if (us > loop_max_us_) loop_max_us_ = us; }

    /// One turn of the main loop that did not fit its period: the delay did not sleep.
    ///
    /// The worst period alone cannot see a loop that is late on every turn — at 11.65 ms
    /// a turn, which starves IDLE0 into a watchdog reset, the maximum reads 12. The count
    /// per report window can: 0 is a healthy board, a handful is one stall, and ~400 in a
    /// 5 s window is a loop that no longer fits 100 Hz at all.
    void note_loop_overrun() { ++loop_overruns_; }

    // Alarm thresholds, bytes of free internal heap (system.heap_internal), not of the whole heap
    uint32_t heap_warning_threshold  = 20000;
    uint32_t heap_critical_threshold = 10000;

    // Інтервал оновлення стану (ms)
    uint32_t report_interval_ms = 5000;

private:
    ErrorService& error_service_;
    uint32_t elapsed_ms_ = 0;
    uint32_t min_free_heap_ = UINT32_MAX;
    uint32_t loop_max_us_ = 0;      ///< worst main-loop period this window; reset on report
    uint32_t loop_overruns_ = 0;    ///< main-loop turns that overran this window; reset on report
    bool heap_warning_sent_ = false;
    bool heap_critical_sent_ = false;
};

} // namespace modesp
