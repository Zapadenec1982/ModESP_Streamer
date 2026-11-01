/**
 * @file main.cpp
 * @brief ModESP Streamer — boot sequence and main loop (ModESP v4 control plane)
 *
 * Staged boot sequence:
 *   1. NVS init (required for WiFi credentials)
 *   2. Register system services -> init_all (phase 1)
 *   3. ConfigService reads board.json + bindings.json
 *   4. Register WiFi service
 *   5. HAL initializes GPIO from BoardConfig
 *   6. DriverManager creates drivers from bindings
 *   7. Register product modules (each binds its drivers in on_bind) -> init_all (phase 2)
 *   8. Inject dependencies, register HTTP + WS -> init_all (phase 3)
 *   9. Connect WS handler to HTTP server
 *  10. Main loop: drivers update -> modules update -> WDT reset
 *
 * Three-phase init works because module_manager.init_all()
 * skips modules with state != CREATED.
 */

#include "modesp/app.h"
#include "modesp/base_module.h"

// Phase 2: System services
#include "modesp/error_service.h"
#include "modesp/watchdog_service.h"
#include "modesp/system_monitor.h"
#include "modesp/logger_service.h"
#include "modesp/log_policy.h"

// Phase 4: Configuration + HAL + Persist
#include "modesp/services/config_service.h"
#include "modesp/services/persist_service.h"
#include "modesp/hal/hal.h"
#include "modesp/hal/driver_manager.h"

// Phase 5a: Network (optional, compile-time Kconfig toggle — menu "ModESP Network")
#if defined(CONFIG_MODESP_NET_ENABLE)
#include "modesp/net/wifi_service.h"
#include "modesp/net/http_service.h"
#include "modesp/net/http_routes.h"
#include "modesp/net/ws_service.h"
#include "userdata_http.h"
#endif
#include "modesp/services/nvs_helper.h"

// esp_hosted's frame copies (P4 + ESP-Hosted; inert elsewhere) and the bench-only UDP probe
// (compiled in only by the MODESP_NETTEST overlay; a no-op otherwise).
#include "hosted_mem.h"
#include "net_probe.h"

// Cloud backend — обраний Kconfig-choice бекенд надає modesp::cloud_backend()
// (ICloudService). main НЕ називає конкретного класу (MqttService).
// MODESP_CLOUD_NONE → секцію не компілюємо взагалі.
#if defined(CONFIG_MODESP_CLOUD_MQTT)
  #include "modesp/net/cloud_service.h"
#endif

// BLE transport service (optional, compile-time Kconfig toggle — menu "ModESP BLE")
#if defined(CONFIG_MODESP_BLE_ENABLE)
  #include "modesp/ble/ble_service.h"
#endif

// Module includes + instances + registration (auto-generated from project.json)
#include "module_includes.h"

#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_ota_ops.h"
#include "esp_sntp.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "etl/error_handler.h"
#include "etl/exception.h"

#include <cstdlib>
#include <cstring>

static const char* TAG = "main";

