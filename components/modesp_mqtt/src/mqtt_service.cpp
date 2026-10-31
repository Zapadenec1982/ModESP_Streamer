/**
 * @file mqtt_service.cpp
 * @brief MQTT client — publish state, subscribe commands, HTTP config API
 */

#include "modesp/net/mqtt_service.h"
#include "modesp/net/http_service.h"
#include "modesp/net/json_escape.h"
#include "modesp/net/ota_handler.h"
#include "modesp/shared_state.h"
#include "modesp/services/nvs_helper.h"
#include "modesp/types.h"

#include "mqtt_topics.h"
#include "state_meta.h"

#include "esp_log.h"
#include "modesp/services/device_id.h"
#include "esp_crt_bundle.h"
#include "esp_wifi.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define JSMN_STATIC
#include "jsmn.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>

static const char* TAG = "MQTT";

namespace modesp {

// ── NVS config ──────────────────────────────────────────────────

void MqttService::load_config() {
    nvs_helper::read_str("mqtt", "broker", broker_, sizeof(broker_));
    nvs_helper::read_str("mqtt", "user", user_, sizeof(user_));
    nvs_helper::read_str("mqtt", "pass", pass_, sizeof(pass_));
    nvs_helper::read_str("mqtt", "prefix", prefix_, sizeof(prefix_));
    nvs_helper::read_str("mqtt", "tenant", tenant_, sizeof(tenant_));
    nvs_helper::read_str("mqtt", "broker_ca", broker_ca_, sizeof(broker_ca_));
    nvs_helper::read_bool("mqtt", "enabled", enabled_);

    int32_t p = 1883;
    if (nvs_helper::read_i32("mqtt", "port", p)) {
        port_ = static_cast<uint16_t>(p);
    }
}

bool MqttService::save_config(const char* broker, uint16_t port,
                               const char* user, const char* pass,
                               const char* prefix, const char* tenant, bool enabled) {
    bool ok = true;
    if (broker)  ok &= nvs_helper::write_str("mqtt", "broker", broker);
    if (user)    ok &= nvs_helper::write_str("mqtt", "user", user);
    if (pass)    ok &= nvs_helper::write_str("mqtt", "pass", pass);
    if (prefix)  ok &= nvs_helper::write_str("mqtt", "prefix", prefix);
    if (tenant)  ok &= nvs_helper::write_str("mqtt", "tenant", tenant);
    ok &= nvs_helper::write_i32("mqtt", "port", static_cast<int32_t>(port));
    ok &= nvs_helper::write_bool("mqtt", "enabled", enabled);

    if (ok) {
        if (broker) {
            strncpy(broker_, broker, sizeof(broker_) - 1);
            broker_[sizeof(broker_) - 1] = '\0';
        }
        port_ = port;
        if (user) {
            strncpy(user_, user, sizeof(user_) - 1);
            user_[sizeof(user_) - 1] = '\0';
        }
        if (pass) {
            strncpy(pass_, pass, sizeof(pass_) - 1);
            pass_[sizeof(pass_) - 1] = '\0';
        }
        if (prefix) {
            strncpy(prefix_, prefix, sizeof(prefix_) - 1);
            prefix_[sizeof(prefix_) - 1] = '\0';
        }
        if (tenant) {
            strncpy(tenant_, tenant, sizeof(tenant_) - 1);
            tenant_[sizeof(tenant_) - 1] = '\0';
        }
        enabled_ = enabled;
        // Rebuild prefix if it was cleared (e.g. tenant change without explicit prefix)
        build_default_prefix();
        ESP_LOGI(TAG, "Config saved (broker=%s, port=%d, tenant=%s, enabled=%d)",
                 broker_, port_, tenant_, enabled_);
    }
    return ok;
}

void MqttService::build_default_prefix() {
    // Device id from the base MAC (modesp::device_id — portable; the P4 has no Wi-Fi MAC)
    snprintf(device_id_, sizeof(device_id_), "%s", modesp::device_id());

    if (prefix_[0] != '\0') return;  // Manual override from NVS

    // Topic root — з project.json (gen::MQTT_TOPIC_ROOT), не бренд-літерал.
    if (tenant_[0] != '\0') {
        snprintf(prefix_, sizeof(prefix_), "%s/v1/%s/%s",
                 gen::MQTT_TOPIC_ROOT, tenant_, device_id_);
    } else {
        snprintf(prefix_, sizeof(prefix_), "%s/v1/pending/%s",
                 gen::MQTT_TOPIC_ROOT, device_id_);
    }
}

// ── Lifecycle ───────────────────────────────────────────────────

bool MqttService::on_init() {
    ESP_LOGI(TAG, "Initializing MQTT service...");

    load_config();
    build_default_prefix();

    state_set("mqtt.connected", false);
    state_set("mqtt.broker", broker_[0] ? broker_ : "");

    if (!enabled_) {
        state_set("mqtt.status", "disabled");
        ESP_LOGI(TAG, "MQTT disabled (not configured)");
        return true;
    }

    if (broker_[0] == '\0') {
        state_set("mqtt.status", "no_broker");
        ESP_LOGW(TAG, "MQTT enabled but no broker configured");
        return true;
    }

    // Deferred start: не підключаємось одразу — чекаємо WiFi в on_update()
    state_set("mqtt.status", "waiting_wifi");
    ESP_LOGI(TAG, "MQTT ready, waiting for WiFi...");
    return true;
}

void MqttService::on_update(uint32_t dt_ms) {
    // Deferred reconnect (запит від httpd task — виконуємо в main loop)
    if (reconnect_requested_) {
        // player-fork: the previous client is still stopping — keep the request for a later turn.
        if (stopping_.load()) return;
        reconnect_requested_ = false;
        ESP_LOGI(TAG, "Executing deferred reconnect...");
        // player-fork: the old client is stopped off the main loop; the deferred start below
        // brings up the new one once it is gone. Inline only if the stopper could not start.
        const bool async = client_ && stop_client_async();
        if (!async) stop_client();
        if (enabled_ && broker_[0] != '\0') {
            state_set("mqtt.status", "connecting");
            if (!async) start_client();
        } else if (!enabled_) {
            state_set("mqtt.status", "disabled");
        }
        return;
    }

    // player-fork: no client while the old one stops, and a new one waits until it is gone
    // (two clients with one client_id would have the broker drop one of them).
    if (stopping_.load()) return;

    // Deferred start: чекаємо WiFi перед першим підключенням
    if (!client_ && enabled_ && broker_[0] != '\0' && state_) {
        auto wifi_val = state_->get("wifi.connected");
        if (wifi_val.has_value()
            && etl::holds_alternative<bool>(wifi_val.value())
            && etl::get<bool>(wifi_val.value())) {
            ESP_LOGI(TAG, "WiFi connected, starting MQTT client...");
            state_set("mqtt.status", "connecting");
            start_client();
        }
        return;
    }

    // Reconnect з exponential backoff (client створено, але не з'єднано)
    if (client_ && !connected_ && enabled_) {
        reconnect_timer_ms_ += dt_ms;
        if (reconnect_timer_ms_ >= reconnect_delay_ms_) {
            reconnect_timer_ms_ = 0;
            ESP_LOGI(TAG, "MQTT reconnect (backoff %lums)",
                     (unsigned long)reconnect_delay_ms_);
            esp_mqtt_client_reconnect(client_);
            // Exponential backoff
            reconnect_delay_ms_ = std::min(reconnect_delay_ms_ * 2,
                                            MQTT_MAX_RECONNECT_MS);
        }
        return;
    }

    if (!enabled_ || !connected_) return;

    // Heartbeat (metadata, every 30s)
    heartbeat_timer_ms_ += dt_ms;
    if (heartbeat_timer_ms_ >= HEARTBEAT_INTERVAL_MS) {
        heartbeat_timer_ms_ = 0;
        publish_heartbeat();
    }

    // HA Auto-Discovery після підключення (виконуємо в main loop task)
    if (ha_discovery_pending_) {
        ha_discovery_pending_ = false;
        publish_ha_discovery();
    }

    publish_timer_ += dt_ms;
    if (publish_timer_ < PUBLISH_INTERVAL_MS) return;
    publish_timer_ = 0;

    // Публікуємо тільки якщо state змінився
    if (state_ && state_->version() != last_version_) {
        publish_state();
    }

    // One-shot: publish writable params on cloud request
    if (params_publish_requested_) {
        params_publish_requested_ = false;
        publish_params();
    }

}

void MqttService::on_stop() {
    stop_client();
}

// ── MQTT client ─────────────────────────────────────────────────

bool MqttService::start_client() {
    if (client_) {
        stop_client();
    }

    // Формуємо URI: broker вже містить "mqtt://host" або просто "host"
    // Порт 8883 = стандарт MQTT over TLS → автоматично mqtts://
    char uri[160];
    if (strncmp(broker_, "mqtt://", 7) == 0 ||
        strncmp(broker_, "mqtts://", 8) == 0) {
        snprintf(uri, sizeof(uri), "%s:%d", broker_, port_);
    } else if (port_ == 8883) {
        snprintf(uri, sizeof(uri), "mqtts://%s:%d", broker_, port_);
    } else {
        snprintf(uri, sizeof(uri), "mqtt://%s:%d", broker_, port_);
    }

    esp_mqtt_client_config_t mqtt_cfg = {};
    mqtt_cfg.broker.address.uri = uri;
    mqtt_cfg.credentials.username = user_[0] ? user_ : nullptr;
    mqtt_cfg.credentials.authentication.password = pass_[0] ? pass_ : nullptr;

    // TLS for mqtts://: prefer a pinned broker CA (NVS) when set, else the
    // built-in public CA bundle. Plaintext mqtt:// is still allowed but flagged.
    secure_ = (strncmp(uri, "mqtts://", 8) == 0);
    if (secure_) {
        if (broker_ca_[0] != '\0') {
            mqtt_cfg.broker.verification.certificate = broker_ca_;
            ESP_LOGI(TAG, "TLS enabled (pinned broker CA)");
        } else {
            mqtt_cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
            ESP_LOGI(TAG, "TLS enabled (built-in CA bundle)");
        }
    } else {
        ESP_LOGW(TAG, "MQTT over plaintext (mqtt://) — credentials and telemetry "
                      "are UNENCRYPTED on the wire. Use port 8883 / mqtts:// for TLS.");
    }

    // LWT (Last Will and Testament) — "offline" при розриві
    char lwt_topic[128];
    snprintf(lwt_topic, sizeof(lwt_topic), "%s/status", prefix_);
    mqtt_cfg.session.last_will.topic = lwt_topic;
    mqtt_cfg.session.last_will.msg = "offline";
    mqtt_cfg.session.last_will.msg_len = 7;
    mqtt_cfg.session.last_will.qos = 1;
    mqtt_cfg.session.last_will.retain = 1;

    mqtt_cfg.network.disable_auto_reconnect = true;
    // player-fork: the client's lock is held for a whole connect (TCP/TLS, then CONNACK) and
    // for every synchronous QoS0 write the main loop makes; the 10 s default per step outlives
    // the 5 s main-loop watchdog. reconnect_timeout_ms only sizes the WAIT_RECONNECT poll a
    // stop waits out — the backoff in on_update() still decides when to reconnect.
    mqtt_cfg.network.timeout_ms = 3000;
    mqtt_cfg.network.reconnect_timeout_ms = 2000;

    // Heap optimization: зменшити task stack (default 6144)
    // Буфери 1024B — достатньо для OTA JSON payload (~300B) з запасом
    mqtt_cfg.task.stack_size = 4096;
    mqtt_cfg.buffer.size     = 1024;     // inbound
    mqtt_cfg.buffer.out_size = 1024;     // outbound

    client_ = esp_mqtt_client_init(&mqtt_cfg);
    if (!client_) {
        ESP_LOGE(TAG, "Failed to init MQTT client");
        state_set("mqtt.status", "error");
        return false;
    }

    esp_mqtt_client_register_event(client_, MQTT_EVENT_ANY,
                                    mqtt_event_handler, this);

    esp_err_t err = esp_mqtt_client_start(client_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client: %s", esp_err_to_name(err));
        esp_mqtt_client_destroy(client_);
        client_ = nullptr;
        state_set("mqtt.status", "error");
        return false;
    }

    ESP_LOGI(TAG, "MQTT client started → %s (prefix=%s)", uri, prefix_);
    return true;
}

void MqttService::stop_client() {
    if (client_) {
        esp_mqtt_client_stop(client_);
        esp_mqtt_client_destroy(client_);
        client_ = nullptr;
    }
    connected_ = false;
    state_set("mqtt.connected", false);
}

// player-fork: esp_mqtt_client_stop() takes the client's API lock, which the MQTT task holds
// across a whole connect attempt (network.timeout_ms for the TCP/TLS connect and again for
// CONNACK), and then waits for the task to leave its WAIT_RECONNECT nap. From the main loop
// that wait met the 5 s task watchdog when a broker that does not answer was corrected or
// switched off. So the old client is detached here and stopped on a short-lived task.
// Returns false (client_ untouched) if that task could not be created.
bool MqttService::stop_client_async() {
    esp_mqtt_client_handle_t old = client_;
    client_ = nullptr;
    stopping_client_ = old;
    stopping_.store(true);
    if (xTaskCreate(stop_task, "mqtt_stop", 4096, this, 2, nullptr) != pdPASS) {
        ESP_LOGW(TAG, "mqtt_stop task not created, stopping inline");
        stopping_client_ = nullptr;
        stopping_.store(false);
        client_ = old;
        return false;
    }
    connected_ = false;
    state_set("mqtt.connected", false);
    return true;
}

void MqttService::stop_task(void* arg) {
    auto* self = static_cast<MqttService*>(arg);
    esp_mqtt_client_handle_t h = self->stopping_client_;
    esp_mqtt_client_stop(h);
    esp_mqtt_client_destroy(h);
    self->stopping_client_ = nullptr;
    // A connect that completed while the stop waited for the lock dispatched CONNECTED on its
    // way out; no event can follow destroy, so what it set is undone here, before a new start.
    self->connected_ = false;
    self->state_set("mqtt.connected", false);
    if (!self->enabled_) self->state_set("mqtt.status", "disabled");
    ESP_LOGI(TAG, "Previous client stopped");
    self->stopping_.store(false);
    vTaskDelete(nullptr);
}

void MqttService::reconnect() {
    // Тільки ставимо прапорець — реальний reconnect відбудеться в on_update()
    // (main loop task), щоб уникнути race condition з httpd task
    ESP_LOGI(TAG, "Reconnect requested (deferred to main loop)");
    reconnect_requested_ = true;
}

// ── MQTT event handler (static callback) ────────────────────────

void MqttService::mqtt_event_handler(void* args, esp_event_base_t base,
                                      int32_t event_id, void* event_data) {
    auto* self = static_cast<MqttService*>(args);
    auto* event = static_cast<esp_mqtt_event_handle_t>(event_data);

    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED: {
            self->connected_ = true;
            self->reconnect_timer_ms_ = 0;
            self->reconnect_delay_ms_ = MQTT_INITIAL_RECONNECT_MS;
            self->last_version_ = 0;  // Форсуємо повну публікацію
            self->state_set("mqtt.connected", true);
            // "insecure" signals a working but UNENCRYPTED (plaintext) link so
            // the operator can see it; a TLS link reports "connected".
            self->state_set("mqtt.status", self->secure_ ? "connected" : "insecure");
            ESP_LOGI(TAG, "Connected to broker (%s, heap=%lu)",
                     self->secure_ ? "TLS" : "PLAINTEXT",
                     esp_get_free_heap_size());

            // Publish "online" status (retain)
            char topic[128];
            snprintf(topic, sizeof(topic), "%s/status", self->prefix_);
            esp_mqtt_client_publish(self->client_, topic, "online", 6, 1, 1);

            // Subscribe to ALL command topics with wildcard
            // Previously subscribed to 60+ individual keys, which could overflow
            // the ESP-MQTT outbox causing silent subscription failures.
            // Wildcard is safe: handle_incoming() validates keys against STATE_META.
            {
                char cmd_wildcard[96];
                snprintf(cmd_wildcard, sizeof(cmd_wildcard), "%s/cmd/+",
                         self->prefix_);
                int msg_id = esp_mqtt_client_subscribe(self->client_, cmd_wildcard, 0);
                if (msg_id < 0) {
                    ESP_LOGE(TAG, "Failed to subscribe to %s", cmd_wildcard);
                } else {
                    ESP_LOGI(TAG, "Subscribed to %s (covers %d keys + _set_tenant)",
                             cmd_wildcard, (int)gen::MQTT_SUBSCRIBE_COUNT);
                }
            }

            self->heartbeat_timer_ms_ = 0;

            // HA Auto-Discovery — відкладаємо до on_update() (більший стек)
            self->ha_discovery_pending_ = true;
            break;
        }

        case MQTT_EVENT_DISCONNECTED:
            self->connected_ = false;
            self->reconnect_timer_ms_ = 0;  // Перший reconnect через reconnect_delay_ms_
            self->state_set("mqtt.connected", false);
            self->state_set("mqtt.status", "disconnected");
            ESP_LOGW(TAG, "Disconnected from broker");
            break;

        case MQTT_EVENT_DATA:
            // A command is an act, not a state. A retained one is replayed by the broker at every
            // connect: after each power-on, or a Wi-Fi drop, it would act again — a retained
            // volume past the player's "no louder than N % after power-on" among them.
            // So it is not taken.
            if (event->retain) {
                ESP_LOGW(TAG, "retained command ignored: %.*s", event->topic_len, event->topic);
                break;
            }
            self->handle_incoming(event->topic, event->topic_len,
                                   event->data, event->data_len);
            break;

        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT error (type=%d)", event->error_handle->error_type);
            self->state_set("mqtt.status", "error");
            break;

        default:
            break;
    }
}

