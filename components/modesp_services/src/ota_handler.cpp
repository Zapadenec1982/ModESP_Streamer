/**
 * @file ota_handler.cpp
 * @brief Cloud OTA: HTTP download → flash → reboot
 *
 * Окремий FreeRTOS таск (8KB стек) для завантаження firmware по HTTP,
 * валідації (magic byte, board match, SHA256), запису в OTA партицію,
 * і перезавантаження. Модулі продовжують працювати під час download.
 */

#include "modesp/services/ota_handler.h"
#include "modesp/shared_state.h"

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
// SHA-256 over the streaming download (mbedtls_sha256_* used below). IDF v6.0
// (mbedtls 4.x) removed the public mbedtls/sha256.h; the legacy API now lives in
// mbedtls/private/sha256.h. The context is used opaquely here (no private field
// access), so including it directly is safe. IDF v5.5 keeps the public header.
#if defined(MBEDTLS_MAJOR_VERSION) && MBEDTLS_MAJOR_VERSION >= 4
#include "mbedtls/private/sha256.h"
#else
#include "mbedtls/sha256.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstring>
#include <cstdio>
#include <atomic>

static const char* TAG = "OTA_HTTP";

namespace modesp {
namespace ota_handler {

// ── Atomic guard — тільки одна OTA операція одночасно ────────────

static std::atomic<bool> s_ota_in_progress{false};

// SharedState для публікації статусу (встановлюється перед запуском task)
static SharedState* s_state = nullptr;

// Статичний буфер параметрів (безпечно: atomic flag блокує повторний запуск)
static OtaParams s_task_params;

// ── Helpers для оновлення статусу через SharedState ──────────────

static void set_status(const char* status) {
    if (s_state) s_state->set("_ota.status", status);
}

static void set_progress(int pct) {
    if (s_state) s_state->set("_ota.progress", static_cast<int32_t>(pct));
}

static void set_error(const char* err) {
    if (s_state) s_state->set("_ota.error", err);
}

// ── OTA task (окремий FreeRTOS таск, 8KB стек) ──────────────────

static void ota_task(void* arg) {
    auto* params = static_cast<OtaParams*>(arg);

    ESP_LOGI(TAG, "OTA task started: version=%s, url=%s", params->version, params->url);

    set_status("downloading");
    set_progress(0);
    set_error("");

    // 1. Знайти OTA партицію
    const esp_partition_t* update_partition = esp_ota_get_next_update_partition(nullptr);
    if (!update_partition) {
        ESP_LOGE(TAG, "No OTA partition found");
        set_status("error");
        set_error("No OTA partition");
        s_ota_in_progress.store(false);
        vTaskDelete(nullptr);
        return;
    }

    ESP_LOGI(TAG, "Target partition: '%s' (offset 0x%lx, size %lu bytes)",
             update_partition->label,
             (unsigned long)update_partition->address,
             (unsigned long)update_partition->size);

    // 2. Відкрити HTTP з'єднання
    esp_http_client_config_t http_cfg = {};
    http_cfg.url = params->url;
    http_cfg.timeout_ms = 30000;
    http_cfg.buffer_size = 4096;
    // Verify the server certificate against the public CA bundle for https URLs
    // so the firmware download cannot be silently MITM'd. Plain http:// is still
    // permitted for LAN servers, but integrity then rests on the mandatory SHA256.
    if (strncmp(params->url, "https://", 8) == 0) {
        http_cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }

    esp_http_client_handle_t http_client = esp_http_client_init(&http_cfg);
    if (!http_client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        set_status("error");
        set_error("HTTP init failed");
        s_ota_in_progress.store(false);
        vTaskDelete(nullptr);
        return;
    }

    esp_err_t err = esp_http_client_open(http_client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        set_status("error");
        set_error("HTTP connect failed");
        esp_http_client_cleanup(http_client);
        s_ota_in_progress.store(false);
        vTaskDelete(nullptr);
        return;
    }

    int content_length = esp_http_client_fetch_headers(http_client);
    int status_code = esp_http_client_get_status_code(http_client);

    ESP_LOGI(TAG, "HTTP response: status=%d, content_length=%d", status_code, content_length);

    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP error: status %d", status_code);
        set_status("error");
        char err_msg[48];
        snprintf(err_msg, sizeof(err_msg), "HTTP %d", status_code);
        set_error(err_msg);
        esp_http_client_close(http_client);
        esp_http_client_cleanup(http_client);
        s_ota_in_progress.store(false);
        vTaskDelete(nullptr);
        return;
    }

    if (content_length > 0 && content_length > (int)update_partition->size) {
        ESP_LOGE(TAG, "Firmware too large: %d > %lu",
                 content_length, (unsigned long)update_partition->size);
        set_status("error");
        set_error("Firmware too large");
        esp_http_client_close(http_client);
        esp_http_client_cleanup(http_client);
        s_ota_in_progress.store(false);
        vTaskDelete(nullptr);
        return;
    }

