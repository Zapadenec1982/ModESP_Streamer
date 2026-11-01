/**
 * @file wifi_service.cpp
 * @brief WiFi STA/AP management implementation
 *
 * WiFi стек ініціалізується ОДИН раз в on_init().
 * start_sta() / start_ap() тільки змінюють режим і конфіг.
 */

#include "modesp/net/wifi_service.h"
#include "modesp/net/mdns_guard.h"
#include "modesp/services/nvs_helper.h"
#include "modesp/services/device_id.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "mdns.h"

#include <cstring>
#include <cstdio>
#include <algorithm>

static const char* TAG = "WiFi";

namespace modesp {

WiFiService* WiFiService::s_instance_ = nullptr;

namespace {
/// player-fork: the disconnect reasons that mean "this network said no" rather than "not
/// yet": wrong password, a security mode we will not use, a network that is not there in
/// any form we can join. Retrying those only delays the answer; try_connect() gives up on
/// the first one.
bool is_refusal(uint8_t reason) {
    switch (reason) {
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        return true;
    default:
        return false;
    }
}

/// player-fork: a wifi_config_t SSID or password is a byte array, not a C string — a 32-byte SSID
/// and a 64-character password (a raw hex PSK) fill it with no NUL after them. strncpy() to
/// sizeof - 1 dropped the last byte: such a network was searched for under 31 characters, or its
/// key hashed as a 63-character passphrase, and never joined. The config comes zeroed, so a
/// shorter value stays terminated. Returns the bytes copied.
template <size_t N>
size_t copy_field(uint8_t (&dst)[N], const char* src) {
    const size_t n = strnlen(src, N);
    memcpy(dst, src, n);
    return n;
}
}  // namespace

// ── Event handler (static, dispatches to instance) ──────────────

void WiFiService::event_handler(void* arg, esp_event_base_t base,
                                 int32_t event_id, void* event_data) {
    auto* self = static_cast<WiFiService*>(arg);
    if (base == WIFI_EVENT) {
        self->handle_wifi_event(event_id, event_data);
    } else if (base == IP_EVENT) {
        self->handle_ip_event(event_id, event_data);
    }
}

void WiFiService::handle_wifi_event(int32_t event_id, void* event_data) {
    switch (event_id) {
        case WIFI_EVENT_STA_START:
            // player-fork: from here on a refusal belongs to the network being tried.
            if (try_phase_ == TryPhase::WAIT_START) {
                try_refused_ = false;
                try_phase_ = TryPhase::CONNECTING;
            }
            // При AP→STA probe connect викликається явно з attempt_ap_sta_probe()
            // player-fork: and a scan's own AP→APSTA switch starts the STA to scan, not to join.
            if (ap_sta_probing_ || scan_busy_) {
                ESP_LOGD(TAG, "STA started (probe or scan, connect deferred)");
            } else {
                ESP_LOGI(TAG, "STA started, connecting...");
                esp_wifi_connect();
            }
            break;

        case WIFI_EVENT_STA_DISCONNECTED: {
            auto* dis = static_cast<wifi_event_sta_disconnected_t*>(event_data);

            // AP→STA probe fast-fail: повертаємо чистий AP mode
            if (ap_sta_probing_) {
                ESP_LOGD(TAG, "AP→STA probe failed (reason %d)", dis->reason);
                cancel_ap_sta_probe();
                break;
            }

            connected_ = false;
            ip_str_[0] = '\0';
            state_set("wifi.connected", false);
            state_set("wifi.ip", "");
            // player-fork: why, for a screen that has to say "wrong password" rather than
            // "could not connect".
            last_reason_ = dis->reason;
            state_set("wifi.reason", static_cast<int32_t>(dis->reason));
            if (try_phase_ == TryPhase::CONNECTING && is_refusal(dis->reason)) try_refused_ = true;
            ESP_LOGW(TAG, "Disconnected from '%.*s', reason=%d",
                     dis->ssid_len, dis->ssid, dis->reason);
            if (!ap_mode_) {
                reconnect_pending_ = true;
            }
            break;
        }

        case WIFI_EVENT_AP_STACONNECTED: {
            auto* event = static_cast<wifi_event_ap_staconnected_t*>(event_data);
            ESP_LOGI(TAG, "AP: station connected (AID=%d)", event->aid);
            break;
        }

        case WIFI_EVENT_AP_STADISCONNECTED: {
            auto* event = static_cast<wifi_event_ap_stadisconnected_t*>(event_data);
            ESP_LOGI(TAG, "AP: station disconnected (AID=%d)", event->aid);
            break;
        }

        default:
            break;
    }
}

void WiFiService::handle_ip_event(int32_t event_id, void* event_data) {
    if (event_id == IP_EVENT_STA_GOT_IP) {
        auto* event = static_cast<ip_event_got_ip_t*>(event_data);
        snprintf(ip_str_, sizeof(ip_str_), IPSTR, IP2STR(&event->ip_info.ip));

        // AP→STA probe успіх: переходимо в чистий STA mode
        if (ap_sta_probing_) {
            ESP_LOGI(TAG, "AP→STA probe success! Switching to STA, IP: %s", ip_str_);
            ap_sta_probing_ = false;
            ap_mode_ = false;
            connected_ = true;
            reconnect_pending_ = false;
            retry_count_ = 0;
            reconnect_interval_ = 2000;

            // APSTA → чистий STA (вивільнити AP ресурси)
            esp_wifi_set_mode(WIFI_MODE_STA);

            // Перезапустити mDNS на STA інтерфейсі
            stop_mdns();
            start_mdns();

            // Скинути STA watchdog
            disconnect_accum_ms_ = 0;
            stable_timer_ms_ = 0;

            state_set("wifi.connected", true);
            state_set("wifi.mode", "sta");
            state_set("wifi.ssid", ssid_);
            state_set("wifi.ip", ip_str_);
            return;
        }

        connected_ = true;
        reconnect_pending_ = false;
        retry_count_ = 0;
        reconnect_interval_ = 2000;

        state_set("wifi.connected", true);
        state_set("wifi.ssid", ssid_);
        state_set("wifi.ip", ip_str_);

        ESP_LOGI(TAG, "Connected! IP: %s", ip_str_);

        // Запускаємо mDNS після отримання IP в STA mode
        start_mdns();
    }
}

// ── Lifecycle ───────────────────────────────────────────────────

bool WiFiService::on_init() {
    ESP_LOGI(TAG, "Initializing WiFi...");
    s_instance_ = this;   // player-fork

    // TCP/IP та event loop — один раз
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Ініціалізуємо WiFi драйвер один раз (netif створюється лениво)
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Реєструємо event handlers
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, this, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, this, nullptr));

    // Стартуємо в потрібному режимі
    if (load_credentials()) {
        ESP_LOGI(TAG, "Credentials found, starting STA mode (SSID: '%s', pass_len: %d)",
                 ssid_, (int)strlen(password_));
        state_set("wifi.ssid", ssid_);
        return start_sta();
    } else {
        ESP_LOGW(TAG, "No credentials found in NVS, starting AP mode");
        return start_ap();
    }
}