// ── Publish state ───────────────────────────────────────────────

int MqttService::format_value(const StateValue& val, char* buf, size_t buf_size) {
    if (etl::holds_alternative<float>(val)) {
        return snprintf(buf, buf_size, "%.2f",
                        static_cast<double>(etl::get<float>(val)));
    } else if (etl::holds_alternative<int32_t>(val)) {
        return snprintf(buf, buf_size, "%ld",
                        (long)etl::get<int32_t>(val));
    } else if (etl::holds_alternative<bool>(val)) {
        return snprintf(buf, buf_size, "%s",
                        etl::get<bool>(val) ? "true" : "false");
    } else if (etl::holds_alternative<StringValue>(val)) {
        return snprintf(buf, buf_size, "%s",
                        etl::get<StringValue>(val).c_str());
    }
    return 0;
}

void MqttService::publish_state() {
    if (!client_ || !state_) return;

    // Системні ключі (OTA статус) — публікуються поряд зі згенерованими
    static constexpr const char* SYS_KEYS[] = {
        "_ota.status", "_ota.progress", "_ota.error"
    };
    static constexpr size_t SYS_KEYS_COUNT = sizeof(SYS_KEYS) / sizeof(SYS_KEYS[0]);

    // 1. Згенеровані module keys (з mqtt-секцій маніфестів)
    for (size_t i = 0; i < gen::MQTT_PUBLISH_COUNT && i < MAX_PUBLISH_KEYS; i++) {
        auto val = state_->get(gen::MQTT_PUBLISH[i]);
        if (!val.has_value()) continue;

        char payload[32];
        int len = format_value(val.value(), payload, sizeof(payload));
        if (len <= 0) continue;

        // Delta-publish: порівнюємо з попереднім значенням
        if (strncmp(payload, last_payloads_[i], sizeof(last_payloads_[i])) == 0) {
            continue;  // Значення не змінилось — не публікуємо
        }

        char topic[128];
        snprintf(topic, sizeof(topic), "%s/state/%s",
                 prefix_, gen::MQTT_PUBLISH[i]);

        // Manifest-driven надійність: mqtt.alarm-ключі → QoS1 + retain
        // (аларм не має губитися і мусить пережити reconnect підписника);
        // решта — QoS0 без retain.
        const bool alarm = gen::MQTT_PUBLISH_ALARM[i];
        int msg_id = esp_mqtt_client_publish(client_, topic, payload, len,
                                             alarm ? 1 : 0, alarm ? 1 : 0);
        if (alarm && msg_id < 0) {
            // Не вдалося поставити в outbox (heap/зупинка клієнта) — НЕ кешуємо,
            // щоб наступний тик повторив публікацію (контракт "не губиться").
            ESP_LOGW(TAG, "alarm publish enqueue failed: %s", gen::MQTT_PUBLISH[i]);
            continue;
        }

        // Зберігаємо опубліковане значення в кеш
        strncpy(last_payloads_[i], payload, sizeof(last_payloads_[i]) - 1);
        last_payloads_[i][sizeof(last_payloads_[i]) - 1] = '\0';
    }

    // 2. System keys (_ota.status, _ota.progress, _ota.error)
    for (size_t j = 0; j < SYS_KEYS_COUNT; j++) {
        size_t cache_idx = gen::MQTT_PUBLISH_COUNT + j;
        if (cache_idx >= MAX_PUBLISH_KEYS) break;

        auto val = state_->get(SYS_KEYS[j]);
        if (!val.has_value()) continue;

        char payload[32];
        int len = format_value(val.value(), payload, sizeof(payload));
        if (len <= 0) {
            // Пустий рядок — публікуємо як "" (1 байт = порожній MQTT payload)
            payload[0] = '\0';
            len = 0;
        }

        if (len > 0 && strncmp(payload, last_payloads_[cache_idx], sizeof(last_payloads_[cache_idx])) == 0) {
            continue;
        }
        // Для len == 0 (пустий рядок): перевіряємо чи кеш вже порожній
        if (len == 0 && last_payloads_[cache_idx][0] == '\0') {
            continue;
        }

        char topic[128];
        snprintf(topic, sizeof(topic), "%s/state/%s", prefix_, SYS_KEYS[j]);
        esp_mqtt_client_publish(client_, topic, payload, len, 0, 0);
        ESP_LOGD(TAG, "SYS publish: %s = '%s'", SYS_KEYS[j], payload);

        strncpy(last_payloads_[cache_idx], payload, sizeof(last_payloads_[cache_idx]) - 1);
        last_payloads_[cache_idx][sizeof(last_payloads_[cache_idx]) - 1] = '\0';
    }

    last_version_ = state_->version();
}

