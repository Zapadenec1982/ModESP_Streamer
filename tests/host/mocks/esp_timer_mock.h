/**
 * @file esp_timer_mock.h
 * @brief HOST BUILD: esp_timer stubs.
 */
#pragma once

#ifndef ESP_TIMER_MOCK_H
#define ESP_TIMER_MOCK_H

#include <cstdint>

/// The host clock, in microseconds. It stands at 0 unless a test moves it — a test that needs
/// time to pass (a watchdog timeout) sets it, and puts it back to 0 when it is done.
inline int64_t& host_timer_us() { static int64_t now = 0; return now; }

inline int64_t esp_timer_get_time() { return host_timer_us(); }

typedef struct esp_timer* esp_timer_handle_t;
typedef void (*esp_timer_cb_t)(void* arg);

#endif // ESP_TIMER_MOCK_H