void WiFiService::on_stop() {
    stop_mdns();
}

void WiFiService::on_update(uint32_t dt_ms) {
    // Deferred reconnect (after save_credentials HTTP response was sent)
    if (deferred_reconnect_) {
        deferred_reconnect_timer_ += dt_ms;
        // player-fork: not while a scan holds the radio (start_scan() says why); it runs after it.
        if (deferred_reconnect_timer_ >= DEFERRED_RECONNECT_DELAY_MS && !scan_busy_) {
            deferred_reconnect_ = false;
            deferred_reconnect_timer_ = 0;

            ESP_LOGI(TAG, "Executing deferred reconnect (current: %s)", ap_mode_ ? "AP" : "STA");
            esp_wifi_disconnect();
            esp_wifi_stop();
            wifi_started_ = false;
            ap_mode_ = false;
            connected_ = false;
            start_sta();
            return;
        }
    }

    // player-fork: a network being tried is judged here, before the retry logic below can
    // send the device to its access point over it.
    if (trying_) {
        try_ms_ += dt_ms;
        if (try_phase_ == TryPhase::CONNECTING && connected_) {
            finish_try(true);
        } else if (try_refused_ || try_ms_ >= TRY_TIMEOUT_MS) {
            finish_try(false);
            return;
        }
    }

    // Reconnect логіка (тільки STA mode)
    if (reconnect_pending_ && !ap_mode_) {
        reconnect_timer_ += dt_ms;
        if (reconnect_timer_ >= reconnect_interval_) {
            reconnect_timer_ = 0;

            if (trying_ && retry_count_ >= MAX_RETRIES) {   // player-fork: back, not to the AP
                finish_try(false);
                return;
            }
            if (retry_count_ >= MAX_RETRIES) {
                ESP_LOGW(TAG, "Max retries reached, switching to AP mode");
                reconnect_pending_ = false;
                start_ap();
                return;
            }

            retry_count_++;
            ESP_LOGI(TAG, "Reconnect attempt %d/%d (backoff %lu ms)",
                     retry_count_, MAX_RETRIES, (unsigned long)reconnect_interval_);
            esp_wifi_connect();

            // Exponential backoff
            reconnect_interval_ *= 2;
            if (reconnect_interval_ > MAX_BACKOFF_MS) {
                reconnect_interval_ = MAX_BACKOFF_MS;
            }
        }
    }

    // RSSI update (STA mode, connected)
    //
    // player-fork: DISABLED, and the reason is a reboot rather than a preference.
    // esp_wifi_sta_get_ap_info() is not a local read here — it is a synchronous RPC to the
    // C6 with a five-second timeout, made from the task that feeds the five-second
    // watchdog. While the SDIO link is carrying a radio stream that RPC takes long enough
    // that the two meet, and the board resets (task_wdt on main, the stack shows
    // eh_host_wifi_sta_get_ap_info). And it buys nothing: the note below records that
    // this path returns 0 on this stack anyway. Revisit after an esp_hosted bump, and then
    // only off the main loop.
    if (false && connected_ && !ap_mode_) {
        rssi_timer_ += dt_ms;
        if (rssi_timer_ >= RSSI_INTERVAL_MS) {
            rssi_timer_ = 0;
            // player-fork: over esp_wifi_remote 1.6.4 + esp_hosted 3.0.7 this reads 0 — the
            // sta_get_ap_info RPC response omits rssi (host glue copies ssid/bssid/channel/
            // authmode only) and esp_wifi_sta_get_rssi() cannot even be composed by the host
            // RPC layer (msg_id 341 "pack_req_payload failed"). Kept as the upstream code
            // path; revisit after an esp_hosted bump.
            wifi_ap_record_t ap_info;
            if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
                state_set("wifi.rssi", static_cast<int32_t>(ap_info.rssi));
            }
        }
    }

    // ── AP→STA periodic probe ──
    // player-fork: and not while a scan holds the radio; the countdown waits for it.
    if (ap_mode_ && !ap_sta_probing_ && !deferred_reconnect_ && !scan_busy_ && ssid_[0] != '\0') {
        if (ap_sta_probe_timer_ > dt_ms) {
            ap_sta_probe_timer_ -= dt_ms;
        } else {
            ap_sta_probe_timer_ = 0;
        }
        if (ap_sta_probe_timer_ == 0) {
            // player-fork: internal RAM, as start_scan() measures it, not the whole heap.
            const size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            if (free_internal >= AP_STA_PROBE_HEAP_MIN) {
                attempt_ap_sta_probe();
            } else {
                ESP_LOGW(TAG, "AP→STA probe skipped: low internal heap %lu",
                         (unsigned long)free_internal);
                ap_sta_probe_timer_ = ap_sta_probe_interval_;
            }
        }
    }

    // ── Probe timeout ──
    if (ap_sta_probing_) {
        if (ap_sta_probe_timeout_ > dt_ms) {
            ap_sta_probe_timeout_ -= dt_ms;
        } else {
            ap_sta_probe_timeout_ = 0;
        }
        if (ap_sta_probe_timeout_ == 0) {
            ESP_LOGW(TAG, "AP→STA probe #%u timeout", ap_sta_probe_count_);
            cancel_ap_sta_probe();
        }
    }

    // STA Watchdog: рестарт при тривалому disconnect
    if (!ap_mode_) {
        if (connected_) {
            stable_timer_ms_ += dt_ms;
            if (stable_timer_ms_ >= WIFI_STABLE_RESET_MS) {
                // Стабільне з'єднання 1 годину — скидаємо лічильник
                disconnect_accum_ms_ = 0;
                stable_timer_ms_ = 0;
            }
        } else {
            stable_timer_ms_ = 0;
            disconnect_accum_ms_ += dt_ms;
            if (disconnect_accum_ms_ >= WIFI_MAX_DISCONNECT_MS) {
                ESP_LOGE(TAG, "WiFi unstable (%lums disconnect), restarting...",
                         (unsigned long)disconnect_accum_ms_);
                esp_restart();
            }
        }
    }
}

