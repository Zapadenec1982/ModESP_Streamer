/**
 * @file mqtt_service.h
 * @brief MQTT client service for remote monitoring and control
 *
 * Publishes SharedState values to MQTT broker and subscribes
 * to command topics for remote control. Topics are auto-generated
 * from module manifests (see generated/mqtt_topics.h).
 *
 * Default: disabled. Enable via POST /api/mqtt or NVS config.
 * NVS namespace: "mqtt"
 */

#pragma once

#include "modesp/net/cloud_service.h"   // ICloudService (спільний контракт)
#include "mqtt_client.h"

#include <atomic>

namespace modesp {

class SharedState;

class MqttService : public ICloudService {
public:
    MqttService() : ICloudService("mqtt", ModulePriority::HIGH) {}

    bool on_init() override;
    void on_update(uint32_t dt_ms) override;
    void on_stop() override;

    // ICloudService
    void set_state(SharedState* s) override { state_ = s; }
    void set_http_server(httpd_handle_t server) override;

    // Public API
    bool is_connected() const { return connected_; }
    bool is_enabled() const { return enabled_; }

    bool save_config(const char* broker, uint16_t port,
                     const char* user, const char* pass,
                     const char* prefix, const char* tenant, bool enabled);
    void reconnect();

private:
    // ESP-MQTT client
    esp_mqtt_client_handle_t client_ = nullptr;
    SharedState* state_ = nullptr;
    httpd_handle_t server_ = nullptr;
    bool http_registered_ = false;

    // NVS config (namespace "mqtt")
    char broker_[128] = {};       // "mqtt://host" або "mqtts://host"
    uint16_t port_ = 1883;
    char user_[64] = {};
    char pass_[64] = {};
    char prefix_[80] = {};        // "modesp/v1/{tenant}/{device}" default
    char tenant_[36] = {};        // tenant slug from NVS (empty = pending)
    char device_id_[8] = {};      // MAC-based device ID (e.g., "A4CF12")
    bool enabled_ = false;        // default-off

    // Optional pinned broker CA (NVS "mqtt"/"broker_ca", PEM). Empty → fall back
    // to the built-in public CA bundle. Lets a private broker with a self-signed
    // cert be verified instead of failing against the public roots.
    char broker_ca_[2048] = {};

    // State tracking
    bool connected_ = false;
    bool secure_ = false;         // current link is TLS (mqtts://) vs plaintext
    bool reconnect_requested_ = false;  // Deferred reconnect (set by httpd, executed by on_update)
    // player-fork: a reconnect hands the old client to the mqtt_stop task (stop_client_async()):
    // esp_mqtt_client_stop() can wait out a whole connect attempt for the client's lock, and the
    // main loop feeds a 5 s watchdog. No new client starts while stopping_ is set.
    std::atomic<bool> stopping_{false};
    esp_mqtt_client_handle_t stopping_client_ = nullptr;
    bool params_publish_requested_ = false;  // One-shot publish of writable params
    uint32_t last_version_ = 0;
    uint32_t publish_timer_ = 0;
    static constexpr uint32_t PUBLISH_INTERVAL_MS = 1000;

    // Reconnect з exponential backoff
    uint32_t reconnect_timer_ms_ = 0;
    uint32_t reconnect_delay_ms_ = MQTT_INITIAL_RECONNECT_MS;
    static constexpr uint32_t MQTT_INITIAL_RECONNECT_MS = 5000;   // 5s
    static constexpr uint32_t MQTT_MAX_RECONNECT_MS = 300000;     // 5 min

    // Heartbeat (metadata, every 30s)
    uint32_t heartbeat_timer_ms_ = 0;
    static constexpr uint32_t HEARTBEAT_INTERVAL_MS = 30000; // 30s

    // Кеш останніх опублікованих значень для delta-publish
    // gen::MQTT_PUBLISH_COUNT (50) + system keys (_ota.status/progress/error) = 53
    static constexpr size_t MAX_PUBLISH_KEYS = 56;
    // 16B covers float/int/bool/alarm_code payloads; longer strings (OTA errors)
    // truncate in cache → extra publish, harmless for rare events
    char last_payloads_[MAX_PUBLISH_KEYS][16] = {};

    // Internal
    void load_config();
    bool start_client();
    void stop_client();
    bool stop_client_async();               // player-fork
    static void stop_task(void* arg);       // player-fork
    void publish_state();
    void publish_params();  // One-shot: publish all writable param current values
    void publish_heartbeat();
    void publish_ha_discovery();
    void publish_ha_entity(const char* state_key, const char* name,
                            const char* entity_type, const char* device_class,
                            const char* unit, const char* state_class,
                            const char* device_id, const char* device_name);
    void handle_incoming(const char* topic, int topic_len,
                         const char* data, int data_len);
    void register_http_handlers();
    void build_default_prefix();

    // HA Auto-Discovery
    bool ha_discovery_pending_ = false;

    // Форматування значення для MQTT payload (на стеку)
    // player-fork: room for the longest value the store holds (a string of
    // MODESP_MAX_STRING_VALUE_LENGTH bytes); format_value() returns the bytes it put in buf.
    static constexpr size_t PAYLOAD_MAX = MODESP_MAX_STRING_VALUE_LENGTH + 1;
    static int format_value(const StateValue& val, char* buf, size_t buf_size);

    // Static ESP-IDF callbacks
    static void mqtt_event_handler(void* args, esp_event_base_t base,
                                    int32_t event_id, void* event_data);

    // HTTP handlers
    static esp_err_t handle_get_mqtt(httpd_req_t* req);
    static esp_err_t handle_post_mqtt(httpd_req_t* req);
    static esp_err_t handle_options(httpd_req_t* req);
    static void set_cors_headers(httpd_req_t* req);
};

} // namespace modesp