// Heap by capability: on the P4 the PSRAM-inflated total hides the internal / DMA-capable
// SRAM that ESP-Hosted, mbedTLS and I2S DMA actually fight over (docs/CONCEPT.md, "Board
// constraints").
static void log_heap(const char* when)
{
    ESP_LOGI(TAG, "Heap %s: total %u | internal %u (largest %u) | DMA %u (largest %u) | PSRAM %u",
             when,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

// esp_hosted drops a frame it cannot find a buffer for and says nothing — on the receive path
// there is neither a log line nor a counter (hosted_mem.h). hosted_mem counts them from inside
// the allocator, where logging could stall the link; this says so from the main loop, every
// 10 s at most and only when a count moved. The second line catches every other DMA-capable
// allocation that failed (the SD card's bounce buffer, an I2S reconfiguration, ...), which
// until now surfaced, if at all, as a symptom somewhere else.
static void report_alloc_failures()
{
    static hosted_mem_stats_t prev = {};
    hosted_mem_stats_t now;
    hosted_mem_get_stats(&now);
    const uint32_t rx  = now.frames_failed_rx - prev.frames_failed_rx;
    const uint32_t tx  = now.frames_failed_tx - prev.frames_failed_tx;
    const uint32_t dma = now.heap_failed_dma - prev.heap_failed_dma;
    if (rx || tx) {
        ESP_LOGW(TAG, "esp_hosted dropped %u received and %u outgoing frame(s): no buffer for the "
                      "copy (copies in %s; DMA free %u, lowest since boot %u)",
                 (unsigned)rx, (unsigned)tx, now.in_psram ? "PSRAM" : "internal RAM",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_DMA));
    }
    if (dma > rx + tx) {
        ESP_LOGW(TAG, "%u DMA-capable allocation(s) failed; the last asked for %u bytes, caps 0x%lx",
                 (unsigned)(dma - rx - tx), (unsigned)now.last_failed_size,
                 (unsigned long)now.last_failed_caps);
    }
    prev = now;
}

// ═══════════════════════════════════════════════════════════════
// Static instances (zero heap allocation)
// ═══════════════════════════════════════════════════════════════

// System services (CRITICAL priority — start first)
static modesp::ErrorService    error_service;
static modesp::LoggerService   logger_service;
static modesp::ConfigService   config_service;
static modesp::PersistService  persist_service;

// Services with dependencies
static modesp::SystemMonitor   system_monitor(error_service);

// HAL + DriverManager (not BaseModule — managed directly)
static modesp::HAL             hal;
static modesp::DriverManager   driver_manager;

// Network services (optional)
#if defined(CONFIG_MODESP_NET_ENABLE)
static modesp::WiFiService     wifi_service;
static modesp::HttpService     http_service;
static modesp::WsService       ws_service;
#endif

// BLE transport (optional — single NimBLE host owner)
#if defined(CONFIG_MODESP_BLE_ENABLE)
static modesp::BleService      ble_service;
#endif

// Module instances (auto-generated from project.json)
#include "module_instances.h"
#include "module_register.h"

// ═══════════════════════════════════════════════════════════════
// ETL assertion handler
// ═══════════════════════════════════════════════════════════════

// Where a failed ETL assertion lands, now that the build defines ETL_LOG_ERRORS and
// ETL_CHECK_PUSH_POP (root CMakeLists.txt). Installed before any module is constructed,
// because ETL containers are static members of the services below.
//
// Why abort(). ETL_CHECK_PUSH_POP's bounds checks are ETL_ASSERT_OR_RETURN: the container
// returns without writing, and the callback is a report. But the plain ETL_ASSERT sites —
// etl::unordered_map::insert on a full map is the one that matters here — CONTINUE into the
// code the assert was guarding as soon as this returns, in ETL's own words: "calls the error
// handler". There is nothing this function can do to stop that, so the choice is a reset
// with a coredump or memory corruption nobody sees. The coredump partition exists for this;
// esp-coredump against the matching ELF gives the container and the caller.
#if !defined(ETL_LOG_ERRORS)
#error "ETL_LOG_ERRORS is not defined — etl::error_handler does not exist and ETL_ASSERT " \
       "compiles to nothing. It is set build-wide in the root CMakeLists.txt; do not drop it."
#endif

// Not volatile and not atomic: every path through this function ends in abort(), so the
// count is only ever read from the coredump, and one racing increment cannot change the
// answer to the question it exists for ("was this the first?").
static uint32_t s_etl_errors = 0;

// e.what() is ETL's TERSE text, because ETL_VERBOSE_ERRORS is deliberately not set: a code of
// the form "<file id><letter>", where the file id is from etl/file_error_numbers.h. The three
// worth knowing by sight: 17A = vector full, 17B = vector empty, 16A = unordered_map full.
// tests/host/test_etl_asserts.cpp pins them.
static void on_etl_error(const etl::exception& e)
{
    s_etl_errors++;
    ESP_LOGE(TAG, "ETL assertion #%lu: %s at etl line %d (17A=vector full, 17B=vector empty, "
                  "16A=map full; ids in etl/file_error_numbers.h) — aborting for a coredump",
             (unsigned long)s_etl_errors, e.what(), (int)e.line_number());
    abort();
}

// ═══════════════════════════════════════════════════════════════
// Safe mode
// ═══════════════════════════════════════════════════════════════

/// A CRITICAL module failed to initialise. ModuleManager::init_all() has always reported
/// that, and the boot has always ignored it — the check lived in App::run(), which nothing
/// called. What it means in practice: config could not read board.json, so HAL has no pins,
/// DriverManager has no drivers, and every product module would bind nothing and run on
/// state that was never loaded. The device used to come up looking healthy and doing none of
/// it.
///
/// Safe mode is the defined answer: say so loudly, publish system.safe_mode so the Web UI and
/// MQTT show it, skip the product modules of the later phases, and keep modesp_net + HTTP
/// alive — a device that can be repaired over the network is the whole point, since the
/// usual cause is a config file that a POST can replace.
///
/// One exception comes before any of that. If this image is still pending OTA verification,
/// the honest move is to restart: the bootloader then rolls back to the image that worked,
/// which beats sitting in safe mode on a new firmware nobody has confirmed.
static void enter_safe_mode(modesp::App& app, const char* phase, bool ota_pending_verify)
{
    ESP_LOGE(TAG, "======================================");
    ESP_LOGE(TAG, "  SAFE MODE — CRITICAL module failed in: %s", phase);
    ESP_LOGE(TAG, "  Product modules will not start. Network + HTTP stay up for repair.");
    ESP_LOGE(TAG, "======================================");

    app.state().set("system.safe_mode", true);

    if (ota_pending_verify) {
        ESP_LOGE(TAG, "OTA: image is pending verification — restarting so the bootloader rolls back");
        esp_restart();
    }
}

// ═══════════════════════════════════════════════════════════════
// Entry point
// ═══════════════════════════════════════════════════════════════

extern "C" void app_main(void)
{
    // First of all, because it decides what the rest of this function is allowed to print.
    // The build ships at WARN; this puts back the handful of tags whose
    // INFO lines have each already settled a fault, including this one.
    modesp::log_policy::apply();

    // Before anything else that holds ETL containers: an assertion that fires with no
    // callback installed is an assertion nobody hears.
    etl::error_handler::set_callback<on_etl_error>();

    ESP_LOGI(TAG, "======================================");
    ESP_LOGI(TAG, "  ModESP Streamer — ModESP v4 control plane");
    ESP_LOGI(TAG, "======================================");

    // ── Step 0: NVS init (before everything) ──
    modesp::nvs_helper::init();

    // ── Step 0b: OTA validity — deferred validation (60s timeout) ──
    const esp_partition_t* running = esp_ota_get_running_partition();
    bool ota_pending_verify = false;
    {
        esp_ota_img_states_t ota_state;
        if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
            if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
                ota_pending_verify = true;
                ESP_LOGW(TAG, "OTA: New firmware — validating (60s timeout)...");
            }
        }
    }
    ESP_LOGI(TAG, "Running from: %s (0x%lx)",
             running->label, (unsigned long)running->address);

    auto& app = modesp::App::instance();

    // ── Step 1: Core init ──
    if (!app.init()) {
        ESP_LOGE(TAG, "App init failed!");
        return;
    }

    // Публікуємо інформацію про прошивку в SharedState
    {
        const esp_app_desc_t* desc = esp_app_get_description();
        app.state().set("_ota.version", desc->version);
        app.state().set("_ota.partition", running->label);
        app.state().set("_ota.idf", desc->idf_ver);

        // Дата + час збірки
        char build_dt[32];
        snprintf(build_dt, sizeof(build_dt), "%s %s", desc->date, desc->time);
        app.state().set("_ota.date", build_dt);
        app.state().set("_ota.board", desc->project_name);

        // Safe mode: the key exists from the start, so the UI and MQTT show "normal" rather
        // than an empty field. enter_safe_mode() sets it to true.
        app.state().set("system.safe_mode", false);

        // OTA статус для cloud feedback (оновлюється з ota_handler.cpp)
        app.state().set("_ota.status", "idle");
        app.state().set("_ota.progress", static_cast<int32_t>(0));
        app.state().set("_ota.error", "");
    }

    // WatchdogService needs ModuleManager reference
    static modesp::WatchdogService watchdog_service(error_service, app.modules());

    // ── Step 2: Register system services (CRITICAL) ──
    app.modules().register_module(error_service);
    app.modules().register_module(watchdog_service);
    app.modules().register_module(logger_service);
    app.modules().register_module(config_service);

    // PersistService: відновлює persisted settings з NVS → SharedState
    // Повинен ініціалізуватися ДО бізнес-модулів (Phase 2)
    persist_service.set_state(&app.state());
    app.modules().register_module(persist_service);

    app.modules().register_module(system_monitor);

    ESP_LOGI(TAG, "Phase 1: Initializing system services...");
    bool safe_mode = !app.modules().init_all(app.state());
    if (safe_mode) enter_safe_mode(app, "system services", ota_pending_verify);

    // ── Step 3: Read config ──
    // In safe mode these are the empty tables a failed parse left behind: HAL brings up no
    // pins and DriverManager creates no drivers, which is the point — a board whose config
    // did not parse must not act on a half-read one.
    const auto& board_cfg = config_service.board_config();
    const auto& bindings  = config_service.binding_table();

    ESP_LOGI(TAG, "Board: %s v%s (%d gpio_out, %d onewire buses)",
             board_cfg.board_name.c_str(),
             board_cfg.board_version.c_str(),
             (int)board_cfg.gpio_outputs.size(),
             (int)board_cfg.onewire_buses.size());

    // ── Step 4: Register WiFi + cloud (HIGH priority) ──