// ── Credentials ─────────────────────────────────────────────────

bool WiFiService::load_credentials() {
    bool ok = nvs_helper::read_str("wifi", "ssid", ssid_, sizeof(ssid_));
    if (!ok || ssid_[0] == '\0') return false;

    nvs_helper::read_str("wifi", "pass", password_, sizeof(password_));
    return true;
}

bool WiFiService::save_credentials(const char* ssid, const char* password) {
    if (!ssid || ssid[0] == '\0') return false;

    bool ok = nvs_helper::write_str("wifi", "ssid", ssid);
    if (ok && password) {
        ok = nvs_helper::write_str("wifi", "pass", password);
    }
    if (ok) {
        strncpy(ssid_, ssid, sizeof(ssid_) - 1);
        ssid_[sizeof(ssid_) - 1] = '\0';
        if (password) {
            strncpy(password_, password, sizeof(password_) - 1);
            password_[sizeof(password_) - 1] = '\0';
        }
        ESP_LOGI(TAG, "Credentials saved for SSID: %s", ssid_);
    }
    return ok;
}

void WiFiService::request_reconnect() {
    ESP_LOGI(TAG, "Reconnect requested — deferred by %d ms to let HTTP response reach client",
             (int)DEFERRED_RECONNECT_DELAY_MS);
    if (ap_sta_probing_) {
        cancel_ap_sta_probe();
    }
    deferred_reconnect_ = true;
    deferred_reconnect_timer_ = 0;
}