void MqttService::publish_params() {
    if (!connected_ || !client_ || !state_) return;

    int count = 0;
    for (size_t i = 0; i < gen::MQTT_SUBSCRIBE_COUNT; i++) {
        auto val = state_->get(gen::MQTT_SUBSCRIBE[i]);
        if (!val.has_value()) continue;

        char payload[32];
        int len = format_value(val.value(), payload, sizeof(payload));
        if (len <= 0) continue;

        char topic[128];
        snprintf(topic, sizeof(topic), "%s/state/%s",
                 prefix_, gen::MQTT_SUBSCRIBE[i]);
        esp_mqtt_client_publish(client_, topic, payload, len, 0, 0);
        count++;
    }

    ESP_LOGI(TAG, "Published %d writable params (one-shot)", count);
}

// ── Heartbeat (metadata) ────────────────────────────────────────

void MqttService::publish_heartbeat() {
    if (!connected_ || !client_) return;

    char topic[96];
    snprintf(topic, sizeof(topic), "%s/heartbeat", prefix_);

    // player-fork: no esp_wifi_sta_get_ap_info() here. Over esp_hosted it is a synchronous RPC
    // to the C6 with a 5 s timeout, made from the task that feeds the 5 s watchdog, and it
    // reads rssi=0 on this stack anyway — see the 17.09.2026 note in wifi_service.cpp, where
    // the same call reset the board. The field stays for the payload's consumers.
    const int rssi = 0;

    // Get firmware version
    const esp_app_desc_t* app_desc = esp_app_get_description();

    char buf[128];  // Stack buffer, no heap allocation
    int len = snprintf(buf, sizeof(buf),
        "{\"proto\":1,\"fw\":\"%s\",\"up\":%lu,\"heap\":%lu,\"rssi\":%d}",
        app_desc->version,
        (unsigned long)(esp_timer_get_time() / 1000000ULL),
        (unsigned long)esp_get_free_heap_size(),
        rssi);

    esp_mqtt_client_publish(client_, topic, buf, len, 0, 0);
}