#if defined(CONFIG_MODESP_NET_ENABLE)
    app.modules().register_module(wifi_service);
#endif
    // Cloud-бекенд надає компонент, що злінкувався (generic — без імен класів).
    // Оголошення під guard'ом на рівні функції — видиме у Step 9 нижче; для
    // MODESP_CLOUD_NONE секції немає взагалі (нуль unused-змінних).
#if defined(CONFIG_MODESP_CLOUD_MQTT)
    modesp::ICloudService* cloud = modesp::cloud_backend();
    cloud->set_state(&app.state());
    app.modules().register_module(*cloud);
#endif

    // BLE transport (optional) — after Wi-Fi so NimBLE host inits after the Wi-Fi
    // stack (coexist init order).
#if defined(CONFIG_MODESP_BLE_ENABLE)
#if defined(CONFIG_MODESP_BLE_PROVISIONING)
    ble_service.set_wifi(&wifi_service);   // provisioning: save creds + trigger reconnect
#endif
    app.modules().register_module(ble_service);
#endif

    // ── Step 5: Initialize HAL (GPIO setup) ──
    // A HAL or driver failure is not fatal to the boot: safe mode with a live network is
    // more useful than a return out of app_main, which leaves the device unreachable and
    // needs a cable to diagnose.
    if (!hal.init(board_cfg)) {
        ESP_LOGE(TAG, "HAL init failed!");
        if (!safe_mode) { safe_mode = true; enter_safe_mode(app, "HAL init", ota_pending_verify); }
    }

    // ── Step 6: Create and init all drivers ──
    if (!safe_mode && !driver_manager.init(bindings, hal)) {
        ESP_LOGE(TAG, "DriverManager init failed!");
        safe_mode = true;
        enter_safe_mode(app, "driver init", ota_pending_verify);
    }
    ESP_LOGI(TAG, "Drivers: %d sensors, %d actuators",
             (int)driver_manager.sensor_count(),
             (int)driver_manager.actuator_count());

    // ── Step 7: Register all modules (auto-generated from project.json) ──
    // Skipped in safe mode: a product module binds hardware that is not configured and runs
    // logic on state that was never loaded. The network stack and HTTP below still come up,
    // which is what the device is repaired through.
    if (!safe_mode) {
        modesp_register_modules(app);

        // Generic bind: кожен зареєстрований модуль із залізом резолвить свої
        // драйвери/бекенди у своєму on_bind() за bindings.json — main.cpp не знає
        // імен модулів.
        app.modules().bind_all(driver_manager, bindings, hal);
    } else {
        ESP_LOGW(TAG, "SAFE MODE: product modules not registered");
    }

    log_heap("before Wi-Fi");
    ESP_LOGI(TAG, "Phase 2: Initializing WiFi + business modules...");
    if (!app.modules().init_all(app.state()) && !safe_mode) {
        safe_mode = true;
        enter_safe_mode(app, "WiFi + business modules", ota_pending_verify);
    }

    // ── Step 7b: NTP time sync ──
    {
        bool ntp_enabled = true;
        modesp::nvs_helper::read_bool("time", "ntp_enabled", ntp_enabled);

        char tz[48] = "EET-2EEST,M3.5.0/3,M10.5.0/4";  // Київ
        modesp::nvs_helper::read_str("time", "timezone", tz, sizeof(tz));
        setenv("TZ", tz, 1);
        tzset();

#if defined(CONFIG_MODESP_NET_ENABLE)
        if (ntp_enabled) {
            esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
            esp_sntp_setservername(0, "pool.ntp.org");
            esp_sntp_init();
            ESP_LOGI(TAG, "SNTP started (TZ=%s)", tz);
        } else {
            ESP_LOGI(TAG, "NTP disabled, manual time mode (TZ=%s)", tz);
        }
#else
        (void)ntp_enabled;  // no network — manual time mode only
        ESP_LOGI(TAG, "Networking disabled, manual time mode (TZ=%s)", tz);
#endif
        app.state().set("system.time", "--:--:--");
        app.state().set("system.date", "--.--.----");
    }

    // ── Step 8: Inject dependencies + register HTTP + WS ──