// ── player-fork: try a network, keep the old one until the new one works ─────────────────────

bool WiFiService::try_connect(const char* ssid, const char* password) {
    if (!ssid || ssid[0] == '\0' || trying_) return false;

    strncpy(prev_ssid_, ssid_, sizeof(prev_ssid_) - 1);
    prev_ssid_[sizeof(prev_ssid_) - 1] = '\0';
    strncpy(prev_pass_, password_, sizeof(prev_pass_) - 1);
    prev_pass_[sizeof(prev_pass_) - 1] = '\0';

    strncpy(ssid_, ssid, sizeof(ssid_) - 1);
    ssid_[sizeof(ssid_) - 1] = '\0';
    strncpy(password_, password ? password : "", sizeof(password_) - 1);
    password_[sizeof(password_) - 1] = '\0';

    trying_ = true;
    try_ms_ = 0;
    try_refused_ = false;
    try_phase_ = TryPhase::WAIT_START;
    last_reason_ = 0;
    state_set("wifi.try_ssid", ssid_);
    state_set("wifi.try", "connecting");
    ESP_LOGI(TAG, "Trying '%s' (keeping '%s' until it answers)", ssid_, prev_ssid_);
    request_reconnect();
    return true;
}

void WiFiService::finish_try(bool ok) {
    trying_ = false;
    try_phase_ = TryPhase::IDLE;
    try_refused_ = false;
    if (ok) {
        save_credentials(ssid_, password_);
        state_set("wifi.try", "ok");
        ESP_LOGI(TAG, "'%s' works, saved", ssid_);
    } else {
        ESP_LOGW(TAG, "'%s' failed (reason %ld, %lu ms) — back to '%s'", ssid_,
                 (long)last_reason_.load(), (unsigned long)try_ms_, prev_ssid_);
        strncpy(ssid_, prev_ssid_, sizeof(ssid_) - 1);
        ssid_[sizeof(ssid_) - 1] = '\0';
        strncpy(password_, prev_pass_, sizeof(password_) - 1);
        password_[sizeof(password_) - 1] = '\0';
        state_set("wifi.try", "fail");
        if (ssid_[0] != '\0') {
            request_reconnect();
        } else {
            start_ap();   // there was no network before this one: the access point, as at boot
        }
    }
    memset(prev_pass_, 0, sizeof(prev_pass_));
}