// ── Handle incoming commands ────────────────────────────────────

void MqttService::handle_incoming(const char* topic, int topic_len,
                                    const char* data, int data_len) {
    // Вхідний topic: "{prefix}/cmd/{key}"
    // Шукаємо "/cmd/" в topic
    char topic_buf[128];
    int copy_len = (topic_len < (int)sizeof(topic_buf) - 1)
                   ? topic_len : (int)sizeof(topic_buf) - 1;
    memcpy(topic_buf, topic, copy_len);
    topic_buf[copy_len] = '\0';

    // Знайти "/cmd/" в topic
    const char* cmd_marker = strstr(topic_buf, "/cmd/");
    if (!cmd_marker) {
        ESP_LOGW(TAG, "Unexpected topic: %s", topic_buf);
        return;
    }

    const char* key = cmd_marker + 5;  // skip "/cmd/"

    // Special command: _ota (JSON payload — не поміщається в val_buf[32])
    // Обробляємо ДО копіювання в val_buf, бо payload може бути 200+ байт
    if (strcmp(key, "_ota") == 0) {
        // Парсимо JSON з data/data_len напряму (на стеку ~640 байт)
        char json_buf[512];
        int json_len = (data_len < (int)sizeof(json_buf) - 1)
                       ? data_len : (int)sizeof(json_buf) - 1;
        memcpy(json_buf, data, json_len);
        json_buf[json_len] = '\0';

        jsmn_parser jp;
        jsmntok_t tokens[16];
        jsmn_init(&jp);
        int tok_count = jsmn_parse(&jp, json_buf, json_len, tokens, 16);
        if (tok_count < 1 || tokens[0].type != JSMN_OBJECT) {
            ESP_LOGE(TAG, "_ota: invalid JSON payload");
            return;
        }

        ota_handler::OtaParams ota_params = {};

        for (int i = 1; i < tok_count - 1; i += 2) {
            if (tokens[i].type != JSMN_STRING) continue;
            json_buf[tokens[i].end] = '\0';
            json_buf[tokens[i + 1].end] = '\0';
            const char* k = json_buf + tokens[i].start;
            const char* v = json_buf + tokens[i + 1].start;
            // player-fork: unescaped — "\/" is valid JSON for "/", and a sender that writes it
            // so would hand the OTA client a URL with backslashes in it.
            if (tokens[i + 1].type == JSMN_STRING &&
                !net::json_unescape(json_buf + tokens[i + 1].start)) {
                ESP_LOGE(TAG, "_ota: malformed escape in '%s'", k);
                return;
            }

            if (strcmp(k, "url") == 0) {
                strncpy(ota_params.url, v, sizeof(ota_params.url) - 1);
            } else if (strcmp(k, "version") == 0) {
                strncpy(ota_params.version, v, sizeof(ota_params.version) - 1);
            } else if (strcmp(k, "checksum") == 0) {
                strncpy(ota_params.checksum, v, sizeof(ota_params.checksum) - 1);
            }
        }

        if (ota_params.url[0] == '\0') {
            ESP_LOGE(TAG, "_ota: missing 'url' in payload");
            return;
        }

        ESP_LOGI(TAG, "Cloud OTA command: version=%s", ota_params.version);

        if (!ota_handler::start_ota(ota_params, state_)) {
            ESP_LOGW(TAG, "_ota: already in progress or failed to start");
        }
        return;
    }

    // Копіюємо payload в null-terminated буфер (потрібно для _set_tenant та звичайних команд)
    char val_buf[32];
    int val_len = (data_len < (int)sizeof(val_buf) - 1)
                  ? data_len : (int)sizeof(val_buf) - 1;
    memcpy(val_buf, data, val_len);
    val_buf[val_len] = '\0';

    // Trim trailing whitespace/newlines (MQTT clients may add \n)
    while (val_len > 0 && (val_buf[val_len - 1] == '\n' ||
                            val_buf[val_len - 1] == '\r' ||
                            val_buf[val_len - 1] == ' ')) {
        val_buf[--val_len] = '\0';
    }

    // Special command: _request_full_state — cloud asks for ALL keys
    if (strcmp(key, "_request_full_state") == 0) {
        ESP_LOGI(TAG, "Full state requested by cloud — clearing publish cache");
        memset(last_payloads_, 0, sizeof(last_payloads_));
        last_version_ = 0;              // Force re-publish of 48 read-only keys
        params_publish_requested_ = true; // Also publish 60 writable params
        return;
    }

    // Special command: _set_mqtt_creds (JSON: {"user":"...","pass":"..."})
    // Cloud sends this BEFORE _set_tenant — save creds but don't reconnect yet.
    // The subsequent _set_tenant triggers reconnect with new credentials.
    // If _set_tenant doesn't arrive within 10s, deferred reconnect kicks in.
    if (strcmp(key, "_set_mqtt_creds") == 0) {
        // Parse JSON from raw data (may be longer than val_buf[32])
        char json_buf[256];
        int json_len = (data_len < (int)sizeof(json_buf) - 1)
                       ? data_len : (int)sizeof(json_buf) - 1;
        memcpy(json_buf, data, json_len);
        json_buf[json_len] = '\0';

        jsmn_parser jp;
        jsmntok_t tokens[8];
        jsmn_init(&jp);
        int tok_count = jsmn_parse(&jp, json_buf, json_len, tokens, 8);
        if (tok_count < 1 || tokens[0].type != JSMN_OBJECT) {
            ESP_LOGE(TAG, "_set_mqtt_creds: invalid JSON");
            return;
        }

        char new_user[64] = {};
        char new_pass[64] = {};

        for (int i = 1; i < tok_count - 1; i += 2) {
            if (tokens[i].type != JSMN_STRING) continue;
            json_buf[tokens[i].end] = '\0';
            json_buf[tokens[i + 1].end] = '\0';
            const char* k = json_buf + tokens[i].start;
            const char* v = json_buf + tokens[i + 1].start;
            // player-fork: unescaped, as /api/mqtt does — a credential holding " or \ would
            // otherwise be stored with the JSON's backslashes in it. A malformed escape drops the
            // command: half-converted, the value is neither what was sent nor what it meant.
            if (tokens[i + 1].type == JSMN_STRING &&
                !net::json_unescape(json_buf + tokens[i + 1].start)) {
                ESP_LOGE(TAG, "_set_mqtt_creds: malformed escape in '%s'", k);
                return;
            }

            if (strcmp(k, "user") == 0 || strcmp(k, "username") == 0) {
                strncpy(new_user, v, sizeof(new_user) - 1);
            } else if (strcmp(k, "pass") == 0 || strcmp(k, "password") == 0) {
                strncpy(new_pass, v, sizeof(new_pass) - 1);
            }
        }

        if (new_user[0] == '\0' || new_pass[0] == '\0') {
            ESP_LOGE(TAG, "_set_mqtt_creds: missing user or pass");
            return;
        }

        // Save to NVS (same keys as load_config reads)
        nvs_helper::write_str("mqtt", "user", new_user);
        nvs_helper::write_str("mqtt", "pass", new_pass);
        // Update in-memory
        strncpy(user_, new_user, sizeof(user_) - 1);
        user_[sizeof(user_) - 1] = '\0';
        strncpy(pass_, new_pass, sizeof(pass_) - 1);
        pass_[sizeof(pass_) - 1] = '\0';

        ESP_LOGI(TAG, "MQTT credentials updated (user=%s), waiting for _set_tenant to reconnect", new_user);
        return;
    }

    // Special command: _set_tenant (not in STATE_META)
    if (strcmp(key, "_set_tenant") == 0) {
        strncpy(tenant_, val_buf, sizeof(tenant_) - 1);
        tenant_[sizeof(tenant_) - 1] = '\0';
        nvs_helper::write_str("mqtt", "tenant", tenant_);
        prefix_[0] = '\0';  // Force prefix rebuild
        nvs_helper::write_str("mqtt", "prefix", "");  // Clear saved prefix so reboot uses tenant
        build_default_prefix();
        ESP_LOGI(TAG, "Tenant set to '%s', reconnecting with prefix '%s'",
                 tenant_, prefix_);
        reconnect();
        return;
    }

    // Валідація: ключ має бути в STATE_META як writable
    const auto* meta = gen::find_state_meta(key);
    if (!meta) {
        ESP_LOGW(TAG, "Unknown key in MQTT cmd: %s", key);
        return;
    }
    if (!meta->writable) {
        ESP_LOGW(TAG, "Key not writable: %s", key);
        return;
    }

    ESP_LOGI(TAG, "CMD: %s = %s", key, val_buf);

    // Парсимо значення за типом з meta
    if (strcmp(meta->type, "float") == 0) {
        char* end = nullptr;
        float f = strtof(val_buf, &end);
        if (end == val_buf) {
            ESP_LOGW(TAG, "Invalid float: %s", val_buf);
            return;
        }
        // Валідація min/max. A clamp is never silent: the range comes from the manifest, and a
        // request outside it is either a client bug or a manifest that lags the code (an
        // audio.source of 4 landed on 3 for a while because the options stopped at Bluetooth).
        const float f_req = f;
        if (f < meta->min_val) f = meta->min_val;
        if (f > meta->max_val) f = meta->max_val;
        if (f != f_req) {
            ESP_LOGW(TAG, "CMD %s: %g is outside [%g..%g], clamped to %g",
                     key, (double)f_req, (double)meta->min_val, (double)meta->max_val, (double)f);
        }
        state_set(key, f);
    } else if (strcmp(meta->type, "int") == 0) {
        char* end = nullptr;
        long v = strtol(val_buf, &end, 10);
        if (end == val_buf) {
            ESP_LOGW(TAG, "Invalid int: %s", val_buf);
            return;
        }
        const long v_req = v;
        if (v < static_cast<long>(meta->min_val)) v = static_cast<long>(meta->min_val);
        if (v > static_cast<long>(meta->max_val)) v = static_cast<long>(meta->max_val);
        if (v != v_req) {
            ESP_LOGW(TAG, "CMD %s: %ld is outside [%ld..%ld], clamped to %ld",
                     key, v_req, (long)meta->min_val, (long)meta->max_val, v);
        }
        // A command key is an event for the module that owns it, not a value in the
        // store: it goes to that module's queue after the same range check above. See
        // STATE_COMMANDS in the generated state_meta.h.
        if (const auto* cmd = gen::find_state_command(key)) {
            if (!post_command_to(cmd->module, key, static_cast<int32_t>(v)))
                ESP_LOGW(TAG, "CMD %s: %s did not take it", key, cmd->module);
        } else {
            state_set(key, static_cast<int32_t>(v));
        }
    } else if (strcmp(meta->type, "bool") == 0) {
        bool b = (strcmp(val_buf, "true") == 0 || strcmp(val_buf, "1") == 0);
        state_set(key, b);
    }
}