#if defined(CONFIG_MODESP_NET_ENABLE)
    http_service.set_state(&app.state());
    http_service.set_config(&config_service);
    http_service.set_modules(&app.modules());
    http_service.set_wifi(&wifi_service);
    http_service.set_persist(&persist_service);
    http_service.set_hal(&hal);
    // /api/log*: будь-який log-модуль самореєструється через
    // modesp::log_source — main.cpp не називає модуль.

    ws_service.set_state(&app.state());

    app.modules().register_module(http_service);
    app.modules().register_module(ws_service);

    ESP_LOGI(TAG, "Phase 3: Initializing HTTP + WebSocket...");
    if (!app.modules().init_all(app.state()) && !safe_mode) {
        safe_mode = true;
        enter_safe_mode(app, "HTTP + WebSocket", ota_pending_verify);
    }

    // ── Step 9: Connect WS handler + finalize HTTP routes ──
    // Order matters: WS handler must be registered BEFORE the
    // wildcard static handler (/*), otherwise httpd_uri_match_wildcard
    // considers /* as already matching /ws for HTTP_GET.
    if (http_service.server()) {
        ws_service.set_http_server(http_service.server());
#if defined(CONFIG_MODESP_CLOUD_MQTT)
        cloud->set_http_server(http_service.server());
#endif
        // Product modules with their own routes (UPnP, …) get the same window, without
        // main.cpp naming any of them — see modesp/net/http_routes.h. A module that would
        // rather register later than during boot uses the restack instead.
        modesp::http_routes::set_wildcard_ctl(
            [](void* self, bool on) {
                auto* http = static_cast<modesp::HttpService*>(self);
                if (on) http->register_static_handler();
                else    http->unregister_static_handler();
            },
            &http_service);
        // Their routes answer to the same login as /api/*.
        modesp::http_routes::set_auth_check(&modesp::HttpService::check_auth);
        userdata_http_add_routes();          // the user's files, backed up from the web (A6)
        modesp::http_routes::run_all(http_service.server());
        http_service.register_static_handler();  // Must be last (wildcard catch-all)
    } else {
        ESP_LOGW(TAG, "HTTP server not started, WebSocket unavailable");
    }
    net_probe_start();   // bench images only (CONFIG_NET_PROBE_ENABLE); nothing otherwise
