/**
 * @file airplay2.c
 * @brief Bringing upstream's receiver up inside the player, and what it asks of its host.
 *
 * Upstream's main.c starts the same services in the same order (start_airplay_services);
 * that file is not taken, because the rest of it is upstream's board, network and web UI.
 * What the core expects of its host — the settings calls, the station MAC — is answered
 * here from what the module passed to airplay2_start().
 */

#include <string.h>
#include <stdatomic.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"

#include "airplay2.h"
#include "airplay2_port.h"
#include "audio_output.h"
#include "audio_receiver.h"
#include "dacp_client.h"
#include "hap.h"
#include "mdns_airplay.h"
#include "ptp_clock.h"
#include "rtsp_events.h"
#include "rtsp_server.h"
#include "rtsp_lifecycle.h"
#include "settings.h"

static const char *TAG = "airplay2";

static struct {
    atomic_bool         started;
    bool                initialized;
    char                name[65];
    uint8_t             mac[6];
    float               volume_db;
    bool                artwork;
    atomic_bool         legacy_mode;
    airplay2_listener_t listener;
    void*               ctx;
    volatile bool       connected;
    volatile bool       playing;
} s;

static void emit(airplay2_event_t ev, const airplay2_event_data_t* d) {
    if (s.listener) s.listener(ev, d, s.ctx);
}

static void on_rtsp(rtsp_event_t event, const rtsp_event_data_t* data, void* user_data) {
    (void)user_data;
    airplay2_event_data_t d = {.title = "", .artist = "", .album = "", .volume_db = s.volume_db,
                               .image_type = ""};
    switch (event) {
    case RTSP_EVENT_CLIENT_CONNECTED:
        s.connected = true;
        emit(AIRPLAY2_EV_CONNECTED, &d);
        break;
    case RTSP_EVENT_PLAYING:
        s.connected = true;
        s.playing = true;
        emit(AIRPLAY2_EV_PLAYING, &d);
        break;
    case RTSP_EVENT_PAUSED:
        s.playing = false;
        emit(AIRPLAY2_EV_PAUSED, &d);
        break;
    case RTSP_EVENT_DISCONNECTED:
        s.connected = false;
        s.playing = false;
        emit(AIRPLAY2_EV_DISCONNECTED, &d);
        break;
    case RTSP_EVENT_METADATA:
        if (!data) break;
        d.title = data->metadata.title;
        d.artist = data->metadata.artist;
        d.album = data->metadata.album;
        d.duration_s = data->metadata.duration_secs;
        d.position_s = data->metadata.position_secs;
        emit(AIRPLAY2_EV_METADATA, &d);
        break;
    }
}

// ── what upstream's core asks of its host ─────────────────────────────────────────────────

esp_err_t airplay2_read_mac(uint8_t* mac, esp_mac_type_t type) {
    if (type == ESP_MAC_WIFI_STA && mac) {
        memcpy(mac, s.mac, sizeof(s.mac));
        return ESP_OK;
    }
    return esp_read_mac(mac, type);        // this file is not under redirect.h: the real one
}

const uint8_t* airplay2_port_mac(void)  { return s.mac; }
const char*    airplay2_port_name(void) { return s.name; }
bool airplay2_port_legacy(void) { return s.legacy_mode; }
bool airplay2_port_artwork_wanted(void)  { return s.artwork; }

void airplay2_port_artwork(const uint8_t* image, size_t len, const char* type) {
    if (!s.artwork) return;
    airplay2_event_data_t d = {.title = "", .artist = "", .album = "", .volume_db = s.volume_db,
                               .image = image, .image_len = len, .image_type = type ? type : ""};
    emit(AIRPLAY2_EV_ARTWORK, &d);
}

esp_err_t settings_get_volume(float* volume_db) {
    if (!volume_db) return ESP_ERR_INVALID_ARG;
    *volume_db = s.volume_db;
    return ESP_OK;
}

void airplay2_report_volume(float volume_db) {
    s.volume_db = volume_db;               // the room's, not the phone's: no event
}

esp_err_t settings_set_volume(float volume_db) {
    // s.volume_db is what /info announces as initialVolume, and that is the room's level
    // (airplay2_report_volume): the phone's own level is only passed on.
    airplay2_event_data_t d = {.title = "", .artist = "", .album = "", .volume_db = volume_db,
                               .image_type = ""};
    emit(AIRPLAY2_EV_VOLUME, &d);
    return ESP_OK;
}

esp_err_t settings_persist_volume(void) {
    return ESP_OK;                         // audio.volume is persisted by the player
}

esp_err_t settings_get_device_name(char* name, size_t len) {
    if (!name || !len) return ESP_ERR_INVALID_ARG;
    strlcpy(name, s.name, len);
    return ESP_OK;
}

// ── the API ───────────────────────────────────────────────────────────────────────────────