// ── HTTP API ────────────────────────────────────────────────────

void MqttService::set_cors_headers(httpd_req_t* req) {
    // No wildcard Access-Control-Allow-Origin (AUDIT-039) — broker/user/tenant
    // config must not be readable cross-origin by a malicious external page.
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET,POST,OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type, Authorization");
}

esp_err_t MqttService::handle_options(httpd_req_t* req) {
    set_cors_headers(req);
    httpd_resp_send(req, nullptr, 0);
    return ESP_OK;
}

void MqttService::set_http_server(httpd_handle_t server) {
    if (server_ == server && http_registered_) return;
    server_ = server;
    if (server_) {
        register_http_handlers();
    }
}

void MqttService::register_http_handlers() {
    if (!server_ || http_registered_) return;

    httpd_uri_t get_mqtt = {};
    get_mqtt.uri      = "/api/mqtt";
    get_mqtt.method   = HTTP_GET;
    get_mqtt.handler  = handle_get_mqtt;
    get_mqtt.user_ctx = this;
    httpd_register_uri_handler(server_, &get_mqtt);

    httpd_uri_t post_mqtt = {};
    post_mqtt.uri      = "/api/mqtt";
    post_mqtt.method   = HTTP_POST;
    post_mqtt.handler  = handle_post_mqtt;
    post_mqtt.user_ctx = this;
    httpd_register_uri_handler(server_, &post_mqtt);

    // OPTIONS для CORS preflight
    httpd_uri_t options = {};
    options.uri      = "/api/mqtt";
    options.method   = HTTP_OPTIONS;
    options.handler  = handle_options;
    options.user_ctx = this;
    httpd_register_uri_handler(server_, &options);

    http_registered_ = true;
    ESP_LOGI(TAG, "HTTP handlers registered (GET/POST /api/mqtt)");
}

