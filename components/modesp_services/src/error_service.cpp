/**
 * @file error_service.cpp
 * @brief Реалізація ErrorService — збір помилок + Safe Mode
 */

#include "modesp/error_service.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/semphr.h"

static const char* TAG = "ErrorService";

namespace modesp {

namespace {

/// Serialises report()'s write of error.count — see there. Made by the first report, so on a
/// running scheduler; static storage, like the rest of this service.
SemaphoreHandle_t count_writer() {
    static StaticSemaphore_t storage;
    static SemaphoreHandle_t mutex = xSemaphoreCreateMutexStatic(&storage);
    return mutex;
}

} // namespace

ErrorService::ErrorService()
    : BaseModule("error", ModulePriority::CRITICAL)
{}

bool ErrorService::on_init() {
    state_set("error.count", static_cast<int32_t>(0));
    state_set("error.safe_mode", false);
    ESP_LOGI(TAG, "ErrorService initialized");
    return true;
}

void ErrorService::on_update(uint32_t dt_ms) {
    uptime_ms_ += dt_ms;
    drain();
}

void ErrorService::on_message(const etl::imessage& msg) {
    // ErrorService може реагувати на системні повідомлення
    // Наприклад, MsgModuleTimeout від WatchdogService
    if (msg.get_message_id() == msg_id::MODULE_TIMEOUT) {
        const auto& timeout_msg = static_cast<const MsgModuleTimeout&>(msg);
        report(timeout_msg.module_name.c_str(), -1,
               ErrorSeverity::ERROR, "Module timeout");
    }
}

void ErrorService::on_stop() {
    ESP_LOGI(TAG, "ErrorService stopped. Total errors: %lu", (unsigned long)total_errors_);
}

void ErrorService::report(const char* source, int32_t code,
                          ErrorSeverity severity, const char* description) {
    // Логуємо в ESP-IDF лог — outside the lock: the console lock blocks.
    switch (severity) {
        case ErrorSeverity::INFO:
            ESP_LOGI(TAG, "[%s] code=%ld: %s", source, (long)code, description);
            break;
        case ErrorSeverity::WARNING:
            ESP_LOGW(TAG, "[%s] code=%ld: %s", source, (long)code, description);
            break;
        case ErrorSeverity::ERROR:
            ESP_LOGE(TAG, "[%s] code=%ld: %s", source, (long)code, description);
            break;
        case ErrorSeverity::CRITICAL:
            ESP_LOGE(TAG, "CRITICAL [%s] code=%ld: %s", source, (long)code, description);
            break;
        case ErrorSeverity::FATAL:
            ESP_LOGE(TAG, "*** FATAL [%s] code=%ld: %s ***", source, (long)code, description);
            break;
    }

    ErrorRecord record;
    record.timestamp_ms = uptime_ms_;
    record.source = source;
    record.code = code;
    record.severity = severity;
    record.description = description;

    // Record + counter + flags under the lock; nothing that can block in here. The bus is
    // deliberately not touched: drain() does that from the main loop.
    bool first_fatal = false;
    bool queued = true;
    portENTER_CRITICAL(&lock_);
    total_errors_++;
    history_.push(record);   // circular: overwrites the oldest, never asserts
    if (severity >= ErrorSeverity::WARNING) {
        if (pending_.full()) {
            dropped_++;
            queued = false;
        } else {
            pending_.push(record);
        }
    }
    if (severity == ErrorSeverity::FATAL && !safe_mode_) {
        safe_mode_ = true;
        safe_mode_pending_ = true;
        safe_mode_reason_ = description;
        first_fatal = true;
    }
    portEXIT_CRITICAL(&lock_);

    // error.count: one writer at a time, writing the count as it stands now rather than the
    // one this call left the lock with. Two reporters could leave the lock in one order and
    // reach the store in the other, and the smaller count, written last, stayed (195 of 200
    // in the host test). Not under lock_: SharedState takes a mutex.
    SemaphoreHandle_t writer = count_writer();
    xSemaphoreTake(writer, portMAX_DELAY);
    portENTER_CRITICAL(&lock_);
    const uint32_t total = total_errors_;
    portEXIT_CRITICAL(&lock_);
    state_set("error.count", static_cast<int32_t>(total));
    xSemaphoreGive(writer);

    if (!queued) {
        ESP_LOGW(TAG, "bus queue full (%d) — [%s] recorded but not published",
                 MODESP_ERROR_QUEUE_DEPTH, source);
    }

    if (first_fatal) {
        state_set("error.safe_mode", true);
        state_set("error.safe_mode_reason", description);

        ESP_LOGE(TAG, "╔═══════════════════════════════════╗");
        ESP_LOGE(TAG, "║     *** SAFE MODE ACTIVATED ***   ║");
        ESP_LOGE(TAG, "║  Reason: %-24s ║", description);
        ESP_LOGE(TAG, "╚═══════════════════════════════════╝");
    }
}

void ErrorService::drain() {
    // Take one item out under the lock, deliver it outside: the bus runs every module's
    // on_message() right here, and one of them may report() in turn.
    for (;;) {
        ErrorRecord rec;
        portENTER_CRITICAL(&lock_);
        const bool have = !pending_.empty();
        if (have) {
            rec = pending_.front();
            pending_.pop();
        }
        portEXIT_CRITICAL(&lock_);
        if (!have) break;

        MsgSystemError msg;
        msg.source = rec.source;
        msg.code = rec.code;
        msg.severity = rec.severity;
        msg.description = rec.description;
        publish(msg);
    }

    bool fire = false;
    etl::string<48> reason;
    portENTER_CRITICAL(&lock_);
    if (safe_mode_pending_) {
        safe_mode_pending_ = false;
        reason = safe_mode_reason_;
        fire = true;
    }
    portEXIT_CRITICAL(&lock_);

    if (fire) {
        // Актуатори повинні вимкнутись
        MsgSafeMode msg;
        msg.reason = reason;
        publish(msg);
    }
}

} // namespace modesp