    // 3. Почати OTA запис
    esp_ota_handle_t ota_handle;
    err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        set_status("error");
        set_error("OTA begin failed");
        esp_http_client_close(http_client);
        esp_http_client_cleanup(http_client);
        s_ota_in_progress.store(false);
        vTaskDelete(nullptr);
        return;
    }

    // 4. Потоковий download → write → SHA256
    mbedtls_sha256_context sha_ctx;
    mbedtls_sha256_init(&sha_ctx);
    mbedtls_sha256_starts(&sha_ctx, 0);  // 0 = SHA-256

    char buf[4096];
    int received_total = 0;
    bool magic_checked = false;
    bool board_checked = false;
    bool download_ok = true;

    // Accumulate the firmware header across reads so magic + board checks run
    // regardless of how the first esp_http_client_read() is chunked. The old
    // `received_total == 0 && read_len >= 0x70` guard silently skipped the board
    // check whenever the first segment was short (chunked transfer / slow link).
    static constexpr size_t HEADER_NEEDED = 0x70;  // covers esp_app_desc_t fields
    uint8_t header[HEADER_NEEDED];
    size_t header_len = 0;

    while (true) {
        int read_len = esp_http_client_read(http_client, buf, sizeof(buf));
        if (read_len < 0) {
            ESP_LOGE(TAG, "HTTP read error at %d bytes", received_total);
            set_status("error");
            set_error("Download error");
            download_ok = false;
            break;
        }
        if (read_len == 0) {
            break;  // EOF
        }

        // Накопичуємо заголовок із перших читань
        if (header_len < HEADER_NEEDED) {
            size_t need = HEADER_NEEDED - header_len;
            size_t copy = (static_cast<size_t>(read_len) < need)
                          ? static_cast<size_t>(read_len) : need;
            memcpy(header + header_len, buf, copy);
            header_len += copy;

            // Magic byte (перший байт = 0xE9 — ESP image header)
            if (!magic_checked && header_len >= 1) {
                magic_checked = true;
                if (header[0] != 0xE9) {
                    ESP_LOGE(TAG, "Invalid magic byte 0x%02X (expected 0xE9)", header[0]);
                    set_status("error");
                    set_error("Invalid firmware file");
                    download_ok = false;
                    break;
                }
            }

            // Board check (project_name в esp_app_desc_t, offset 0x20+48)
            if (!board_checked && header_len >= HEADER_NEEDED) {
                board_checked = true;
                static constexpr size_t DESC_OFFSET = 0x20;
                static constexpr size_t NAME_OFFSET = DESC_OFFSET + 48;

                uint32_t desc_magic;
                memcpy(&desc_magic, header + DESC_OFFSET, 4);

                if (desc_magic == 0xABCD5432) {
                    char incoming_name[33] = {};
                    memcpy(incoming_name, header + NAME_OFFSET, 32);
                    const char* running_name = esp_app_get_description()->project_name;

                    if (strcmp(incoming_name, running_name) != 0) {
                        ESP_LOGE(TAG, "Board mismatch: running '%s', incoming '%s'",
                                 running_name, incoming_name);
                        set_status("error");
                        char err_msg[96];
                        snprintf(err_msg, sizeof(err_msg),
                                 "Board mismatch: %.32s vs %.32s", running_name, incoming_name);
                        set_error(err_msg);
                        download_ok = false;
                        break;
                    }
                    ESP_LOGI(TAG, "Board check OK (%s)", running_name);
                } else {
                    ESP_LOGE(TAG, "Missing esp_app_desc magic — refusing unverifiable image");
                    set_status("error");
                    set_error("Invalid app descriptor");
                    download_ok = false;
                    break;
                }
            }
        }

        // Запис в OTA партицію
        err = esp_ota_write(ota_handle, buf, read_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA write failed: %s", esp_err_to_name(err));
            set_status("error");
            set_error("Flash write failed");
            download_ok = false;
            break;
        }

        // Оновити SHA256
        mbedtls_sha256_update(&sha_ctx,
                              reinterpret_cast<const unsigned char*>(buf), read_len);

        received_total += read_len;

        // Оновити прогрес (%)
        if (content_length > 0) {
            int pct = static_cast<int>(
                static_cast<int64_t>(received_total) * 100 / content_length);
            if (pct > 100) pct = 100;
            set_progress(pct);
        }
    }

    esp_http_client_close(http_client);
    esp_http_client_cleanup(http_client);

    if (!download_ok) {
        esp_ota_abort(ota_handle);
        mbedtls_sha256_free(&sha_ctx);
        s_ota_in_progress.store(false);
        vTaskDelete(nullptr);
        return;
    }