esp_err_t MqttService::handle_get_mqtt(httpd_req_t* req) {
    if (!HttpService::check_auth(req)) return ESP_OK;
    auto* self = static_cast<MqttService*>(req->user_ctx);

    // Визначаємо status рядок
    const char* status = "disabled";
    if (self->enabled_) {
        if (self->connected_) status = self->secure_ ? "connected" : "insecure";
        else if (self->client_) status = "connecting";
        else if (self->broker_[0] == '\0') status = "no_broker";
        else status = "disconnected";
    }

    // player-fork: the text fields escaped, as POST unescapes them on the way in — a user such
    // as DOMAIN\user or a prefix holding a quote came back as broken JSON, the page was left with
    // an empty form, and a Save from it would have wiped the settings. At their worst (six bytes
    // for one) the fields and the reply take ~4 KB: the heap's, not the server's 8 KB stack.
    struct Text {
        char broker[net::json_escaped_capacity(sizeof(MqttService::broker_) - 1)];
        char user[net::json_escaped_capacity(sizeof(MqttService::user_) - 1)];
        char prefix[net::json_escaped_capacity(sizeof(MqttService::prefix_) - 1)];
        char tenant[net::json_escaped_capacity(sizeof(MqttService::tenant_) - 1)];
        char json[sizeof(broker) + sizeof(user) + sizeof(prefix) + sizeof(tenant) + 256];
    };
    std::unique_ptr<Text> t(new (std::nothrow) Text);
    if (!t || !net::json_escape(t->broker, sizeof(t->broker), self->broker_) ||
        !net::json_escape(t->user, sizeof(t->user), self->user_) ||
        !net::json_escape(t->prefix, sizeof(t->prefix), self->prefix_) ||
        !net::json_escape(t->tenant, sizeof(t->tenant), self->tenant_)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    int len = snprintf(t->json, sizeof(t->json),
        "{\"enabled\":%s,"
        "\"connected\":%s,"
        "\"broker\":\"%s\","
        "\"port\":%d,"
        "\"user\":\"%s\","
        "\"prefix\":\"%s\","
        "\"tenant\":\"%s\","
        "\"device_id\":\"%s\","
        "\"status\":\"%s\"}",
        self->enabled_ ? "true" : "false",
        self->connected_ ? "true" : "false",
        t->broker,
        self->port_,
        t->user,
        t->prefix,
        t->tenant,
        self->device_id_,
        status);

    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, t->json, len);
    return ESP_OK;
}

