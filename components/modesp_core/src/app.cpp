/**
 * @file app.cpp
 * @brief Application lifecycle — init та graceful shutdown (the main loop lives in main.cpp)
 */

#include "modesp/app.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"

static const char* TAG = "App";

namespace modesp {

App& App::instance() {
    static App app;
    return app;
}

bool App::init() {
    ESP_LOGI(TAG, "Initializing ModESP v%s (%s)", FIRMWARE_VERSION, BUILD_DATE);
    ESP_LOGI(TAG, "Free heap: %lu bytes", esp_get_free_heap_size());

    boot_time_us_ = (uint32_t)esp_timer_get_time();
    return true;
}

void App::stop() {
    ESP_LOGI(TAG, "Shutting down...");
    modules_.stop_all();
    ESP_LOGI(TAG, "Shutdown complete");
}

uint32_t App::uptime_sec() const {
    return (uint32_t)((esp_timer_get_time() - boot_time_us_) / 1000000ULL);
}

} // namespace modesp