    if (received_total < 256) {
        ESP_LOGE(TAG, "Firmware too small: %d bytes", received_total);
        set_status("error");
        set_error("Firmware too small");
        esp_ota_abort(ota_handle);
        mbedtls_sha256_free(&sha_ctx);
        s_ota_in_progress.store(false);
        vTaskDelete(nullptr);
        return;
    }

    ESP_LOGI(TAG, "Downloaded %d bytes", received_total);

    // 5. Перевірити SHA256 checksum — ОБОВ'ЯЗКОВО.
    // Без checksum цілісність образу не гарантована (esp_ota_end робить лише
    // CRC32, який атакувальник з контролем байтів тривіально задовольняє).
    {
        if (params->checksum[0] == '\0') {
            mbedtls_sha256_free(&sha_ctx);
            ESP_LOGE(TAG, "No checksum provided — refusing to flash unverified image");
            set_status("error");
            set_error("Checksum required");
            esp_ota_abort(ota_handle);
            s_ota_in_progress.store(false);
            vTaskDelete(nullptr);
            return;
        }

        unsigned char sha_result[32];
        mbedtls_sha256_finish(&sha_ctx, sha_result);
        mbedtls_sha256_free(&sha_ctx);

        // Конвертуємо в hex рядок
        char sha_hex[65];
        for (int i = 0; i < 32; i++) {
            snprintf(sha_hex + i * 2, 3, "%02x", sha_result[i]);
        }

        // Порівнюємо з очікуваним (пропускаємо "sha256:" префікс)
        const char* expected = params->checksum;
        if (strncmp(expected, "sha256:", 7) == 0) {
            expected += 7;
        }

        if (strcmp(sha_hex, expected) != 0) {
            ESP_LOGE(TAG, "Checksum mismatch!");
            ESP_LOGE(TAG, "  got:      %s", sha_hex);
            ESP_LOGE(TAG, "  expected: %s", expected);
            set_status("error");
            set_error("Checksum mismatch");
            esp_ota_abort(ota_handle);
            s_ota_in_progress.store(false);
            vTaskDelete(nullptr);
            return;
        }

        ESP_LOGI(TAG, "SHA256 checksum verified OK");
    }

    // 6. Фіналізація OTA (CRC32 валідація ESP-IDF)
    set_status("verifying");

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA validation failed: %s", esp_err_to_name(err));
        set_status("error");
        set_error("OTA validation failed");
        s_ota_in_progress.store(false);
        vTaskDelete(nullptr);
        return;
    }

    // 7. Встановити boot партицію
    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Set boot partition failed: %s", esp_err_to_name(err));
        set_status("error");
        set_error("Set boot partition failed");
        s_ota_in_progress.store(false);
        vTaskDelete(nullptr);
        return;
    }

    ESP_LOGI(TAG, "===== OTA SUCCESS =====");
    ESP_LOGI(TAG, "Version: %s → partition '%s'", params->version, update_partition->label);
    ESP_LOGI(TAG, "Restarting in 2 seconds...");

    set_status("rebooting");
    set_progress(100);

    // Даємо час для публікації статусу через MQTT/WS
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();

    // Unreachable
    vTaskDelete(nullptr);
}

// ── Public API ───────────────────────────────────────────────────

bool start_ota(const OtaParams& params, SharedState* state) {
    // Atomic guard: тільки одна OTA одночасно
    if (!try_begin()) {
        ESP_LOGW(TAG, "OTA already in progress — ignoring");
        return false;
    }

    s_state = state;

    // Копіюємо параметри в статичний буфер (безпечно: atomic flag блокує повторний виклик)
    strncpy(s_task_params.url, params.url, sizeof(s_task_params.url) - 1);
    s_task_params.url[sizeof(s_task_params.url) - 1] = '\0';
    strncpy(s_task_params.version, params.version, sizeof(s_task_params.version) - 1);
    s_task_params.version[sizeof(s_task_params.version) - 1] = '\0';
    strncpy(s_task_params.checksum, params.checksum, sizeof(s_task_params.checksum) - 1);
    s_task_params.checksum[sizeof(s_task_params.checksum) - 1] = '\0';

    // Створюємо окремий таск з достатнім стеком (8KB)
    // Пріоритет 5: вищий за main loop (1), нижчий за WiFi/MQTT
    BaseType_t ret = xTaskCreate(ota_task, "ota_http", 8192,
                                  &s_task_params, 5, nullptr);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create OTA task (not enough memory?)");
        s_ota_in_progress.store(false);
        return false;
    }

    ESP_LOGI(TAG, "OTA task created (version=%s)", params.version);
    return true;
}

bool is_in_progress() {
    return s_ota_in_progress.load();
}

bool try_begin() {
    return !s_ota_in_progress.exchange(true);
}

void end() {
    s_ota_in_progress.store(false);
}

} // namespace ota_handler
} // namespace modesp
