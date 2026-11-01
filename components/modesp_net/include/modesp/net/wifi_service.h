/**
 * @file wifi_service.h
 * @brief WiFi station/AP management service
 *
 * Connects to a configured AP (STA mode) or creates a fallback AP
 * if no credentials are stored in NVS. Handles reconnection with
 * exponential backoff.
 */

#pragma once

#include "modesp/base_module.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include <atomic>

namespace modesp {

class WiFiService : public BaseModule {
public:
    WiFiService() : BaseModule("wifi", ModulePriority::HIGH) {}

    bool on_init() override;
    void on_update(uint32_t dt_ms) override;
    void on_stop() override;

    /// player-fork: the one instance, for a product module that has no other way to reach it
    /// (main.cpp does not name product modules). Null until on_init() has run.
    static WiFiService* instance() { return s_instance_; }

    // Current state
    bool is_connected() const { return connected_; }
    const char* ip_address() const { return ip_str_; }
    bool is_ap_mode() const { return ap_mode_; }

    // Control
    bool save_credentials(const char* ssid, const char* password);
    void request_reconnect();

    /// player-fork: join another network WITHOUT giving up the one that works.
    ///
    /// save_credentials() writes first and asks questions later: a mistyped password leaves
    /// the device on a network it cannot join, and after three retries in its own access
    /// point, reachable only by someone who walks up to it with a phone. This keeps the old
    /// pair in memory, connects with the new one, and writes it to NVS only once an address
    /// has been obtained. Refused credentials, no such network, or TRY_TIMEOUT_MS without an
    /// address put the old pair back and reconnect with it (or the access point, when there
    /// was none). Progress is published as `wifi.try` ("connecting", "ok", "fail") and
    /// `wifi.try_ssid`; the reason of the last refusal is `wifi.reason`.
    ///
    /// Main-loop only, like on_update(). Returns false when an attempt is already running.
    bool try_connect(const char* ssid, const char* password);
    bool is_trying() const { return trying_; }

    /// player-fork: forget the stored network and fall back to the access point.
    void forget_credentials();

    /// player-fork: a saved network exists (the AP fallback may be up regardless).
    bool has_credentials() const { return ssid_[0] != '\0'; }

    /// player-fork: the access point's password (NVS wifi/ap_pass) as start_ap() uses it. WPA2
    /// takes 8..63 bytes, so `out` gets at most 63, and fewer than 8 mean an open access point.
    /// Returns the length stored: more than 63 means `out` holds the first 63 of it.
    static size_t read_ap_password(char (&out)[64]);

    // Scan
    static constexpr size_t MAX_SCAN_RESULTS = 20;
    struct ScanResult {
        char ssid[33];
        int8_t rssi;
        uint8_t authmode;  // wifi_auth_mode_t
    };
    bool start_scan();
    int  get_scan_results(ScanResult* out, size_t max_results);
    bool is_scan_done() const { return scan_done_; }

private:
    static WiFiService* s_instance_;

    // player-fork: try_connect()
    //
    // WAIT_START ignores the disconnect our own teardown produces (reason 8, leaving): it is
    // queued before STA_START, so it is not the new network refusing anything.
    enum class TryPhase : uint8_t { IDLE, WAIT_START, CONNECTING };
    std::atomic<bool>     trying_{false};        ///< player-fork: atomic, start_scan() reads it
    std::atomic<TryPhase> try_phase_{TryPhase::IDLE};
    std::atomic<bool>     try_refused_{false};   ///< set on the event task, read on the loop
    std::atomic<int32_t>  last_reason_{0};
    uint32_t try_ms_ = 0;
    char     prev_ssid_[33] = {};
    char     prev_pass_[65] = {};
    static constexpr uint32_t TRY_TIMEOUT_MS = 25000;
    void finish_try(bool ok);

    bool connected_ = false;
    bool ap_mode_ = false;
    bool wifi_started_ = false;
    char ip_str_[16] = {};
    char ssid_[33] = {};
    char password_[65] = {};

    // Netif handles (створюються один раз)
    esp_netif_t* sta_netif_ = nullptr;
    esp_netif_t* ap_netif_  = nullptr;

    // mDNS
    bool mdns_started_ = false;

    // Scan state
    bool scan_done_ = false;
    bool restore_ap_after_scan_ = false;  // AP mode restore deferred until results read
    /// player-fork: a scan holds the radio, from start_scan() until get_scan_results() has put
    /// it back. Scans run on the glass's task and on the server's; see start_scan().
    std::atomic<bool> scan_busy_{false};

    // Deferred reconnect (give HTTP response time to reach client)
    std::atomic<bool> deferred_reconnect_{false};   // player-fork: atomic, start_scan() reads it
    uint32_t deferred_reconnect_timer_ = 0;
    static constexpr uint32_t DEFERRED_RECONNECT_DELAY_MS = 1500;

    // Reconnect logic
    uint32_t reconnect_timer_ = 0;
    uint32_t reconnect_interval_ = 2000;
    uint8_t  retry_count_ = 0;
    bool     reconnect_pending_ = false;
    static constexpr uint8_t MAX_RETRIES = 3;
    static constexpr uint32_t MAX_BACKOFF_MS = 32000;

    // RSSI update throttle
    uint32_t rssi_timer_ = 0;
    static constexpr uint32_t RSSI_INTERVAL_MS = 10000;

    // STA watchdog: restart якщо disconnect сумарно > 10 хв
    uint32_t disconnect_accum_ms_ = 0;
    uint32_t stable_timer_ms_ = 0;
    static constexpr uint32_t WIFI_MAX_DISCONNECT_MS = 600000;   // 10 min
    static constexpr uint32_t WIFI_STABLE_RESET_MS = 3600000;    // 1 hour

    // AP→STA periodic probe: спроби STA підключення з AP mode
    std::atomic<bool> ap_sta_probing_{false};    // player-fork: atomic, start_scan() reads it
    uint32_t ap_sta_probe_timer_    = 0;
    uint32_t ap_sta_probe_interval_ = 30000;
    uint32_t ap_sta_probe_timeout_  = 0;
    uint8_t  ap_sta_probe_count_    = 0;
    static constexpr uint32_t AP_STA_PROBE_INITIAL_MS  = 30000;   // 30s
    static constexpr uint32_t AP_STA_PROBE_MAX_MS      = 300000;  // 5 min
    static constexpr uint32_t AP_STA_PROBE_TIMEOUT_MS  = 15000;   // 15s
    static constexpr uint32_t AP_STA_PROBE_HEAP_MIN    = 51200;   // 50KB of internal heap

    bool load_credentials();
    bool start_sta();
    bool start_ap();
    void ensure_sta_netif();
    void ensure_ap_netif();
    void start_mdns();
    void stop_mdns();
    void attempt_ap_sta_probe();
    void cancel_ap_sta_probe();

    // ESP event handler (static because ESP-IDF API requires function pointer)
    static void event_handler(void* arg, esp_event_base_t base,
                              int32_t event_id, void* event_data);
    void handle_wifi_event(int32_t event_id, void* event_data);
    void handle_ip_event(int32_t event_id, void* event_data);
};

} // namespace modesp