#endif // CONFIG_MODESP_NET_ENABLE

    // ── Step 10: Main loop ──
    ESP_LOGI(TAG, "Registered %d modules total", (int)app.modules().module_count());
    log_heap("after init");

    // HW Watchdog — ESP-IDF v5.x auto-ініціалізує TWDT, тому reconfigure
    //
    // The idle tasks are watched again. The override to 0 disabled exactly the check that
    // catches CPU starvation: an idle task that never runs is the symptom of a core being
    // held by a busy task, which is how this board has rebooted — and with idle unwatched
    // the only thing left to notice is `main` missing its own feed, several seconds later
    // and naming the wrong culprit. The mask is built from the Kconfig
    // settings, which is what the IDF startup code would have used.
    bool wdt_subscribed = false;
    uint32_t idle_core_mask = 0;
#if defined(CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0)
    idle_core_mask |= (1 << 0);
#endif
#if defined(CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1)
    idle_core_mask |= (1 << 1);
#endif
    esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms  = MODESP_WDT_TIMEOUT_MS,
        .idle_core_mask = idle_core_mask,
        .trigger_panic = true,
    };
    esp_err_t wdt_err = esp_task_wdt_reconfigure(&wdt_cfg);
    if (wdt_err != ESP_OK) {
        // TWDT не ініціалізований — створюємо
        wdt_err = esp_task_wdt_init(&wdt_cfg);
    }
    if (wdt_err == ESP_OK) {
        esp_err_t add_err = esp_task_wdt_add(nullptr);
        if (add_err == ESP_OK) {
            wdt_subscribed = true;
            ESP_LOGI(TAG, "HW watchdog: %d ms timeout, idle cores 0x%lx",
                     MODESP_WDT_TIMEOUT_MS, (unsigned long)idle_core_mask);
        } else if (add_err == ESP_ERR_INVALID_STATE) {
            // Задача вже підписана — ОК
            wdt_subscribed = true;
            ESP_LOGI(TAG, "HW watchdog: task already subscribed (%d ms)", MODESP_WDT_TIMEOUT_MS);
        } else {
            ESP_LOGW(TAG, "HW watchdog: add task failed: %s", esp_err_to_name(add_err));
        }
    } else {
        ESP_LOGW(TAG, "HW watchdog init failed: %s", esp_err_to_name(wdt_err));
    }

    constexpr int LOOP_HZ = MODESP_MAIN_LOOP_HZ;
    const TickType_t period = pdMS_TO_TICKS(1000 / LOOP_HZ);
    const uint32_t dt_ms = 1000 / LOOP_HZ;

    ESP_LOGI(TAG, "Entering main loop (%d Hz)...", LOOP_HZ);

    // ── What it takes to keep a freshly flashed image ──
    //
    // Wi-Fi and an HTTP server were the whole test, which accepts an image that boots,
    // reaches the network, and plays nothing through a screen that never came up — the two
    // things this device is for. Both additions are things a broken image actually fails:
    //
    //   audio.state == "error" is what AudioModule publishes when its task will not start,
    //   and the module stays RUNNING while saying it, so module state alone cannot see it.
    //
    //   ui_lvgl reaching RUNNING says the panel module survived init. It returns true with
    //   no screen when there is no LCD binding, so this does not roll back a board that was
    //   built without a panel — it rolls back an image whose panel init threw it into ERROR.
    //
    // Deliberately NOT a check that pixels appeared: `ui.screen` stays "boot" for ever on a
    // board whose panel is dead, and a device that reboots every minute because of a broken
    // ribbon cable is worse than one that shows nothing.
    auto ota_healthy = [&app]() -> bool {
        auto v = app.state().get("audio.state");
        if (v.has_value()) {
            if (const auto* s = etl::get_if<modesp::StringValue>(&v.value()))
                if (*s == "error") return false;
        }
        struct Panel { bool found = false; bool running = false; } panel;
        app.modules().for_each_module([](const modesp::BaseModule& m, void* ctx) {
            auto* p = static_cast<Panel*>(ctx);
            if (strcmp(m.name(), "ui_lvgl") != 0) return;
            p->found   = true;
            p->running = (m.state() == modesp::BaseModule::State::RUNNING);
        }, &panel);
        return !panel.found || panel.running;
    };

    TickType_t last_wake = xTaskGetTickCount();
    int64_t    prev_turn_us = esp_timer_get_time();

    while (true) {
        // 1. Update all drivers (sensors read, actuators apply)
        driver_manager.update_all(dt_ms);

        // 2. Update all modules (business logic reads from drivers)
        app.modules().update_all(dt_ms);

        // 3. OTA rollback validation (60s timeout)
        if (ota_pending_verify) {
#if defined(CONFIG_MODESP_NET_ENABLE)
            bool net_ok = wifi_service.is_connected() && http_service.server();
            if (net_ok && ota_healthy()) {
                esp_ota_mark_app_valid_cancel_rollback();
                ota_pending_verify = false;
                ESP_LOGI(TAG, "OTA: Firmware validated (WiFi + HTTP + audio + panel OK "
                              "after %lus)", (unsigned long)app.uptime_sec());
            } else if (app.uptime_sec() > 60) {
                ESP_LOGE(TAG, "OTA: Validation timeout (60s, net %s, audio+panel %s) — "
                              "rollback on next reboot",
                         net_ok ? "ok" : "down", ota_healthy() ? "ok" : "bad");
                esp_restart();
            }
#else
            // No network → no WiFi/HTTP health check. The first full
            // driver+module update cycle completing is the best validity
            // signal we have — accept the image at the END of cycle one.
            esp_ota_mark_app_valid_cancel_rollback();
            ota_pending_verify = false;
            ESP_LOGI(TAG, "OTA: Firmware validated (first main-loop cycle OK; no net check)");
#endif
        }

        // 4. Uptime in SharedState (once per second)
        //
        // No heap figure here. `system.heap_largest` used to be taken in this spot, with
        // heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), once a second — and that call
        // walks every block of every heap, each heap under portENTER_CRITICAL_SAFE. With
        // the 32 MB PSRAM pool in it the walk measured 2.6-4.3 ms with interrupts off on
        // this core (6 700 blocks), past the ~2 ms the satellite UART's RX
        // FIFO holds at 460800 baud; every overrun cost one cover frame. SystemMonitor
        // publishes the key now, from the internal heap only.
        static uint32_t sec_counter = 0;
        sec_counter += dt_ms;
        if (sec_counter >= 1000) {
            sec_counter = 0;
            // track_change=false: діагностичні ключі не тригерять WS delta
            app.state().set("system.uptime",
                            static_cast<int32_t>(app.uptime_sec()), false);

            static uint32_t alloc_report_s = 0;
            if (++alloc_report_s >= 10) {
                alloc_report_s = 0;
                report_alloc_failures();
            }
        }

        // 5. HW watchdog reset
        if (wdt_subscribed) esp_task_wdt_reset();

        // 6. Loop timing — and what happens when a turn did not fit its period
        //
        // Delay-until does not sleep once the wake time has passed, and it moves the
        // reference on by ONE period only: a loop N periods behind runs N turns back to back
        // without blocking. This task is the lowest priority above idle on CPU0, so for as
        // long as that lasts IDLE0 gets no CPU at all, and the task watchdog resets the board
        // 5 s in. That is the reset seen under HTTP load: its coredump has `main` 432 turns
        // (11.65 ms each against 10) past its last sleep, ~715 ms behind, IDLE0 starved for
        // 5.03 s.
        //
        // So a turn that overran drops the missed ones instead of making them up, and gives
        // the core away for one tick before the next: whatever else is ready on CPU0 runs,
        // idle included. The schedule restarts from there.
        if (xTaskDelayUntil(&last_wake, period) == pdFALSE) {
            system_monitor.note_loop_overrun();
            vTaskDelay(1);
            last_wake = xTaskGetTickCount();
        }

        // 7. How long that turn actually took, measured across the delay — the whole point
        //    is to catch the turns where the delay returned late because something above
        //    this task would not give the CPU back. SystemMonitor keeps the worst one and
        //    publishes it as system.main_loop_max_ms.
        const int64_t turn_us = esp_timer_get_time();
        system_monitor.note_loop_period_us((uint32_t)(turn_us - prev_turn_us));
        prev_turn_us = turn_us;
    }
}
