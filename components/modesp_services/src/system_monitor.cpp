/**
 * @file system_monitor.cpp
 * @brief Реалізація SystemMonitor — heap, uptime, boot reason
 */

#include "modesp/system_monitor.h"
#include "modesp/error_service.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <ctime>

static const char* TAG = "SystemMonitor";

namespace modesp {

SystemMonitor::SystemMonitor(ErrorService& error_service)
    : BaseModule("sys_monitor", ModulePriority::LOW)
    , error_service_(error_service)
{}

bool SystemMonitor::on_init() {
    // Boot reason tracking
    esp_reset_reason_t reason = esp_reset_reason();
    state_set("system.reset_reason", static_cast<int32_t>(reason));

    const char* reason_str = "unknown";
    switch (reason) {
        case ESP_RST_POWERON:  reason_str = "power_on";   break;
        case ESP_RST_SW:       reason_str = "software";    break;
        case ESP_RST_PANIC:    reason_str = "panic";       break;
        case ESP_RST_INT_WDT:  reason_str = "int_wdt";    break;
        case ESP_RST_TASK_WDT: reason_str = "task_wdt";   break;
        case ESP_RST_WDT:      reason_str = "wdt";        break;
        case ESP_RST_BROWNOUT: reason_str = "brownout";    break;
        case ESP_RST_DEEPSLEEP:reason_str = "deep_sleep";  break;
        default: break;
    }
    state_set("system.boot_reason", reason_str);

    ESP_LOGI(TAG, "Boot reason: %s (%d)", reason_str, (int)reason);

    if (reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT || reason == ESP_RST_INT_WDT) {
        error_service_.report("system", static_cast<int32_t>(reason),
                             ErrorSeverity::WARNING, "Watchdog reset detected");
    }
    if (reason == ESP_RST_BROWNOUT) {
        error_service_.report("system", static_cast<int32_t>(reason),
                             ErrorSeverity::WARNING, "Brownout reset — check power");
    }
    if (reason == ESP_RST_PANIC) {
        error_service_.report("system", static_cast<int32_t>(reason),
                             ErrorSeverity::ERROR, "Panic reset — check logs");
    }

    uint32_t free_heap = esp_get_free_heap_size();
    min_free_heap_ = free_heap;
    state_set("system.heap_free", static_cast<int32_t>(free_heap));
    state_set("system.heap_min", static_cast<int32_t>(min_free_heap_));

    ESP_LOGI(TAG, "SystemMonitor initialized. Free heap: %lu bytes (internal %u, DMA %u, PSRAM %u)",
             free_heap,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return true;
}

void SystemMonitor::on_update(uint32_t dt_ms) {
    elapsed_ms_ += dt_ms;
    if (elapsed_ms_ < report_interval_ms) return;
    elapsed_ms_ = 0;

    uint32_t free_heap = esp_get_free_heap_size();
    if (free_heap < min_free_heap_) {
        min_free_heap_ = free_heap;
    }

    // The largest block of INTERNAL RAM, not of the whole heap. Every largest-block query
    // walks the pool it is asked about block by block under portENTER_CRITICAL_SAFE
    // (multi_heap_get_info_impl), so its cost is interrupts off for as long as the walk.
    // Asked of MALLOC_CAP_8BIT on a PSRAM target that is the 32 MB PSRAM pool: 2.6-4.3 ms
    // over ~6 700 blocks, measured on the P4 player, which is longer than a 460800-baud UART
    // FIFO lasts — it dropped frames on the satellite link. The internal pool is ~800 blocks
    // and about 0.45 ms, and it is the pool whose fragmentation is worth watching anyway:
    // the PSRAM figure was a green light that could never turn red.
    uint32_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    // Internal DMA-capable SRAM is the scarce resource on PSRAM targets (ESP-Hosted RX
    // pool, mbedTLS and I2S DMA compete for it) — report it apart from the PSRAM-inflated
    // total. On a P4 a DMA largest block of 0 is an exhaustion alarm, not a platform quirk.
    uint32_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint32_t free_dma      = heap_caps_get_free_size(MALLOC_CAP_DMA);
    uint32_t free_psram    = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    // The lowest free internal RAM since boot. The allocator keeps a low-water mark per heap
    // and lowers it inside every allocation, so this is a field read and not a walk, and it
    // holds a dip that came and went between two reports, which a minimum of 5 s samples
    // such as system.heap_min cannot. It sums per-region lows that need not have fallen at
    // the same moment, so it errs low, the side a warning should err on. Internal for the
    // alarms' reason below: the whole heap's minimum counts the free PSRAM too.
    uint32_t min_internal  = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    // track_change=false: діагностичні ключі не тригерять WS delta
    state_set("system.heap_free", static_cast<int32_t>(free_heap), false);
    state_set("system.heap_min", static_cast<int32_t>(min_free_heap_), false);
    state_set("system.heap_largest", static_cast<int32_t>(largest_block), false);
    state_set("system.heap_internal", static_cast<int32_t>(free_internal), false);
    state_set("system.heap_internal_min", static_cast<int32_t>(min_internal), false);
    state_set("system.heap_dma", static_cast<int32_t>(free_dma), false);
    state_set("system.heap_psram", static_cast<int32_t>(free_psram), false);

    // Two numbers about the task this service runs on, which is `main`.
    //
    // The high water mark is in BYTES on ESP-IDF — the kernel says so explicitly, against
    // the words the standard FreeRTOS documentation promises — so it is published as it
    // comes back. It is the only way to see the 4 KB that used to sit on this stack inside
    // a WebSocket broadcast, and to know how much room the next thing put there has.
    //
    // The loop period is the worst turn since the last report, rounded up: it reads 10 on a
    // healthy board and climbs the moment something above `main` stops giving the CPU back.
    state_set("system.main_stack_min",
              static_cast<int32_t>(uxTaskGetStackHighWaterMark(nullptr)), false);
    state_set("system.main_loop_max_ms",
              static_cast<int32_t>((loop_max_us_ + 999) / 1000), false);
    loop_max_us_ = 0;
    state_set("system.main_loop_overruns", static_cast<int32_t>(loop_overruns_), false);
    loop_overruns_ = 0;

    // The alarms watch internal RAM (system.heap_internal), the kind that runs out. With
    // CONFIG_SPIRAM_USE_MALLOC=y the whole heap also counts every free byte of PSRAM, tens of
    // MB, so a threshold on it could never trip. Without PSRAM the two are nearly one number.
    if (free_internal < heap_critical_threshold && !heap_critical_sent_) {
        heap_critical_sent_ = true;
        error_service_.report("sys_monitor", -100,
                             ErrorSeverity::CRITICAL, "Heap critically low!");
        ESP_LOGE(TAG, "CRITICAL: Free internal heap %lu < %lu", free_internal, heap_critical_threshold);
    } else if (free_internal < heap_warning_threshold && !heap_warning_sent_) {
        heap_warning_sent_ = true;
        error_service_.report("sys_monitor", -101,
                             ErrorSeverity::WARNING, "Heap low");
        ESP_LOGW(TAG, "WARNING: Free internal heap %lu < %lu", free_internal, heap_warning_threshold);
    }

    if (free_internal > heap_warning_threshold) {
        heap_warning_sent_ = false;
        heap_critical_sent_ = false;
    }

    // Публікація поточного часу
    time_t now = time(nullptr);
    if (now > 1700000000) {  // Час валідний (після 2023)
        struct tm timeinfo;
        localtime_r(&now, &timeinfo);

        char time_str[9];   // "HH:MM:SS"
        strftime(time_str, sizeof(time_str), "%H:%M:%S", &timeinfo);
        state_set("system.time", time_str, false);

        char date_str[11];  // "DD.MM.YYYY"
        strftime(date_str, sizeof(date_str), "%d.%m.%Y", &timeinfo);
        state_set("system.date", date_str, false);
    }
}

void SystemMonitor::on_stop() {
    ESP_LOGI(TAG, "SystemMonitor stopped. Min free heap: %lu bytes", min_free_heap_);
}

} // namespace modesp