esp_err_t MqttService::handle_post_mqtt(httpd_req_t* req) {
    if (!HttpService::check_auth(req)) return ESP_OK;
    auto* self = static_cast<MqttService*>(req->user_ctx);

    char buf[384];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    buf[len] = '\0';

    // Parse JSON з jsmn
    jsmn_parser parser;
    jsmntok_t tokens[20];
    jsmn_init(&parser);
    int r = jsmn_parse(&parser, buf, len, tokens, 20);
    if (r < 1 || tokens[0].type != JSMN_OBJECT) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    // Зчитуємо поточні значення як fallback
    char new_broker[128];
    strncpy(new_broker, self->broker_, sizeof(new_broker));
    uint16_t new_port = self->port_;
    char new_user[64];
    strncpy(new_user, self->user_, sizeof(new_user));
    char new_pass[64];
    strncpy(new_pass, self->pass_, sizeof(new_pass));
    char new_prefix[80];
    strncpy(new_prefix, self->prefix_, sizeof(new_prefix));
    char new_tenant[36];
    strncpy(new_tenant, self->tenant_, sizeof(new_tenant));
    bool new_enabled = self->enabled_;
    bool tenant_changed = false;
    bool prefix_explicit = false;

    for (int i = 1; i < r - 1; i += 2) {
        if (tokens[i].type != JSMN_STRING) continue;

        buf[tokens[i].end] = '\0';
        buf[tokens[i + 1].end] = '\0';

        const char* key = buf + tokens[i].start;
        const char* val = buf + tokens[i + 1].start;

        // Приймаємо як "broker" так і "mqtt.broker" (WebUI toggle/save)
        const char* k = key;
        if (strncmp(k, "mqtt.", 5) == 0) k += 5;  // Відрізаємо префікс

        // player-fork: as in /api/wifi and /api/auth — jsmn leaves a string's JSON escapes in
        // it, so a password holding " or \ was stored with the backslashes and the broker
        // refused it. The text fields must be JSON strings, unescaped in place.
        if ((strcmp(k, "broker") == 0 || strcmp(k, "user") == 0 || strcmp(k, "password") == 0 ||
             strcmp(k, "prefix") == 0 || strcmp(k, "tenant") == 0) &&
            (tokens[i + 1].type != JSMN_STRING || !net::json_unescape(buf + tokens[i + 1].start))) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                "broker, user, password, prefix and tenant must be JSON strings");
            return ESP_FAIL;
        }

        if (strcmp(k, "broker") == 0) {
            strncpy(new_broker, val, sizeof(new_broker) - 1);
            new_broker[sizeof(new_broker) - 1] = '\0';
        } else if (strcmp(k, "port") == 0) {
            new_port = static_cast<uint16_t>(atoi(val));
        } else if (strcmp(k, "user") == 0) {
            strncpy(new_user, val, sizeof(new_user) - 1);
            new_user[sizeof(new_user) - 1] = '\0';
        } else if (strcmp(k, "password") == 0) {
            // Порожній пароль = не змінювати збережений (WebUI не показує пароль)
            if (strlen(val) > 0) {
                strncpy(new_pass, val, sizeof(new_pass) - 1);
                new_pass[sizeof(new_pass) - 1] = '\0';
            }
        } else if (strcmp(k, "prefix") == 0) {
            strncpy(new_prefix, val, sizeof(new_prefix) - 1);
            new_prefix[sizeof(new_prefix) - 1] = '\0';
            prefix_explicit = true;
        } else if (strcmp(k, "tenant") == 0) {
            strncpy(new_tenant, val, sizeof(new_tenant) - 1);
            new_tenant[sizeof(new_tenant) - 1] = '\0';
            tenant_changed = true;
        } else if (strcmp(k, "enabled") == 0) {
            new_enabled = (val[0] == 't' || val[0] == '1');
        }
    }

    // If tenant changed and no explicit prefix, clear prefix for auto-rebuild
    if (tenant_changed && !prefix_explicit) {
        new_prefix[0] = '\0';
    }

    bool ok = self->save_config(new_broker, new_port, new_user,
                                 new_pass, new_prefix, new_tenant, new_enabled);

    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    if (ok) {
        // Перезапустити клієнт з новими налаштуваннями
        self->reconnect();
        httpd_resp_sendstr(req, "{\"ok\":true,\"msg\":\"Config saved, reconnecting\"}");
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save");
    }
    return ESP_OK;
}