void WiFiService::forget_credentials() {
    nvs_helper::erase_key("wifi", "ssid");
    nvs_helper::erase_key("wifi", "pass");
    ESP_LOGW(TAG, "Forgetting '%s' — access point mode", ssid_);
    ssid_[0] = '\0';
    memset(password_, 0, sizeof(password_));
    trying_ = false;
    try_phase_ = TryPhase::IDLE;
    deferred_reconnect_ = false;
    state_set("wifi.try", "");
    start_ap();
}

// ── WiFi Scan ───────────────────────────────────────────────────

bool WiFiService::start_scan() {
    // player-fork: one scan at a time, and none while a join or a probe has the radio. The glass
    // scans from its own task and the web from the server's, and nothing kept them apart, from
    // each other or from the main loop: a second scan stopped the first, the AP restore at the
    // end of a scan put a join's STA back to AP (a right password failed after 25 s), and a scan
    // cancelled a probe and doubled its backoff. scan_busy_ is claimed before the main loop's
    // flags are read; the main loop sets its flag before it reads scan_busy_ (the deferred
    // reconnect, attempt_ap_sta_probe()), so whichever comes second sees the other and waits.
    bool idle = false;
    if (!scan_busy_.compare_exchange_strong(idle, true)) {
        ESP_LOGW(TAG, "Cannot scan: another scan is running");
        return false;
    }
    if (trying_ || deferred_reconnect_ || ap_sta_probing_) {
        ESP_LOGW(TAG, "Cannot scan: a join or an AP->STA attempt has the radio");
        scan_busy_ = false;
        return false;
    }
    scan_done_ = false;

    if (!wifi_started_) {
        ESP_LOGW(TAG, "Cannot scan: WiFi not started");
        scan_busy_ = false;
        return false;
    }

    // APSTA + scan need ~30-40 KB more. player-fork: of internal RAM, the kind the Wi-Fi path
    // allocates from and the kind that runs out. With CONFIG_SPIRAM_USE_MALLOC=y the whole heap
    // also counts every free byte of PSRAM, tens of MB, and a check on it could never refuse.
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "Scan requested (internal heap: %u)", (unsigned)free_heap);
    if (free_heap < 50000) {
        ESP_LOGW(TAG, "Cannot scan: low memory (%u bytes of internal heap free, need 50000)",
                 (unsigned)free_heap);
        scan_busy_ = false;
        return false;
    }

    // В AP mode потрібно переключитись в APSTA для сканування.
    // Порожній STA config щоб STA не почала connecting.
    bool was_ap_only = false;
    if (ap_mode_) {
        wifi_mode_t current_mode;
        esp_wifi_get_mode(&current_mode);
        if (current_mode == WIFI_MODE_AP) {
            ensure_sta_netif();
            ESP_LOGI(TAG, "Switching AP -> APSTA for scan");
            esp_err_t mode_err = esp_wifi_set_mode(WIFI_MODE_APSTA);
            if (mode_err != ESP_OK) {
                ESP_LOGW(TAG, "Cannot switch to APSTA: %s", esp_err_to_name(mode_err));
                scan_busy_ = false;
                return false;
            }
            wifi_config_t sta_cfg = {};
            esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
            was_ap_only = true;
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    wifi_scan_config_t scan_config = {};
    scan_config.show_hidden = false;
    scan_config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scan_config.scan_time.active.min = 120;
    scan_config.scan_time.active.max = 500;  // 500ms per channel — більше шансів знайти всі AP

    esp_err_t err = esp_wifi_scan_start(&scan_config, true);  // blocking scan

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Scan failed: %s (heap: %u)", esp_err_to_name(err),
                 (unsigned)esp_get_free_heap_size());
        esp_wifi_scan_stop();
        if (was_ap_only) esp_wifi_set_mode(WIFI_MODE_AP);
        scan_busy_ = false;
        return false;
    }

    scan_done_ = true;
    restore_ap_after_scan_ = was_ap_only;
    ESP_LOGI(TAG, "WiFi scan completed (heap: %u)", (unsigned)esp_get_free_heap_size());
    return true;
}

