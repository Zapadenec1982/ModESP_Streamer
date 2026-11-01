/**
 * @file radio_http.cpp
 * @brief Plain-HTTP GET for the catalog and the covers, HTTPS on request. See radio_http.h.
 */

#include "radio_http.h"
#include "radio_art.h"   // split_url

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <cstdio>
#include <cstring>

namespace radio_http {
namespace {

/// Held for the whole of an HTTPS fetch (get_url).
SemaphoreHandle_t tls_lock() {
    static StaticSemaphore_t storage;
    static SemaphoreHandle_t lock = xSemaphoreCreateMutexStatic(&storage);
    return lock;
}

/// Opens, checks the status and reads the whole body. A fresh client every time.
int fetch(esp_http_client_config_t& cfg, const char* label, char* buf, size_t cap,
          char* why, size_t why_sz) {
    cfg.timeout_ms        = 5000;
    cfg.buffer_size       = 2048;
    cfg.buffer_size_tx    = 1024;
    cfg.keep_alive_enable = false;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        snprintf(why, why_sz, "no memory for the HTTP client");
        return -1;
    }
    int n = -1;
    const esp_err_t e = esp_http_client_open(c, 0);
    if (e != ESP_OK) {
        snprintf(why, why_sz, "%s: %s", label, esp_err_to_name(e));
    } else {
        esp_http_client_fetch_headers(c);
        const int status = esp_http_client_get_status_code(c);
        if (status != 200) {
            snprintf(why, why_sz, "%s: HTTP %d", label, status);
        } else {
            size_t got = 0;
            bool failed = false;
            while (got + 1 < cap) {
                const int r = esp_http_client_read(c, buf + got, (int)(cap - 1 - got));
                if (r < 0) { failed = true; break; }
                if (r == 0) break;
                got += (size_t)r;
            }
            if (failed)              snprintf(why, why_sz, "%s: read failed", label);
            else if (got + 1 >= cap) snprintf(why, why_sz, "answer over %u KB", (unsigned)(cap / 1024));
            else if (!esp_http_client_is_complete_data_received(c))
                                     snprintf(why, why_sz, "%s: answer cut off", label);
            else { buf[got] = '\0'; n = (int)got; }
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return n;
}

}  // namespace

int get(const char* host, int port, const char* path, char* buf, size_t cap,
        const char* user_agent, char* why, size_t why_sz) {
    esp_http_client_config_t cfg = {};
    cfg.host           = host;
    cfg.path           = path;
    cfg.port           = port;
    cfg.transport_type = HTTP_TRANSPORT_OVER_TCP;
    cfg.user_agent     = user_agent;
    return fetch(cfg, host, buf, cap, why, why_sz);
}

int get_url(const char* url, char* buf, size_t cap, const char* user_agent, bool allow_tls,
            char* why, size_t why_sz) {
    if (!url || !url[0]) {
        snprintf(why, why_sz, "no URL");
        return -1;
    }
    if (!strncmp(url, "http://", 7)) {
        char host[96];
        char path[256];
        int port = 80;
        if (!radio_art::split_url(url, host, sizeof(host), port, path, sizeof(path))) {
            snprintf(why, why_sz, "unusable URL");
            return -1;
        }
        return get(host, port, path, buf, cap, user_agent, why, why_sz);
    }
    if (!strncmp(url, "https://", 8)) {
        if (!allow_tls) {
            snprintf(why, why_sz, "HTTPS while the station holds the TLS session");
            return -1;
        }
        esp_http_client_config_t cfg = {};
        cfg.url               = url;
        cfg.user_agent        = user_agent;
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
        xSemaphoreTake(tls_lock(), portMAX_DELAY);
        const int n = fetch(cfg, "https", buf, cap, why, why_sz);
        xSemaphoreGive(tls_lock());
        return n;
    }
    snprintf(why, why_sz, "not an http(s) URL");
    return -1;
}

int get_logo(const char* url, char* buf, size_t cap, bool allow_tls, char* why, size_t why_sz) {
    char plain[radio_art::URL_LEN];
    snprintf(plain, sizeof(plain), "%s", url ? url : "");
    radio_art::plain_http(plain);
    int len = get_url(plain, buf, cap, user_agent(), false, why, why_sz);
    if (len <= 0 && url && strcmp(plain, url) != 0)
        len = get_url(url, buf, cap, user_agent(), allow_tls, why, why_sz);
    return len;
}

const char* user_agent() {
    static char ua[48] = "";
    if (!ua[0]) {
        const esp_app_desc_t* app = esp_app_get_description();
        snprintf(ua, sizeof(ua), "P4Player/%s", app ? app->version : "0");
    }
    return ua;
}

}  // namespace radio_http