// ── Home Assistant MQTT Auto-Discovery ──────────────────────────

void MqttService::publish_ha_entity(
        const char* state_key, const char* name,
        const char* entity_type, const char* device_class,
        const char* unit, const char* state_class,
        const char* device_id, const char* device_name) {

    // object_id: "player.volume" → "player_volume"
    char object_id[48] = {};
    strncpy(object_id, state_key, sizeof(object_id) - 1);
    for (char* p = object_id; *p; p++) {
        if (*p == '.') *p = '_';
    }

    char disc_topic[96];
    snprintf(disc_topic, sizeof(disc_topic),
             "homeassistant/%s/%s_%s/%s/config",
             entity_type, gen::MQTT_TOPIC_ROOT, device_id, object_id);

    char state_topic[128];
    snprintf(state_topic, sizeof(state_topic), "%s/state/%s", prefix_, state_key);

    char unique_id[64];
    snprintf(unique_id, sizeof(unique_id), "%s_%s_%s",
             gen::MQTT_TOPIC_ROOT, device_id, object_id);

    char payload[512];
    int len = 0;

    len += snprintf(payload + len, sizeof(payload) - len,
        "{\"name\":\"%s\","
        "\"unique_id\":\"%s\","
        "\"state_topic\":\"%s\","
        "\"availability_topic\":\"%s/status\","
        "\"payload_available\":\"online\","
        "\"payload_not_available\":\"offline\"",
        name, unique_id, state_topic, prefix_);

    if (device_class && device_class[0]) {
        len += snprintf(payload + len, sizeof(payload) - len,
            ",\"device_class\":\"%s\"", device_class);
    }
    if (unit && unit[0]) {
        len += snprintf(payload + len, sizeof(payload) - len,
            ",\"unit_of_measurement\":\"%s\"", unit);
    }
    if (state_class && state_class[0]) {
        len += snprintf(payload + len, sizeof(payload) - len,
            ",\"state_class\":\"%s\"", state_class);
    }
    // binary_sensor: HA очікує ON/OFF, але ми публікуємо true/false
    if (strcmp(entity_type, "binary_sensor") == 0) {
        len += snprintf(payload + len, sizeof(payload) - len,
            ",\"payload_on\":\"true\",\"payload_off\":\"false\"");
    }

    len += snprintf(payload + len, sizeof(payload) - len,
        ",\"device\":{"
        "\"identifiers\":[\"modesp_%s\"],"
        "\"name\":\"%s\","
        "\"model\":\"ModESP v4\","
        "\"manufacturer\":\"ModESP\""
        "}}",
        device_id, device_name);

    esp_mqtt_client_publish(client_, disc_topic, payload, len, 1, 1);
    ESP_LOGD(TAG, "HA discovery: %s", disc_topic);
}

void MqttService::publish_ha_discovery() {
    if (!client_ || !connected_) return;

    // device_id: витягуємо з prefix_ "modesp/A1B2C3" → "a1b2c3" (lowercase)
    char device_id[8] = {};
    const char* slash = strrchr(prefix_, '/');
    if (slash && *(slash + 1) != '\0') {
        strncpy(device_id, slash + 1, sizeof(device_id) - 1);
        for (int i = 0; device_id[i]; i++) {
            if (device_id[i] >= 'A' && device_id[i] <= 'F') {
                device_id[i] = device_id[i] - 'A' + 'a';
            }
        }
    }

    char device_name[32];
    snprintf(device_name, sizeof(device_name), "%s %s", gen::MQTT_TOPIC_ROOT, device_id);

    // Entities — ЦІЛКОМ з маніфестів (mqtt.ha секції модулів) через
    // згенерований gen::HA_ENTITIES[]. Жодних продуктових ключів у framework:
    // нова entity з'являється правкою маніфеста, не цього файлу.
    if (gen::HA_ENTITIES_COUNT == 0) {
        ESP_LOGI(TAG, "HA discovery: no entities declared in manifests");
        return;
    }

    ESP_LOGI(TAG, "Publishing HA discovery (%d entities, device=%s)",
             (int)gen::HA_ENTITIES_COUNT, device_id);

    for (size_t i = 0; i < gen::HA_ENTITIES_COUNT; i++) {
        const auto& e = gen::HA_ENTITIES[i];
        publish_ha_entity(e.state_key, e.name, e.component,
                          e.device_class, e.unit, e.state_class,
                          device_id, device_name);
    }

    ESP_LOGI(TAG, "HA discovery complete");
}

// Cloud-бекенд цієї прошивки (Kconfig MODESP_CLOUD_MQTT) — main бере його
// через generic modesp::cloud_backend(), не знаючи класу MqttService.
ICloudService* cloud_backend() {
    static MqttService instance;
    return &instance;
}

} // namespace modesp