int WiFiService::get_scan_results(ScanResult* out, size_t max_results) {
    if (!scan_done_) return 0;

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);

    // player-fork: up to MAX_SCAN_RESULTS, in PSRAM. The stack array held ten, and a block of
    // flats hears more networks than that — the panel's own list would stop at whichever ten
    // the driver happened to put first. The records are copied out at once and freed.
    if (max_results > MAX_SCAN_RESULTS) max_results = MAX_SCAN_RESULTS;
    if (ap_count > max_results) ap_count = static_cast<uint16_t>(max_results);

    auto* ap_records = static_cast<wifi_ap_record_t*>(
        heap_caps_calloc(ap_count ? ap_count : 1, sizeof(wifi_ap_record_t),
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!ap_records) {
        ESP_LOGW(TAG, "No memory for %u scan records", (unsigned)ap_count);
        esp_wifi_clear_ap_list();
        ap_count = 0;
    } else {
        esp_wifi_scan_get_ap_records(&ap_count, ap_records);
    }

    for (uint16_t i = 0; i < ap_count; i++) {
        strncpy(out[i].ssid, reinterpret_cast<const char*>(ap_records[i].ssid), 32);
        out[i].ssid[32] = '\0';
        out[i].rssi = ap_records[i].rssi;
        out[i].authmode = static_cast<uint8_t>(ap_records[i].authmode);
    }
    heap_caps_free(ap_records);

    scan_done_ = false;

    // Повертаємо AP mode ПІСЛЯ зчитування результатів —
    // esp_wifi_scan_get_ap_records() звільняє внутрішні буфери ESP-IDF
    // player-fork: unless a join was asked for meanwhile — its reconnect waited for this scan and
    // takes the radio from here, so it is not put back to AP under it.
    if (restore_ap_after_scan_ && !trying_) {
        ESP_LOGI(TAG, "Restoring AP mode after scan results read (heap: %u)",
                 (unsigned)esp_get_free_heap_size());
        esp_wifi_set_mode(WIFI_MODE_AP);
    }
    restore_ap_after_scan_ = false;
    scan_busy_ = false;

    return static_cast<int>(ap_count);
}

// ── Lazy netif creation ──────────────────────────────────────────

void WiFiService::ensure_sta_netif() {
    if (!sta_netif_) {
        sta_netif_ = esp_netif_create_default_wifi_sta();
        ESP_LOGI(TAG, "STA netif created (heap: %u)", (unsigned)esp_get_free_heap_size());
    }
}

void WiFiService::ensure_ap_netif() {
    if (!ap_netif_) {
        ap_netif_ = esp_netif_create_default_wifi_ap();
        ESP_LOGI(TAG, "AP netif created (heap: %u)", (unsigned)esp_get_free_heap_size());
    }
}

// ── STA / AP ────────────────────────────────────────────────────