esp_err_t airplay2_start(const airplay2_config_t* cfg) {
    if (s.started) return ESP_OK;
    ESP_RETURN_ON_FALSE(cfg && cfg->name && cfg->name[0], ESP_ERR_INVALID_ARG, TAG, "no name");
    strlcpy(s.name, cfg->name, sizeof(s.name));
    memcpy(s.mac, cfg->mac, sizeof(s.mac));
    s.volume_db = cfg->volume_db;
    s.artwork = cfg->artwork;
    s.legacy_mode = cfg->legacy_mode;
    s.listener = cfg->listener;
    s.ctx = cfg->ctx;

    // The order is upstream's (main.c, start_airplay_services): the clock and the pairing
    // keys before anything that could accept a connection needing them.
    esp_err_t err = ptp_clock_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "PTP clock: %s", esp_err_to_name(err));
        return err;
    }
    ESP_RETURN_ON_ERROR(hap_init(), TAG, "HAP keys");
    ESP_RETURN_ON_ERROR(audio_receiver_init(), TAG, "receiver");
    ESP_RETURN_ON_ERROR(audio_output_init(), TAG, "output");
    dacp_init();                           // upstream does this in playback_control_init()
    ESP_RETURN_ON_FALSE(rtsp_events_register(on_rtsp, NULL) == 0, ESP_ERR_NO_MEM, TAG,
                        "event listener");
    audio_output_start();
    s.initialized = true;
    ESP_RETURN_ON_ERROR(rtsp_server_start(), TAG, "RTSP server");
    s.started = true;
    ESP_LOGI(TAG, "receiver up as \"%s\"", s.name);
    // The module advertises on its main loop, serialized with Wi-Fi mDNS lifecycle.
    return ESP_OK;
}

// Called by the module's single lifecycle worker after mDNS removal. Never change
// the wire profile while an old client can still format a response or clean up audio.
esp_err_t airplay2_set_legacy_mode(bool legacy) {
    if (!s.initialized) return ESP_ERR_INVALID_STATE;
    const bool previous = atomic_load(&s.legacy_mode);
    if (previous == legacy && atomic_load(&s.started)) return ESP_OK;

    esp_err_t err = rtsp_server_stop_checked();
    atomic_store(&s.started, false);
    if (err != ESP_OK) return err;

    atomic_store(&s.legacy_mode, legacy);
    err = rtsp_server_start();
    if (err == ESP_OK) {
        atomic_store(&s.started, true);
        return ESP_OK;
    }

    // A timed-out start may still own tasks. Join them before restoring the old
    // profile; otherwise late replies could use a different profile mid-session.
    const esp_err_t stopped = rtsp_server_stop_checked();
    if (stopped != ESP_OK) return stopped;
    atomic_store(&s.legacy_mode, previous);
    const esp_err_t rollback = rtsp_server_start();
    if (rollback == ESP_OK) {
        atomic_store(&s.started, true);
    } else {
        (void)rtsp_server_stop_checked();
        ESP_LOGE(TAG, "mode switch failed (%s); rollback failed (%s)",
                 esp_err_to_name(err), esp_err_to_name(rollback));
    }
    return err;
}

esp_err_t airplay2_stop_listening(void) {
    if (!s.initialized) return ESP_ERR_INVALID_STATE;
    if (!atomic_load(&s.started)) return ESP_OK;
    const esp_err_t err = rtsp_server_stop_checked();
    atomic_store(&s.started, false);
    return err;
}

bool airplay2_legacy_mode(void) { return atomic_load(&s.legacy_mode); }
bool airplay2_started(void)   { return atomic_load(&s.started); }
bool airplay2_connected(void) { return s.connected; }
bool airplay2_playing(void)   { return s.playing; }

void airplay2_advertise(void) {
    if (s.started) mdns_airplay_init();
}

void airplay2_unadvertise(void) {
    if (s.started) mdns_airplay_remove();
}

bool airplay2_pause_sender(void) {
    if (!s.playing || !dacp_is_active()) return false;
    dacp_send_playpause();                 // a toggle: only ever sent while it is playing
    return true;
}

bool airplay2_remote_ready(void) {
    return atomic_load(&s.started) && dacp_is_active();
}

bool airplay2_remote(airplay2_remote_t cmd) {
    if (!airplay2_remote_ready()) return false;
    switch (cmd) {
    case AIRPLAY2_REMOTE_PLAYPAUSE:
        // Paused, an AirPlay 1 phone has closed its connection and the receiver holds the slot
        // for it while its DACP service is advertised (rtsp_server.c, the grace period).
        // Asking for the resume keeps that wait open while the phone reconnects, which is
        // what upstream's own play button does alongside the playpause.
        if (!s.playing) rtsp_server_request_resume();
        dacp_send_playpause();
        return true;
    case AIRPLAY2_REMOTE_NEXT:
        dacp_send_next();
        return true;
    case AIRPLAY2_REMOTE_PREV:
        dacp_send_prev();
        return true;
    }
    return false;
}
