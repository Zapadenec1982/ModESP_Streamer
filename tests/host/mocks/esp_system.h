/**
 * @file esp_system.h
 * @brief Host stand-in: the shutdown-handler registration PersistService makes. A host process
 *        never restarts, so nothing is kept.
 */

#pragma once

#include "esp_err.h"

typedef void (*shutdown_handler_t)(void);

static inline esp_err_t esp_register_shutdown_handler(shutdown_handler_t handler) {
    (void)handler;
    return ESP_OK;
}