bool WiFiService::start_sta() {
    ensure_sta_netif();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    wifi_config_t wifi_config = {};
    copy_field(wifi_config.sta.ssid, ssid_);
    copy_field(wifi_config.sta.password, password_);
    // player-fork: an open network has no password, and a WPA2 floor refuses it outright
    // (reason 211, "no AP in the authmode threshold") — the panel could list it and never
    // join it. A network with a password keeps the WPA2 floor.
    wifi_config.sta.threshold.authmode = password_[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_LOGI(TAG, "Starting STA: SSID='%s' pass_len=%d",
             ssid_, (int)strlen(password_));

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

#if defined(CONFIG_MODESP_NET_SET_DEFAULT_NETIF)
    // Wi-Fi on a co-processor (ESP-Hosted / esp_wifi_remote): make the STA netif the
    // default route so unbound UDP sockets (SNTP) leave through it — docs/NETWORK.md, "WiFiService".
    esp_netif_set_default_netif(sta_netif_);
#endif

    // Country code UA (канали 1-13) — потрібно викликати ПІСЛЯ esp_wifi_start(),
    // інакше налаштування не застосовуються у деяких версіях ESP-IDF v5.x
    {
        wifi_country_t country = {};
        country.cc[0] = 'U'; country.cc[1] = 'A'; country.cc[2] = '\0';
        country.schan = 1;
        country.nchan = 13;
        country.max_tx_power = 80;  // 20 dBm (значення в 0.25 dBm)
        country.policy = WIFI_COUNTRY_POLICY_MANUAL;
        esp_wifi_set_country(&country);
    }

    ap_mode_ = false;
    wifi_started_ = true;
    retry_count_ = 0;
    reconnect_interval_ = 2000;
    reconnect_timer_ = 0;
    reconnect_pending_ = false;
    state_set("wifi.mode", "sta");
    return true;
}

bool WiFiService::start_ap() {
    ensure_ap_netif();

    // Зупиняємо якщо працює (переключення STA→AP)
    if (wifi_started_) {
        esp_wifi_disconnect();
        esp_wifi_stop();
        wifi_started_ = false;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    // Спочатку генеруємо дефолтний SSID з базового MAC (device_mac — портабельно)
    const uint8_t* mac = modesp::device_mac();
    char ap_ssid[33];
    snprintf(ap_ssid, sizeof(ap_ssid), "ModESP-%02X%02X", mac[4], mac[5]);

    char ap_pass[64];
    int32_t ap_channel = 1;

    // Читаємо кастомну конфігурацію AP з NVS (якщо є)
    char custom_ssid[33] = {};
    if (nvs_helper::read_str("wifi", "ap_ssid", custom_ssid, sizeof(custom_ssid))
        && custom_ssid[0] != '\0') {
        strncpy(ap_ssid, custom_ssid, sizeof(ap_ssid) - 1);
        ap_ssid[sizeof(ap_ssid) - 1] = '\0';
    }
    const size_t stored_pass = read_ap_password(ap_pass);
    if (stored_pass > strlen(ap_pass)) {
        ESP_LOGE(TAG, "AP password is %u bytes and WPA2 takes 63: the AP uses its first 63 - "
                      "set it again", (unsigned)stored_pass);
    }
    nvs_helper::read_i32("wifi", "ap_chan", ap_channel);
    if (ap_channel < 1 || ap_channel > 13) ap_channel = 1;

    wifi_config_t wifi_config = {};
    wifi_config.ap.ssid_len = static_cast<uint8_t>(copy_field(wifi_config.ap.ssid, ap_ssid));
    wifi_config.ap.channel = static_cast<uint8_t>(ap_channel);
    wifi_config.ap.max_connection = 2;

    // Пароль >= 8 символів → WPA2, інакше відкрита мережа
    if (strlen(ap_pass) >= 8) {
        copy_field(wifi_config.ap.password, ap_pass);
        wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Country code UA — після esp_wifi_start()
    {
        wifi_country_t country = {};
        country.cc[0] = 'U'; country.cc[1] = 'A'; country.cc[2] = '\0';
        country.schan = 1;
        country.nchan = 13;
        country.max_tx_power = 80;
        country.policy = WIFI_COUNTRY_POLICY_MANUAL;
        esp_wifi_set_country(&country);
    }

    ap_mode_ = true;
    wifi_started_ = true;
    connected_ = false;
    reconnect_pending_ = false;

    // Ініціалізація AP→STA probe (якщо є збережені credentials)
    ap_sta_probing_        = false;
    ap_sta_probe_timer_    = AP_STA_PROBE_INITIAL_MS;
    ap_sta_probe_interval_ = AP_STA_PROBE_INITIAL_MS;
    ap_sta_probe_count_    = 0;

    // AP IP завжди 192.168.4.1
    strncpy(ip_str_, "192.168.4.1", sizeof(ip_str_));
    state_set("wifi.mode", "ap");
    state_set("wifi.ssid", ap_ssid);
    state_set("wifi.ip", ip_str_);
    state_set("wifi.connected", false);

    ESP_LOGI(TAG, "AP mode started: %s ch=%ld %s", ap_ssid, (long)ap_channel,
             wifi_config.ap.authmode == WIFI_AUTH_WPA2_PSK ? "WPA2" : "open");

    // Запускаємо mDNS в AP mode (клієнти AP можуть використовувати .local)
    start_mdns();
    return true;
}

size_t WiFiService::read_ap_password(char (&out)[64]) {
    // player-fork: with room for anything POST /api/wifi/ap could ever store (its body is 256
    // bytes). Read into 65, a longer password did not read at all and was taken for none: the
    // access point came up open while its owner believed it was WPA2. A stored password is
    // never taken for none now; one longer than WPA2 takes is cut to it, and start_ap() says so.
    char stored[256] = {};
    out[0] = '\0';
    if (!nvs_helper::read_str("wifi", "ap_pass", stored, sizeof(stored))) return 0;
    strncpy(out, stored, sizeof(out) - 1);
    out[sizeof(out) - 1] = '\0';
    return strlen(stored);
}

// ── AP→STA Probe ─────────────────────────────────────────────────

void WiFiService::attempt_ap_sta_probe() {
    // Встановити ДО mode switch — STA_START handler перевіряє цей прапорець
    // player-fork: and before scan_busy_ is read, as start_scan() claims scan_busy_ before it
    // reads this one: a scan that started meanwhile keeps the radio, and the probe waits.
    ap_sta_probing_ = true;
    if (scan_busy_) {
        ap_sta_probing_ = false;
        return;
    }
    ap_sta_probe_count_++;
    ESP_LOGI(TAG, "AP→STA probe #%u (interval %lus)",
             ap_sta_probe_count_,
             (unsigned long)(ap_sta_probe_interval_ / 1000));

    ap_sta_probe_timeout_ = AP_STA_PROBE_TIMEOUT_MS;

    ensure_sta_netif();
    esp_wifi_set_mode(WIFI_MODE_APSTA);

    // Config + connect ПІСЛЯ mode switch (STA_START не викликає auto-connect)
    wifi_config_t sta_cfg = {};
    copy_field(sta_cfg.sta.ssid, ssid_);
    copy_field(sta_cfg.sta.password, password_);
    sta_cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    sta_cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    esp_wifi_connect();
}

void WiFiService::cancel_ap_sta_probe() {
    // Встановити прапорець ДО disconnect, щоб STA_DISCONNECTED event не викликав рекурсію
    ap_sta_probing_ = false;

    esp_wifi_disconnect();
    esp_wifi_set_mode(WIFI_MODE_AP);

    // Backoff: 30s → 60s → 120s → 240s → 300s (cap)
    ap_sta_probe_interval_ = std::min(ap_sta_probe_interval_ * 2, AP_STA_PROBE_MAX_MS);
    ap_sta_probe_timer_    = ap_sta_probe_interval_;

    ESP_LOGD(TAG, "AP→STA probe cancelled, next in %lus",
             (unsigned long)(ap_sta_probe_interval_ / 1000));
}

// ── mDNS ────────────────────────────────────────────────────────

void WiFiService::start_mdns() {
    MdnsGuard guard;
    if (!guard.locked()) return;
    if (mdns_started_) return;

    const uint8_t* mac = modesp::device_mac();
    char hostname[24];
    snprintf(hostname, sizeof(hostname), "modesp-%02x%02x", mac[4], mac[5]);

    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS init failed: %s", esp_err_to_name(err));
        return;
    }

    mdns_hostname_set(hostname);
    mdns_instance_name_set("ModESP Controller");

    // Реєструємо HTTP сервіс (для виявлення WebUI в мережі)
    mdns_txt_item_t txt_items[] = {
        {"path", "/"},
    };
    mdns_service_add(nullptr, "_http", "_tcp", 80, txt_items, 1);

    mdns_started_ = true;
    state_set("system.mdns_hostname", hostname);
    ESP_LOGI(TAG, "mDNS started: %s.local", hostname);
}

void WiFiService::stop_mdns() {
    MdnsGuard guard;
    if (!guard.locked()) return;
    if (!mdns_started_) return;
    mdns_free();
    mdns_started_ = false;
    ESP_LOGI(TAG, "mDNS stopped");
}

} // namespace modesp
