/**
 * @file http_service.cpp
 * @brief HTTP server with REST API and static file serving
 */

#include "modesp/net/http_service.h"
#include "modesp/shared_state.h"
#include "modesp/module_manager.h"
#include "modesp/app.h"                // MODESP_WDT_TIMEOUT_MS
#include "modesp/services/config_service.h"
#include "safe_file.h"
#include "user_store.h"
#include "modesp/services/persist_service.h"
#include "modesp/net/wifi_service.h"
#include "modesp/hal/hal.h"
#include "modesp/types.h"
#include "sdkconfig.h"
#include "modesp/hal/driver_registry.h"  // generic discovery dispatch (/api/onewire/scan)
#include "modesp/net/log_source.h"   // /api/log* — джерело через self-registration слот
#include "modesp/log_policy.h"       // /api/loglevel — shipping verbosity and its runtime switch

#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "modesp/services/device_id.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_sntp.h"
#include "modesp/services/nvs_helper.h"
#include "mbedtls/base64.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"

#include "state_meta.h"
#include "jsmn.h"
#include "modesp/net/json_escape.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <new>
#include <sys/time.h>
#include <sys/stat.h>

static const char* TAG = "HTTP";

namespace modesp {

// ── Auth credentials (static, accessible from any handler) ──────

static char s_auth_user[32] = "admin";
static char s_auth_pass[64] = "modesp";
static bool s_auth_enabled = true;
static const char* AUTH_NVS_NS = "auth";

// ── Auth rate-limiting (brute force protection) ──────
static constexpr uint8_t  AUTH_MAX_FAILURES    = 5;
static constexpr uint32_t AUTH_LOCKOUT_MS      = 30000;  // 30 секунд блокування
static uint8_t  s_auth_fail_count = 0;
static uint32_t s_auth_lockout_until = 0;  // xTaskGetTickCount() значення

void HttpService::load_auth_from_nvs() {
    nvs_helper::read_str(AUTH_NVS_NS, "user", s_auth_user, sizeof(s_auth_user));
    nvs_helper::read_str(AUTH_NVS_NS, "pass", s_auth_pass, sizeof(s_auth_pass));
    bool enabled = true;
    nvs_helper::read_bool(AUTH_NVS_NS, "enabled", enabled);
    s_auth_enabled = enabled;
    ESP_LOGI(TAG, "Auth loaded (user='%s', enabled=%d)", s_auth_user, s_auth_enabled);
}

void HttpService::save_auth_to_nvs() {
    nvs_helper::write_str(AUTH_NVS_NS, "user", s_auth_user);
    nvs_helper::write_str(AUTH_NVS_NS, "pass", s_auth_pass);
    nvs_helper::write_bool(AUTH_NVS_NS, "enabled", s_auth_enabled);
    ESP_LOGI(TAG, "Auth saved (user='%s', enabled=%d)", s_auth_user, s_auth_enabled);
}

bool HttpService::check_auth(httpd_req_t* req) {
    // Якщо auth вимкнено — пропускаємо
    if (!s_auth_enabled) return true;

    // Rate-limiting: блокування після AUTH_MAX_FAILURES невдалих спроб
    if (s_auth_fail_count >= AUTH_MAX_FAILURES) {
        uint32_t now = xTaskGetTickCount();
        if ((now - s_auth_lockout_until) < pdMS_TO_TICKS(AUTH_LOCKOUT_MS)) {
            ESP_LOGW(TAG, "Auth locked out (%u failures, %lus remaining)",
                     s_auth_fail_count,
                     (unsigned long)((pdMS_TO_TICKS(AUTH_LOCKOUT_MS) - (now - s_auth_lockout_until))
                                     / configTICK_RATE_HZ));
            httpd_resp_set_status(req, "429 Too Many Requests");
            httpd_resp_set_type(req, "text/plain");
            httpd_resp_send(req, "Too many failed attempts. Try again later.", HTTPD_RESP_USE_STRLEN);
            return false;
        }
        // Lockout минув — скидаємо лічильник
        s_auth_fail_count = 0;
    }

    // Читаємо Authorization header
    size_t hdr_len = httpd_req_get_hdr_value_len(req, "Authorization");
    if (hdr_len == 0 || hdr_len > 200) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ModESP\"");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "Unauthorized", HTTPD_RESP_USE_STRLEN);
        return false;
    }

    char auth_buf[202];
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth_buf, sizeof(auth_buf)) != ESP_OK) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ModESP\"");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "Unauthorized", HTTPD_RESP_USE_STRLEN);
        return false;
    }

    // Перевіряємо "Basic " prefix
    if (strncmp(auth_buf, "Basic ", 6) != 0) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ModESP\"");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "Unauthorized", HTTPD_RESP_USE_STRLEN);
        return false;
    }

    // Base64 decode
    unsigned char decoded[128];
    size_t decoded_len = 0;
    int ret = mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &decoded_len,
                                     (const unsigned char*)(auth_buf + 6),
                                     strlen(auth_buf + 6));
    if (ret != 0 || decoded_len == 0) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ModESP\"");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "Unauthorized", HTTPD_RESP_USE_STRLEN);
        return false;
    }
    decoded[decoded_len] = '\0';

    // Розділяємо user:pass
    char* colon = strchr((char*)decoded, ':');
    if (!colon) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ModESP\"");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "Unauthorized", HTTPD_RESP_USE_STRLEN);
        return false;
    }

    *colon = '\0';
    const char* user = (char*)decoded;
    const char* pass = colon + 1;

    if (strcmp(user, s_auth_user) == 0 && strcmp(pass, s_auth_pass) == 0) {
        s_auth_fail_count = 0;  // Успішний логін — скидаємо лічильник
        return true;
    }

    // Невдала спроба — інкремент лічильника
    s_auth_fail_count++;
    if (s_auth_fail_count >= AUTH_MAX_FAILURES) {
        s_auth_lockout_until = xTaskGetTickCount();
        ESP_LOGW(TAG, "Auth lockout triggered after %u failures", s_auth_fail_count);
    }

    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ModESP\"");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "Unauthorized", HTTPD_RESP_USE_STRLEN);
    return false;
}

// ── Helpers ─────────────────────────────────────────────────────

// AUDIT-039: CORS * видалений — UI на тому ж origin, cross-origin запити не потрібні.
// Це захищає від CSRF атак з зовнішніх сайтів на локальний IP пристрою.
void HttpService::set_cors_headers(httpd_req_t* req) {
    (void)req;
}

esp_err_t HttpService::handle_options(httpd_req_t* req) {
    set_cors_headers(req);
    httpd_resp_send(req, nullptr, 0);
    return ESP_OK;
}

// Helper: read a file from LittleFS into buffer, return bytes read or -1
static int read_file_to_buf(const char* path, char* buf, size_t buf_size) {
    FILE* f = fopen(path, "r");
    if (!f) return -1;
    int len = (int)fread(buf, 1, buf_size - 1, f);
    fclose(f);
    if (len >= 0) buf[len] = '\0';
    return len;
}

// A value outside the manifest range is clamped, never silently: the range is the manifest's
// word, and a request past it is either a client bug or a manifest that lags the code (an
// audio.source of 4 landed on 3 for a while because the options stopped at Bluetooth).
static void log_clamp(const char* where, const char* key, double requested, double clamped,
                      const gen::StateMeta* meta) {
    ESP_LOGW(TAG, "%s: %s=%g is outside [%g..%g], clamped to %g",
             where, key, requested, (double)meta->min_val, (double)meta->max_val, clamped);
}

// player-fork: a JSON value of the type the key's manifest declares. The store takes any type
// for any key, so a string or a true/false written over a number changed what the key holds —
// its readers fell back to their default until a reboot — and a command posted as "2" was
// stored instead of being run.
static bool value_fits(const gen::StateMeta* meta, const jsmntok_t& tok, const char* val) {
    if (strcmp(meta->type, "string") == 0) return tok.type == JSMN_STRING;
    if (tok.type != JSMN_PRIMITIVE) return false;
    if (strcmp(meta->type, "bool") == 0) return strcmp(val, "true") == 0 || strcmp(val, "false") == 0;
    return val[0] == '-' || (val[0] >= '0' && val[0] <= '9');          // "int", "float"
}

// Context for SharedState serialization
struct SerCtx {
    char* buf;
    size_t size;
    size_t pos;
    bool first;
    bool failed;

    size_t remaining() const { return (pos < size) ? size - pos : 0; }
};

/// Deliver a write to a command key to the module the manifest says owns it. Returns false
/// when the key is an ordinary state key, and the caller stores it as before.
/// A serialisation buffer, from PSRAM, allocated once and kept.
///
/// It has to be 8 KB now (a `long` value is up to 256 characters rather than the 63 the old
/// escape buffer allowed), and 8 KB of internal .bss would give back a chunk of what stage 7
/// just freed. Nothing here is DMA: the send copies out of it. Allocated on first use rather
/// than per request, so a request never pays for an allocation, and never on the stack - the
/// tasks that reach this have 8 KB of it.
static char* json_scratch(size_t bytes) {
    static char* p = static_cast<char*>(heap_caps_calloc_prefer(
        1, bytes, 2, MALLOC_CAP_SPIRAM  | MALLOC_CAP_8BIT,
                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    return p;
}

static bool post_if_command(modesp::ModuleManager* modules, const char* key, int32_t value) {
    const auto* cmd = gen::find_state_command(key);
    if (!cmd) return false;
    if (!modules) {
        ESP_LOGW(TAG, "command '%s' with no module manager bound", key);
        return true;                 // consumed: never fall through into the state store
    }
    modesp::BaseModule* m = modules->find(cmd->module);
    if (!m) {
        ESP_LOGW(TAG, "command '%s': module '%s' not registered", key, cmd->module);
        return true;
    }
    if (!m->post_command(key, value))
        ESP_LOGW(TAG, "command '%s' dropped: %s is not keeping up", key, cmd->module);
    return true;
}

static void serialize_state_entry(const StateKey& key, const StateValue& value,
                                  const char* text, void* ud) {
    auto* ctx = static_cast<SerCtx*>(ud);
    if (ctx->failed) return;

    const char* sep = ctx->first ? "" : ",";
    int n = -1;

    if (etl::holds_alternative<float>(value)) {
        n = snprintf(ctx->buf + ctx->pos, ctx->remaining(),
                             "%s\"%s\":%.2f", sep, key.c_str(),
                             static_cast<double>(etl::get<float>(value)));
    } else if (etl::holds_alternative<int32_t>(value)) {
        n = snprintf(ctx->buf + ctx->pos, ctx->remaining(),
                             "%s\"%s\":%ld", sep, key.c_str(),
                             (long)etl::get<int32_t>(value));
    } else if (etl::holds_alternative<bool>(value)) {
        n = snprintf(ctx->buf + ctx->pos, ctx->remaining(),
                             "%s\"%s\":%s", sep, key.c_str(),
                             etl::get<bool>(value) ? "true" : "false");
    } else if (etl::holds_alternative<StringValue>(value)) {
        // AUDIT-004: ескейпимо string значення для коректного JSON
        // From `text`, not from the variant: for a long key the variant holds the inline
        // prefix only. The buffer is the side table's capacity plus room for the escapes,
        // so a track path reaches the browser whole — it used to stop at 63 characters.
        char escaped[net::json_escaped_capacity(MODESP_MAX_LONG_STRING_LENGTH)];
        if (!net::json_escape(escaped, sizeof(escaped), text ? text : "")) {
            ctx->failed = true;
            return;
        }
        n = snprintf(ctx->buf + ctx->pos, ctx->remaining(),
                             "%s\"%s\":\"%s\"", sep, key.c_str(), escaped);
    }
    if (n < 0 || static_cast<size_t>(n) >= ctx->remaining()) ctx->failed = true;
    else { ctx->pos += static_cast<size_t>(n); ctx->first = false; }
}

// ── API Handlers ────────────────────────────────────────────────

esp_err_t HttpService::handle_get_state(httpd_req_t* req) {
    if (!check_auth(req)) return ESP_OK;
    auto* self = static_cast<HttpService*>(req->user_ctx);

    // Room for every declared long value even when every byte needs six JSON bytes,
    // plus the short keys. Overflow is still an explicit 500, never partial JSON.
    // The HTTP server has one task; json_scratch() keeps this buffer in PSRAM.
    constexpr size_t BUF_SIZE = 81920;
    char* buf = json_scratch(BUF_SIZE);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    SerCtx ctx = {buf, BUF_SIZE, 0, true, false};
    ctx.pos += snprintf(buf, BUF_SIZE, "{");

    self->state_->for_each(serialize_state_entry, &ctx);

    if (ctx.failed || ctx.remaining() < 2) {
        // Say so rather than send half an object: whoever added the key that overflowed it
        // needs to see this line, not a JSON error in a browser console.
        ESP_LOGE(TAG, "GET /api/state: %u keys did not fit %u bytes",
                 (unsigned)self->state_->size(), (unsigned)BUF_SIZE);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "state too large");
        return ESP_FAIL;
    }
    buf[ctx.pos++] = '}';
    buf[ctx.pos] = '\0';

    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, (ssize_t)ctx.pos);
    return ESP_OK;
}

esp_err_t HttpService::handle_get_board(httpd_req_t* req) {
    if (!check_auth(req)) return ESP_OK;
    FILE* f = fopen("/data/board.json", "rb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "board.json not found");
        return ESP_FAIL;
    }

    // The same upper bound as the on-device config parser. Read and verify the whole
    // document before sending HTTP 200, so a filesystem error cannot yield partial JSON.
    constexpr size_t CAP = ConfigService::MAX_JSON_SIZE;
    char* buf = static_cast<char*>(heap_caps_malloc(CAP + 1, MALLOC_CAP_8BIT));
    if (!buf) {
        fclose(f);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    const size_t len = fread(buf, 1, CAP + 1, f);
    const bool failed = ferror(f) || len == 0 || len > CAP;
    fclose(f);
    if (failed) {
        heap_caps_free(buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "board.json unreadable or too large");
        return ESP_FAIL;
    }

    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    const esp_err_t rc = httpd_resp_send(req, buf, len);
    heap_caps_free(buf);
    return rc;
}

esp_err_t HttpService::handle_get_ui(httpd_req_t* req) {
    FILE* f = fopen("/data/ui.json", "r");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "ui.json not found");
        return ESP_FAIL;
    }

    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    // Стрімінг файлу чанками — ui.json може бути >4KB
    char buf[512];
    size_t read_bytes;
    while ((read_bytes = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, read_bytes) != ESP_OK) {
            fclose(f);
            httpd_resp_send_chunk(req, nullptr, 0);
            return ESP_FAIL;
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, nullptr, 0);
    return ESP_OK;
}

esp_err_t HttpService::handle_get_bindings(httpd_req_t* req) {
    if (!check_auth(req)) return ESP_OK;
    // The same limit the parser and the POST below use, off the handler stack: a 1 KB buffer
    // here would hand the editor a truncated copy of a file the POST accepts in full, and
    // saving that back would delete the bindings it could not show.
    char* buf = static_cast<char*>(heap_caps_malloc(ConfigService::MAX_JSON_SIZE, MALLOC_CAP_8BIT));
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    int len = read_file_to_buf("/data/bindings.json", buf, ConfigService::MAX_JSON_SIZE);
    if (len < 0) {
        heap_caps_free(buf);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "bindings.json not found");
        return ESP_FAIL;
    }

    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    esp_err_t rc = httpd_resp_send(req, buf, len);
    heap_caps_free(buf);
    return rc;
}

esp_err_t HttpService::handle_post_bindings(httpd_req_t* req) {
    if (!check_auth(req)) return ESP_OK;

    // The body limit is the boot parser's limit, so a file this handler accepts is a file
    // the next boot can read. It used to be 1023 bytes against a 4095-byte parser, and the
    // file was written after a glance at two keys — a bindings.json the parser would reject
    // went straight to flash, and the board came up without its drivers.
    constexpr size_t LIMIT = ConfigService::MAX_JSON_SIZE;
    int remaining = req->content_len;
    if (remaining <= 0 || remaining >= (int)LIMIT) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            remaining <= 0 ? "Empty body" : "Body too large for the config parser");
        return ESP_FAIL;
    }

    // A 4 KB body and a ~13 KB scratch BindingTable do not belong on the 8 KB handler stack.
    struct Scratch {
        char* buf = nullptr;
        BindingTable* table = nullptr;
        ~Scratch() { heap_caps_free(buf); delete table; }
    } scratch;
    scratch.buf   = static_cast<char*>(heap_caps_malloc(LIMIT, MALLOC_CAP_8BIT));
    scratch.table = new (std::nothrow) BindingTable();
    if (!scratch.buf || !scratch.table) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    char* buf = scratch.buf;

    int total = 0;
    int timeouts = 0;   // player-fork: consecutive stalls, bounded — see handle_post_ota()
    while (remaining > 0) {
        int recv_len = httpd_req_recv(req, buf + total, remaining);
        if (recv_len <= 0) {
            if (recv_len == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) continue;
            httpd_resp_send_err(req, recv_len == HTTPD_SOCK_ERR_TIMEOUT ? HTTPD_408_REQ_TIMEOUT
                                                                        : HTTPD_400_BAD_REQUEST,
                                "Receive error");
            return ESP_FAIL;
        }
        timeouts = 0;
        total += recv_len;
        remaining -= recv_len;
    }
    buf[total] = '\0';

    // Write the candidate next to the live file, run the boot parser over it, and only then
    // rename it over the old one: a rejected upload leaves bindings.json as it was, and a
    // power cut mid-write leaves a .new file instead of a half-written config.
    //
    // The sequence itself now lives in components/safe_file (plan item 11.2) so that the EQ,
    // the station list and the resume point get the same one instead of three near-copies.
    // What stays here is the part that is specific to this endpoint: the validator IS the
    // boot parser, so a file this handler accepts is a file the next boot can read.
    static const char* LIVE = "/data/bindings.json";

    struct V {
        static bool parse(const char* path, void* ctx) {
            return ConfigService::load_bindings_file(path, *static_cast<BindingTable*>(ctx));
        }
    };

    if (!safe_file::save(LIVE, buf, (size_t)total, &V::parse, scratch.table)) {
        // Either the write/read-back failed or the parser refused. Tell them apart for the
        // log — "rejected" and "no room on the filesystem" need different answers — by
        // asking the parser directly about what is still on disk.
        const bool parses = ConfigService::load_bindings_file(LIVE, *scratch.table);
        ESP_LOGW(TAG, "POST /bindings: not applied — live file untouched (still %s)",
                 parses ? "valid" : "unreadable");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "bindings.json rejected (parser or filesystem — see device log)");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "POST /bindings: saved %d bytes (%d bindings), restart needed",
             total, (int)scratch.table->bindings.size());

    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"needs_restart\":true}");
    return ESP_OK;
}

// Runtime device registry (/data/devices.json). User-subscribed remote devices (BLE by MAC
// today; transport-generic) — the runtime-writable sibling of board.json remote_devices. GET/POST
// mirror the bindings handlers; ConfigService merges the file into the device inventory at boot
// (runtime-wins-by-id). Bounded so the merged set fits MAX_REMOTE_DEVICES.
static constexpr int MAX_RUNTIME_DEVICES = 12;   // headroom below MAX_REMOTE_DEVICES (16) for the board seed

esp_err_t HttpService::handle_get_devices(httpd_req_t* req) {
    if (!check_auth(req)) return ESP_OK;
    // 4096 matches the boot parser's MAX_JSON_SIZE so GET can echo any file the firmware
    // will actually parse (and POST accepts) — no truncation of a valid registry. Static
    // (not stack): the httpd task serves one request at a time, and 4 KB would strain the
    // 8 KB handler stack.
    static char buf[4096];
    char path[160];
    UserStore::read_path("devices.json", path, sizeof(path));
    int len = read_file_to_buf(path, buf, sizeof(buf));
    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    if (len < 0) {
        // No registry yet — a fresh device has none. Return an empty list, not 404,
        // so the Devices page loads cleanly on first use.
        httpd_resp_sendstr(req, "{\"manifest_version\":1,\"devices\":[]}");
        return ESP_OK;
    }
    httpd_resp_send(req, buf, len);
    return ESP_OK;
}

esp_err_t HttpService::handle_post_devices(httpd_req_t* req) {
    if (!check_auth(req)) return ESP_OK;
    // Static (not stack): matches boot parser MAX_JSON_SIZE + handle_get_devices; the httpd
    // task serves one request at a time, so a shared static body buffer is safe and keeps
    // 4 KB off the 8 KB handler stack.
    static char buf[4096];
    int total = 0;
    int remaining = req->content_len;
    if (remaining <= 0 || remaining >= (int)sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            remaining <= 0 ? "Empty body" : "Body too large");
        return ESP_FAIL;
    }

    int timeouts = 0;   // player-fork: consecutive stalls, bounded — see handle_post_ota()
    while (remaining > 0) {
        int recv_len = httpd_req_recv(req, buf + total, remaining);
        if (recv_len <= 0) {
            if (recv_len == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) continue;
            httpd_resp_send_err(req, recv_len == HTTPD_SOCK_ERR_TIMEOUT ? HTTPD_408_REQ_TIMEOUT
                                                                        : HTTPD_400_BAD_REQUEST,
                                "Receive error");
            return ESP_FAIL;
        }
        timeouts = 0;
        total += recv_len;
        remaining -= recv_len;
    }
    buf[total] = '\0';

    // Validate: manifest_version present + a bounded "devices" array.
    jsmn_parser parser;
    jsmntok_t tokens[160];
    jsmn_init(&parser);
    int r = jsmn_parse(&parser, buf, total, tokens, 160);
    if (r < 1 || tokens[0].type != JSMN_OBJECT) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    bool has_version = false;
    bool has_devices = false;
    int  devices_count = 0;
    for (int i = 1; i + 1 < r; ) {
        if (tokens[i].type != JSMN_STRING) break;
        int klen = tokens[i].end - tokens[i].start;
        const char* kstr = buf + tokens[i].start;
        if (klen == 16 && strncmp(kstr, "manifest_version", 16) == 0) {
            has_version = true;
        } else if (klen == 7 && strncmp(kstr, "devices", 7) == 0) {
            if (tokens[i + 1].type == JSMN_ARRAY) {
                has_devices = true;
                devices_count = tokens[i + 1].size;
            }
        }
        i++;  // skip key
        if (i < r && (tokens[i].type == JSMN_PRIMITIVE ||
                      tokens[i].type == JSMN_STRING)) {
            i++;
        } else if (i < r) {
            int count = 1;
            int j = i + 1;
            while (count > 0 && j < r) {
                if (tokens[j].type == JSMN_OBJECT || tokens[j].type == JSMN_ARRAY) {
                    count += tokens[j].size;
                }
                count--;
                j++;
            }
            i = j;
        }
    }

    if (!has_version || !has_devices) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "Missing manifest_version or devices array");
        return ESP_FAIL;
    }
    if (devices_count > MAX_RUNTIME_DEVICES) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Too many devices");
        return ESP_FAIL;
    }

    // Same write -> read back -> rename as bindings (plan item 11.2). This one used to be a
    // plain fopen("w") + fwrite: it truncated the live registry before the first byte of the
    // new one arrived, so a reset in between left a half-written file. ConfigService only
    // WARNs about an unusable devices.json and carries on, so the loss would have been
    // silent — the subscribed devices would simply be gone at the next boot.
    char path[160];
    UserStore::write_path("devices.json", path, sizeof(path));
    if (!safe_file::save(path, buf, (size_t)total)) {
        ESP_LOGE(TAG, "POST /devices: write failed — the previous registry is still there");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write error");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "POST /devices: saved %d bytes to %s (%d devices), restart needed",
             total, path, devices_count);

    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"needs_restart\":true}");
    return ESP_OK;
}

// Callback context for module serialization
struct ModSerCtx {
    char* buf;
    size_t size;
    size_t pos;
    bool first;
    size_t remaining() const { return (pos < size) ? size - pos : 0; }
};

static const char* state_to_str(BaseModule::State s) {
    switch (s) {
        case BaseModule::State::CREATED:     return "created";
        case BaseModule::State::INITIALIZED: return "initialized";
        case BaseModule::State::RUNNING:     return "running";
        case BaseModule::State::STOPPED:     return "stopped";
        case BaseModule::State::ERROR:       return "error";
        default:                             return "unknown";
    }
}

static void serialize_module(const BaseModule& module, void* ud) {
    auto* ctx = static_cast<ModSerCtx*>(ud);
    if (ctx->remaining() < 128) return;

    const char* sep = ctx->first ? "" : ",";
    ctx->first = false;

    ctx->pos += snprintf(ctx->buf + ctx->pos, ctx->remaining(),
                         "%s{\"name\":\"%s\",\"priority\":%d,\"state\":\"%s\"}",
                         sep,
                         module.name(),
                         static_cast<int>(module.priority()),
                         state_to_str(module.state()));
}

esp_err_t HttpService::handle_get_modules(httpd_req_t* req) {
    if (!check_auth(req)) return ESP_OK;
    auto* self = static_cast<HttpService*>(req->user_ctx);

    char buf[1536];
    ModSerCtx ctx = {buf, sizeof(buf), 0, true};
    ctx.pos += snprintf(buf, sizeof(buf), "[");

    self->modules_->for_each_module(serialize_module, &ctx);

    ctx.pos += snprintf(buf + ctx.pos, ctx.remaining(), "]");

    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, (ssize_t)ctx.pos);
    return ESP_OK;
}

esp_err_t HttpService::handle_post_settings(httpd_req_t* req) {
    if (!check_auth(req)) return ESP_OK;
    auto* self = static_cast<HttpService*>(req->user_ctx);

    // 512B: один ключ до 32 chars + float = ~40B → вміщує до ~12 пар за раз.
    // 32 tokens: 1 об'єкт + 2 tokens/пара × 15 пар = 31 tokens — з запасом.
    char buf[512];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    buf[len] = '\0';

    // Parse with jsmn
    jsmn_parser parser;
    jsmntok_t tokens[32];
    jsmn_init(&parser);
    int r = jsmn_parse(&parser, buf, len, tokens, 32);
    if (r < 1 || tokens[0].type != JSMN_OBJECT) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    int accepted = 0;
    int rejected = 0;

    for (int i = 1; i + 1 < r; i += 2) {
        if (tokens[i].type != JSMN_STRING) continue;

        const char* key_str = buf + tokens[i].start;
        const jsmntok_t& val_tok = tokens[i + 1];

        // Null-terminate key and value in-place
        buf[tokens[i].end] = '\0';
        buf[val_tok.end] = '\0';
        const char* val_str = buf + val_tok.start;

        // Валідація через state_meta — тільки відомі writable ключі
        const auto* meta = gen::find_state_meta(key_str);
        if (!meta || !meta->writable) {
            ESP_LOGW(TAG, "POST /settings: rejected key '%s' (unknown or readonly)", key_str);
            rejected++;
            continue;
        }
        if (!value_fits(meta, val_tok, val_str)) {
            ESP_LOGW(TAG, "POST /settings: rejected '%s' (not a %s)", key_str, meta->type);
            rejected++;
            continue;
        }
        // player-fork: jsmn hands a string over as it stands in the body, escapes and all, so a
        // catalog station called Radio "Rock" was stored as Radio \"Rock\", in stations.json too.
        if (val_tok.type == JSMN_STRING && !net::json_unescape(buf + val_tok.start)) {
            ESP_LOGW(TAG, "POST /settings: rejected '%s' (an escape that cannot be stored)", key_str);
            rejected++;
            continue;
        }

        if (val_tok.type == JSMN_PRIMITIVE) {
            // BUG-023 fix: використовуємо meta->type замість евристики з крапкою.
            // JavaScript може відправити "5" (без крапки) для float параметра,
            // що ламало persist (etl::get_if<float> на int32_t = nullptr).
            if (strcmp(meta->type, "bool") == 0) {
                self->state_->set(key_str, val_str[0] == 't');
                accepted++;
            } else if (strcmp(meta->type, "float") == 0) {
                float fval = static_cast<float>(atof(val_str));
                const float f_req = fval;
                if (fval < meta->min_val) fval = meta->min_val;
                if (fval > meta->max_val) fval = meta->max_val;
                if (fval != f_req) log_clamp("POST /settings", key_str, (double)f_req, (double)fval, meta);
                self->state_->set(key_str, fval);
                accepted++;
            } else {
                int32_t ival = static_cast<int32_t>(atoi(val_str));
                const int32_t i_req = ival;
                if (static_cast<float>(ival) < meta->min_val) ival = static_cast<int32_t>(meta->min_val);
                if (static_cast<float>(ival) > meta->max_val) ival = static_cast<int32_t>(meta->max_val);
                if (ival != i_req) log_clamp("POST /settings", key_str, (double)i_req, (double)ival, meta);
                // A command key is an event for a module, not a value in the store: it goes
                // to that module's queue after the same range check. See STATE_COMMANDS in
                // the generated state_meta.h, and BaseModule::post_command().
                if (post_if_command(self->modules_, key_str, ival)) { accepted++; continue; }
                self->state_->set(key_str, ival);
                accepted++;
            }
        } else {                                                    // a string key (value_fits)
            self->state_->set(key_str, val_str);
            accepted++;
        }
    }

    ESP_LOGI(TAG, "POST /settings: %d accepted, %d rejected", accepted, rejected);

    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    if (rejected > 0 && accepted == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "All keys rejected");
        return ESP_FAIL;
    }
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

esp_err_t HttpService::handle_post_wifi(httpd_req_t* req) {
    if (!check_auth(req)) return ESP_OK;
    auto* self = static_cast<HttpService*>(req->user_ctx);

    char buf[256];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    buf[len] = '\0';

    jsmn_parser parser;
    jsmntok_t tokens[8];
    jsmn_init(&parser);
    int r = jsmn_parse(&parser, buf, len, tokens, 8);
    if (r < 1 || tokens[0].type != JSMN_OBJECT) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    const char* ssid = nullptr;
    const char* pass = nullptr;
    bool join = false, forget = false;

    for (int i = 1; i + 1 < r; i += 2) {
        if (tokens[i].type != JSMN_STRING) continue;
        buf[tokens[i].end] = '\0';
        buf[tokens[i + 1].end] = '\0';

        const char* key = buf + tokens[i].start;
        const char* val = buf + tokens[i + 1].start;

        // player-fork: jsmn leaves a string's escapes in it, so a name or password holding a quote
        // or a backslash arrived escaped and was saved that way. Unescaped in place.
        if ((strcmp(key, "ssid") == 0 || strcmp(key, "password") == 0) &&
            (tokens[i + 1].type != JSMN_STRING || !net::json_unescape(buf + tokens[i + 1].start))) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid and password must be JSON strings");
            return ESP_FAIL;
        }
        if (strcmp(key, "ssid") == 0) ssid = val;
        else if (strcmp(key, "password") == 0) pass = val;
        else if (strcmp(key, "join") == 0) join = (strcmp(val, "true") == 0);
        else if (strcmp(key, "forget") == 0) forget = (strcmp(val, "true") == 0);
    }

    // player-fork: join or forget from the web as the panel does — try_connect() keeps the
    // network that works until the new one gives an address (wifi_service.h), so a mistyped
    // password from a phone no longer strands the device. Both run on the main loop
    // (on_update()); progress is wifi.try / wifi.try_ssid / wifi.reason, as for the panel.
    if (self->wifi_ && forget) {
        self->wifi_forget_.store(true, std::memory_order_release);
        set_cors_headers(req);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":true,\"forget\":true}");
        return ESP_OK;
    }
    // player-fork: what a wifi_config_t holds — an SSID of 32 bytes, a password of up to 64 (64
    // is a raw hex PSK) — for a save as for a join. A save took any length: an SSID over 32 bytes
    // then failed to load at boot, which brought the access point up, and a longer password read
    // back empty.
    const bool fits = ssid && ssid[0] && strlen(ssid) < sizeof(self->wifi_join_ssid_) &&
                      (!pass || strlen(pass) < sizeof(self->wifi_join_pass_));
    if (self->wifi_ && join) {
        if (!fits) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid 1..32, password up to 64");
            return ESP_FAIL;
        }
        if (self->wifi_->is_trying() || self->wifi_join_.load(std::memory_order_acquire)) {
            set_cors_headers(req);
            httpd_resp_set_status(req, "409 Conflict");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, "{\"error\":\"busy\"}");
            return ESP_OK;
        }
        strncpy(self->wifi_join_ssid_, ssid, sizeof(self->wifi_join_ssid_) - 1);
        self->wifi_join_ssid_[sizeof(self->wifi_join_ssid_) - 1] = '\0';
        strncpy(self->wifi_join_pass_, pass ? pass : "", sizeof(self->wifi_join_pass_) - 1);
        self->wifi_join_pass_[sizeof(self->wifi_join_pass_) - 1] = '\0';
        self->wifi_join_.store(true, std::memory_order_release);
        set_cors_headers(req);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":true,\"join\":true}");
        return ESP_OK;
    }

    if (!ssid || !self->wifi_) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing ssid");
        return ESP_FAIL;
    }
    if (!fits) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid 1..32, password up to 64");
        return ESP_FAIL;
    }

    bool ok = self->wifi_->save_credentials(ssid, pass ? pass : "");

    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    if (ok) {
        httpd_resp_sendstr(req, "{\"ok\":true,\"msg\":\"Credentials saved. Restart to connect.\"}");
        // Не робимо reconnect автоматично — UI каже користувачу перезавантажити.
        // Якщо робити reconnect тут — AP зникає і клієнт втрачає з'єднання
        // ще до отримання HTTP response.
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save");
    }
    return ESP_OK;
}

// player-fork: the scan's security, named after IDF's own wifi_auth_mode_t. The table this
// replaces counted 0..6 as IDF 4.x had them, and IDF 5 put ENTERPRISE at 5: a WPA2-Enterprise
// network was sent as "WPA3", WPA3 as "WPA2/WPA3" and WPA2/WPA3 as "unknown".
static const char* wifi_auth_name(uint8_t mode) {
    switch (mode) {
    case WIFI_AUTH_OPEN:                    return "open";
    case WIFI_AUTH_WEP:                     return "WEP";
    case WIFI_AUTH_WPA_PSK:                 return "WPA";
    case WIFI_AUTH_WPA2_PSK:                return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:            return "WPA/WPA2";
    case WIFI_AUTH_ENTERPRISE:              return "WPA2-Enterprise";
    case WIFI_AUTH_WPA3_PSK:
    case WIFI_AUTH_WPA3_EXT_PSK:
    case WIFI_AUTH_WPA3_EXT_PSK_MIXED_MODE: return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:           return "WPA2/WPA3";
    case WIFI_AUTH_WAPI_PSK:                return "WAPI";
    case WIFI_AUTH_OWE:                     return "OWE";
    case WIFI_AUTH_WPA3_ENT_192:            return "WPA3-Enterprise-192";
    case WIFI_AUTH_DPP:                     return "DPP";
    case WIFI_AUTH_WPA3_ENTERPRISE:         return "WPA3-Enterprise";
    case WIFI_AUTH_WPA2_WPA3_ENTERPRISE:    return "WPA2/WPA3-Enterprise";
    case WIFI_AUTH_WPA_ENTERPRISE:          return "WPA-Enterprise";
    default:                                return "unknown";
    }
}

esp_err_t HttpService::handle_get_wifi_scan(httpd_req_t* req) {
    if (!check_auth(req)) return ESP_OK;
    auto* self = static_cast<HttpService*>(req->user_ctx);

    if (!self->wifi_) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "WiFi service unavailable");
        return ESP_FAIL;
    }

    // Start blocking scan
    if (!self->wifi_->start_scan()) {
        set_cors_headers(req);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"networks\":[]}");
        return ESP_OK;
    }

    // Get results
    WiFiService::ScanResult results[WiFiService::MAX_SCAN_RESULTS];
    int count = self->wifi_->get_scan_results(results, WiFiService::MAX_SCAN_RESULTS);

    // Serialize to JSON.
    // player-fork: twenty names escaped (up to six bytes for one) do not fit the 2 KB this had,
    // and a name with " in it broke the whole list, so the names are escaped and every entry
    // goes in whole or not at all. Static: the server runs one handler at a time, and this is
    // more than its stack should carry.
    static char buf[64 + WiFiService::MAX_SCAN_RESULTS * (net::json_escaped_capacity(32) + 48)];
    size_t pos = (size_t)snprintf(buf, sizeof(buf), "{\"ok\":true,\"networks\":[");

    for (int i = 0; i < count; i++) {
        char ssid[net::json_escaped_capacity(sizeof(results[i].ssid) - 1)];
        if (!net::json_escape(ssid, sizeof(ssid), results[i].ssid)) continue;
        const size_t room = sizeof(buf) - pos - 3;          // "]}" and the NUL stay free
        const int w = snprintf(buf + pos, room, "%s{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":\"%s\"}",
                               i == 0 ? "" : ",", ssid, results[i].rssi,
                               wifi_auth_name(results[i].authmode));
        if (w < 0 || (size_t)w >= room) break;
        pos += (size_t)w;
    }
    pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "]}");

    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, (ssize_t)pos);
    return ESP_OK;
}

esp_err_t HttpService::handle_get_wifi_ap(httpd_req_t* req) {
    // Повертаємо поточну конфігурацію AP з NVS
    char ap_ssid[33] = {};
    int32_t ap_channel = 1;
    bool has_password = false;

    nvs_helper::read_str("wifi", "ap_ssid", ap_ssid, sizeof(ap_ssid));
    nvs_helper::read_i32("wifi", "ap_chan", ap_channel);

    // Якщо кастомний SSID не збережений — показуємо дефолтний (ModESP-XXXX)
    if (ap_ssid[0] == '\0') {
        const uint8_t* mac = modesp::device_mac();
        snprintf(ap_ssid, sizeof(ap_ssid), "ModESP-%02X%02X", mac[4], mac[5]);
    }

    // Перевірити чи є збережений пароль (не повертаємо сам пароль)
    // player-fork: read as start_ap() reads it, so a password too long for the old 65 bytes is
    // reported as set — the access point is WPA2 with it.
    char ap_pass[64];
    WiFiService::read_ap_password(ap_pass);
    has_password = (strlen(ap_pass) >= 8);

    char buf[256];
    char escaped_ssid[net::json_escaped_capacity(sizeof(ap_ssid) - 1)];
    if (!net::json_escape(escaped_ssid, sizeof(escaped_ssid), ap_ssid)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Invalid SSID");
        return ESP_FAIL;
    }

    snprintf(buf, sizeof(buf),
             "{\"ssid\":\"%s\",\"channel\":%ld,\"password_set\":%s}",
             escaped_ssid, (long)ap_channel,
             has_password ? "true" : "false");

    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

esp_err_t HttpService::handle_post_wifi_ap(httpd_req_t* req) {
    if (!check_auth(req)) return ESP_OK;
    char buf[256];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    buf[len] = '\0';

    jsmn_parser parser;
    jsmntok_t tokens[12];
    jsmn_init(&parser);
    int r = jsmn_parse(&parser, buf, len, tokens, 12);
    if (r < 1 || tokens[0].type != JSMN_OBJECT) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    const char* ssid = nullptr;
    const char* password = nullptr;
    int channel = -1;

    for (int i = 1; i + 1 < r; i += 2) {
        if (tokens[i].type != JSMN_STRING) continue;
        buf[tokens[i].end] = '\0';
        buf[tokens[i + 1].end] = '\0';

        const char* key = buf + tokens[i].start;
        const char* val = buf + tokens[i + 1].start;

        // player-fork: as in handle_post_wifi — the access point would otherwise be given a
        // name or password with the JSON's backslashes still in it.
        if ((strcmp(key, "ssid") == 0 || strcmp(key, "password") == 0) &&
            (tokens[i + 1].type != JSMN_STRING || !net::json_unescape(buf + tokens[i + 1].start))) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid and password must be JSON strings");
            return ESP_FAIL;
        }
        if (strcmp(key, "ssid") == 0) ssid = val;
        else if (strcmp(key, "password") == 0) password = val;
        else if (strcmp(key, "channel") == 0) channel = atoi(val);
    }

    // player-fork: an SSID the access point can carry (32 bytes), and a WPA2 passphrase of 8..63
    // or none. A longer password was stored and answered ok, start_ap() could not read it back,
    // and the access point came up open while its owner believed it was WPA2.
    if (!ssid || strlen(ssid) == 0 || strlen(ssid) > 32) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID must be 1-32 bytes");
        return ESP_FAIL;
    }

    // Пароль: або порожній (open), або 8..63 символи (WPA2)
    if (password && strlen(password) > 0 && (strlen(password) < 8 || strlen(password) > 63)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "Password must be empty (open) or 8-63 characters");
        return ESP_FAIL;
    }

    if (channel < 1 || channel > 13) channel = 1;

    // Зберігаємо в NVS
    nvs_helper::write_str("wifi", "ap_ssid", ssid);
    nvs_helper::write_str("wifi", "ap_pass", password ? password : "");
    nvs_helper::write_i32("wifi", "ap_chan", (int32_t)channel);

    ESP_LOGI(TAG, "AP config saved: ssid='%s' channel=%d pass=%s",
             ssid, channel, (password && strlen(password) >= 8) ? "set" : "open");

    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"msg\":\"AP config saved. Restart to apply.\"}");
    return ESP_OK;
}

// player-fork: the shutdown handlers below called stop_all() on this task while the main loop
// went on calling on_update() — a module's teardown ran under its own update (an MQTT client
// freed under a publish, a file closed under the task still reading it). The stop is handed to
// the main loop and waited for; a stop that has not finished in STOP_WAIT_MS is abandoned and
// the handler restarts anyway. After the wait come the final flush, the delay before
// esp_restart() (at most 1 s) and esp_restart()'s shutdown handlers — esp_wifi_stop among them,
// a hosted RPC — all while main may still be blocked. RESTART_TAIL_MS budgets that tail, so an
// on_stop() stuck on main is cut short by the restart rather than by a watchdog panic.
static constexpr uint32_t STOP_WAIT_MS = 2000;
static constexpr uint32_t RESTART_TAIL_MS = 2500;
static_assert(STOP_WAIT_MS + RESTART_TAIL_MS < MODESP_WDT_TIMEOUT_MS,
              "the restart must come before the main-loop watchdog");

static void stop_modules_on_main(ModuleManager* modules) {
    if (!modules) return;
    modules->request_stop_all();
    const TickType_t start = xTaskGetTickCount();
    while (!modules->stop_all_done()) {
        if (xTaskGetTickCount() - start >= pdMS_TO_TICKS(STOP_WAIT_MS)) {
            ESP_LOGW(TAG, "Modules not stopped in %lu ms, restarting without it",
                     (unsigned long)STOP_WAIT_MS);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t HttpService::handle_post_restart(httpd_req_t* req) {
    if (!check_auth(req)) return ESP_OK;
    auto* self = static_cast<HttpService*>(req->user_ctx);
    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"msg\":\"Restarting...\"}");

    // player-fork: flushed before the stop as well, as handle_post_restore does — a stop that
    // goes wrong must not take the debounced settings with it.
    if (self->persist_) {
        self->persist_->flush_now();
    }

    // Graceful shutdown: flush log sources to flash, зупинити модулі
    stop_modules_on_main(self->modules_);

    // BUG-014: flush pending NVS writes before restart
    if (self->persist_) {
        self->persist_->flush_now();
    }

    // Delay to allow response to be sent
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK; // never reached
}

esp_err_t HttpService::handle_post_factory_reset(httpd_req_t* req) {
    if (!check_auth(req)) return ESP_OK;
    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    ESP_LOGW("HTTP", "FACTORY RESET — erasing NVS partition...");
    esp_err_t err = nvs_flash_erase();
    if (err != ESP_OK) {
        ESP_LOGE("HTTP", "NVS erase failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS erase failed");
        return ESP_FAIL;
    }

    httpd_resp_sendstr(req, "{\"ok\":true,\"msg\":\"Factory reset. Restarting...\"}");

    // Graceful shutdown: flush log sources to flash
    auto* self = static_cast<HttpService*>(req->user_ctx);
    stop_modules_on_main(self->modules_);

    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK; // never reached
}

// ── Backup / Restore ────────────────────────────────────────────

esp_err_t HttpService::handle_get_backup(httpd_req_t* req) {
    if (!check_auth(req)) return ESP_OK;
    auto* self = static_cast<HttpService*>(req->user_ctx);

    // Серіалізуємо тільки persist:true ключі з STATE_META
    char buf[2048];
    size_t pos = 0;
    pos += snprintf(buf, sizeof(buf), "{\"modesp_backup\":1,\"keys\":{");

    bool first = true;
    for (size_t i = 0; i < gen::STATE_META_COUNT; i++) {
        const auto& meta = gen::STATE_META[i];
        if (!meta.persist) continue;

        auto val = self->state_->get(meta.key);
        if (!val.has_value()) continue;

        size_t rem = sizeof(buf) - pos;
        if (rem < 64) break;

        // player-fork: the separator goes with a value that is written. A key whose value is not
        // of its manifest type is left out, and it used to take a "," with it: as the first key
        // that made `{"keys":{,` — a backup nothing could read back.
        const char* sep = first ? "" : ",";
        int n = 0;

        if (strcmp(meta.type, "float") == 0) {
            const auto* fp = etl::get_if<float>(&val.value());
            if (fp) {
                n = snprintf(buf + pos, rem, "%s\"%s\":%.2f",
                             sep, meta.key, static_cast<double>(*fp));
            }
        } else if (strcmp(meta.type, "int") == 0) {
            const auto* ip = etl::get_if<int32_t>(&val.value());
            if (ip) {
                n = snprintf(buf + pos, rem, "%s\"%s\":%ld",
                             sep, meta.key, (long)*ip);
            }
        } else if (strcmp(meta.type, "bool") == 0) {
            const auto* bp = etl::get_if<bool>(&val.value());
            if (bp) {
                n = snprintf(buf + pos, rem, "%s\"%s\":%s",
                             sep, meta.key, *bp ? "true" : "false");
            }
        }
        if (n > 0 && (size_t)n < rem) {
            pos += (size_t)n;
            first = false;
        }
    }

    pos += snprintf(buf + pos, sizeof(buf) - pos, "}}");

    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Content-Disposition",
                       "attachment; filename=\"modesp_backup.json\"");
    httpd_resp_send(req, buf, (ssize_t)pos);
    return ESP_OK;
}

esp_err_t HttpService::handle_post_restore(httpd_req_t* req) {
    if (!check_auth(req)) return ESP_OK;
    auto* self = static_cast<HttpService*>(req->user_ctx);

    char buf[2048];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    buf[len] = '\0';

    // Парсимо JSON — шукаємо об'єкт "keys"
    jsmn_parser parser;
    jsmntok_t tokens[128];  // ~33 persist ключів × 2 + envelope
    jsmn_init(&parser);
    int r = jsmn_parse(&parser, buf, len, tokens, 128);
    if (r < 1 || tokens[0].type != JSMN_OBJECT) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    // Знаходимо "keys" об'єкт
    int keys_tok = -1;
    for (int i = 1; i + 1 < r; i += 2) {
        if (tokens[i].type != JSMN_STRING) continue;
        int klen = tokens[i].end - tokens[i].start;
        if (klen == 4 && strncmp(buf + tokens[i].start, "keys", 4) == 0) {
            keys_tok = i + 1;
            break;
        }
        // Пропускаємо значення (може бути вкладений об'єкт)
        if (tokens[i + 1].type == JSMN_OBJECT || tokens[i + 1].type == JSMN_ARRAY) {
            int skip = tokens[i + 1].size * 2;
            i += skip;
        }
    }

    if (keys_tok < 0 || tokens[keys_tok].type != JSMN_OBJECT) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'keys' object");
        return ESP_FAIL;
    }

    int restored = 0;
    int skipped = 0;
    int key_count = tokens[keys_tok].size;

    // Ітеруємо пари ключ:значення всередині "keys"
    int j = keys_tok + 1;
    for (int k = 0; k < key_count && j + 1 < r; k++, j += 2) {
        if (tokens[j].type != JSMN_STRING) continue;

        buf[tokens[j].end] = '\0';
        buf[tokens[j + 1].end] = '\0';
        const char* key_str = buf + tokens[j].start;
        const char* val_str = buf + tokens[j + 1].start;

        // Валідація: тільки persist ключі
        const auto* meta = gen::find_state_meta(key_str);
        if (!meta || !meta->persist) {
            skipped++;
            continue;
        }

        if (tokens[j + 1].type == JSMN_PRIMITIVE) {
            if (val_str[0] == 't' || val_str[0] == 'f') {
                self->state_->set(key_str, val_str[0] == 't');
                restored++;
            } else if (strcmp(meta->type, "float") == 0) {
                float fval = static_cast<float>(atof(val_str));
                const float f_req = fval;
                if (fval < meta->min_val) fval = meta->min_val;
                if (fval > meta->max_val) fval = meta->max_val;
                if (fval != f_req) log_clamp("POST /restore", key_str, (double)f_req, (double)fval, meta);
                self->state_->set(key_str, fval);
                restored++;
            } else {
                int32_t ival = static_cast<int32_t>(atoi(val_str));
                const int32_t i_req = ival;
                if (static_cast<float>(ival) < meta->min_val)
                    ival = static_cast<int32_t>(meta->min_val);
                if (static_cast<float>(ival) > meta->max_val)
                    ival = static_cast<int32_t>(meta->max_val);
                if (ival != i_req) log_clamp("POST /restore", key_str, (double)i_req, (double)ival, meta);
                self->state_->set(key_str, ival);
                restored++;
            }
        }
    }

    ESP_LOGI(TAG, "Restore: %d restored, %d skipped", restored, skipped);

    // Flush NVS перед рестартом
    if (self->persist_) {
        self->persist_->flush_now();
    }

    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    char resp[96];
    snprintf(resp, sizeof(resp),
             "{\"ok\":true,\"restored\":%d,\"skipped\":%d}", restored, skipped);
    httpd_resp_sendstr(req, resp);

    // Graceful shutdown: flush log sources to flash
    stop_modules_on_main(self->modules_);

    // Перезавантаження для застосування налаштувань
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

// ── OTA Handlers ────────────────────────────────────────────────

esp_err_t HttpService::handle_get_ota(httpd_req_t* req) {
    set_cors_headers(req);

    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_app_desc_t* desc = esp_app_get_description();

    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"partition\":\"%s\",\"version\":\"%s\",\"idf\":\"%s\","
        "\"date\":\"%s\",\"time\":\"%s\",\"board\":\"%s\"}",
        running ? running->label : "?",
        desc->version,
        desc->idf_ver,
        desc->date,
        desc->time,
        desc->project_name);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

esp_err_t HttpService::handle_post_ota(httpd_req_t* req) {
    if (!check_auth(req)) return ESP_OK;
    set_cors_headers(req);

    ESP_LOGI(TAG, "OTA: Starting firmware update (%d bytes)", req->content_len);

    if (req->content_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }

    // Знайти OTA partition
    const esp_partition_t* update_partition = esp_ota_get_next_update_partition(nullptr);
    if (!update_partition) {
        ESP_LOGE(TAG, "OTA: No OTA partition found");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
        return ESP_FAIL;
    }

    // Валідація розміру: firmware повинна поміститися в partition
    if (req->content_len > (int)update_partition->size) {
        ESP_LOGE(TAG, "OTA: Firmware too large (%d > %lu)",
                 req->content_len, (unsigned long)update_partition->size);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Firmware too large");
        return ESP_FAIL;
    }
    if (req->content_len < 256) {
        ESP_LOGE(TAG, "OTA: File too small (%d bytes)", req->content_len);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "File too small");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA: Writing to partition '%s' at offset 0x%lx (%d bytes)",
             update_partition->label, (unsigned long)update_partition->address,
             req->content_len);

    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA: esp_ota_begin failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_FAIL;
    }

    // Приймаємо firmware частинами
    char buf[1024];
    int remaining = req->content_len;
    int received_total = 0;
    bool magic_checked = false;
    bool board_checked = false;
    // player-fork: consecutive recv timeouts (recv_wait_timeout, 30 s each). Retried for ever,
    // a client that vanished without FIN or RST (a phone off the Wi-Fi mid-upload) held the
    // only httpd task — every page, API call, WS delta and accept — until a reboot. Three in a
    // row (~90 s) end the request; progress resets the count. Same bound as userdata_post.
    int timeouts = 0;

    while (remaining > 0) {
        int to_recv = (remaining < (int)sizeof(buf)) ? remaining : (int)sizeof(buf);
        int recv_len = httpd_req_recv(req, buf, to_recv);
        if (recv_len <= 0) {
            if (recv_len == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) {
                continue; // Retry на timeout
            }
            ESP_LOGE(TAG, "OTA: Receive error at %d/%d bytes", received_total, req->content_len);
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, recv_len == HTTPD_SOCK_ERR_TIMEOUT
                                         ? HTTPD_408_REQ_TIMEOUT : HTTPD_500_INTERNAL_SERVER_ERROR,
                                "Receive error");
            return ESP_FAIL;
        }
        timeouts = 0;

        // Перевірка magic byte (ESP_IMAGE_HEADER_MAGIC = 0xE9) у першому chunk
        if (!magic_checked && recv_len > 0) {
            magic_checked = true;
            if (static_cast<uint8_t>(buf[0]) != 0xE9) {
                ESP_LOGE(TAG, "OTA: Invalid magic byte 0x%02X (expected 0xE9)",
                         static_cast<uint8_t>(buf[0]));
                esp_ota_abort(ota_handle);
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Not a valid ESP32 firmware");
                return ESP_FAIL;
            }
        }

        // Валідація плати: esp_app_desc_t на offset 0x20 у .bin
        if (!board_checked && received_total == 0 && recv_len >= 0x70) {
            board_checked = true;
            static constexpr size_t DESC_OFFSET = 0x20;
            static constexpr size_t NAME_OFFSET = DESC_OFFSET + 48; // project_name
            uint32_t desc_magic;
            memcpy(&desc_magic, buf + DESC_OFFSET, 4);
            if (desc_magic == 0xABCD5432) {
                char incoming_name[33] = {};
                memcpy(incoming_name, buf + NAME_OFFSET, 32);
                const char* running_name = esp_app_get_description()->project_name;
                if (strcmp(incoming_name, running_name) != 0) {
                    ESP_LOGE(TAG, "OTA: Board mismatch: running '%s', incoming '%s'",
                             running_name, incoming_name);
                    esp_ota_abort(ota_handle);
                    char err_msg[160];
                    snprintf(err_msg, sizeof(err_msg),
                        "{\"error\":\"board_mismatch\",\"running\":\"%s\",\"incoming\":\"%s\"}",
                        running_name, incoming_name);
                    httpd_resp_set_type(req, "application/json");
                    httpd_resp_set_status(req, "400 Bad Request");
                    httpd_resp_sendstr(req, err_msg);
                    return ESP_FAIL;
                }
                ESP_LOGI(TAG, "OTA: Board check OK (%s)", running_name);
            }
        }

        err = esp_ota_write(ota_handle, buf, recv_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA: Write failed: %s", esp_err_to_name(err));
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA write failed");
            return ESP_FAIL;
        }

        remaining -= recv_len;
        received_total += recv_len;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA: Validation failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA validation failed");
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA: Set boot partition failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Set boot partition failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA: Success! Wrote %d bytes to '%s'", received_total, update_partition->label);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"Firmware updated. Restarting...\"}");

    // Graceful shutdown: flush log sources to flash
    auto* self = static_cast<HttpService*>(req->user_ctx);
    stop_modules_on_main(self->modules_);

    // Дати HTTP response дійти до клієнта перед restart
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK; // unreachable
}

// ── Time API ────────────────────────────────────────────────────

esp_err_t HttpService::handle_get_time(httpd_req_t* req) {
    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    bool ntp_enabled = true;
    nvs_helper::read_bool("time", "ntp_enabled", ntp_enabled);

    char tz[48] = "EET-2EEST,M3.5.0/3,M10.5.0/4";
    nvs_helper::read_str("time", "timezone", tz, sizeof(tz));

    time_t now = time(nullptr);
    bool synced = (now > 1700000000);

    char time_str[9] = "--:--:--";
    char date_str[11] = "--.--.----";
    if (synced) {
        struct tm ti;
        localtime_r(&now, &ti);
        strftime(time_str, sizeof(time_str), "%H:%M:%S", &ti);
        strftime(date_str, sizeof(date_str), "%d.%m.%Y", &ti);
    }

    char json[256];
    snprintf(json, sizeof(json),
             "{\"time\":\"%s\",\"date\":\"%s\",\"ntp_enabled\":%s,"
             "\"timezone\":\"%s\",\"synced\":%s,\"unix\":%ld}",
             time_str, date_str,
             ntp_enabled ? "true" : "false",
             tz,
             synced ? "true" : "false",
             (long)now);
    httpd_resp_sendstr(req, json);
    return ESP_OK;
}

esp_err_t HttpService::handle_post_time(httpd_req_t* req) {
    if (!check_auth(req)) return ESP_OK;
    set_cors_headers(req);

    char buf[256];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    buf[len] = '\0';

    jsmn_parser parser;
    jsmntok_t tokens[12];
    jsmn_init(&parser);
    int r = jsmn_parse(&parser, buf, len, tokens, 12);
    if (r < 1 || tokens[0].type != JSMN_OBJECT) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    const char* mode = nullptr;
    long unix_time = 0;
    char new_tz[48] = {};

    for (int i = 1; i + 1 < r; i += 2) {
        if (tokens[i].type != JSMN_STRING) continue;
        buf[tokens[i].end] = '\0';
        buf[tokens[i + 1].end] = '\0';
        const char* key = buf + tokens[i].start;
        const char* val = buf + tokens[i + 1].start;

        if (strcmp(key, "mode") == 0) {
            mode = val;
        } else if (strcmp(key, "time") == 0) {
            unix_time = atol(val);
        } else if (strcmp(key, "timezone") == 0) {
            strncpy(new_tz, val, sizeof(new_tz) - 1);
        }
    }

    // Зберегти timezone якщо передано
    if (new_tz[0]) {
        nvs_helper::write_str("time", "timezone", new_tz);
        setenv("TZ", new_tz, 1);
        tzset();
        ESP_LOGI(TAG, "Timezone set: %s", new_tz);
    }

    if (mode && strcmp(mode, "ntp") == 0) {
        nvs_helper::write_bool("time", "ntp_enabled", true);
        if (esp_sntp_enabled()) {
            esp_sntp_stop();
        }
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "pool.ntp.org");
        esp_sntp_init();
        ESP_LOGI(TAG, "NTP enabled, syncing...");
    } else if (mode && strcmp(mode, "manual") == 0) {
        nvs_helper::write_bool("time", "ntp_enabled", false);
        if (esp_sntp_enabled()) {
            esp_sntp_stop();
        }
        if (unix_time > 1700000000) {
            struct timeval tv = { .tv_sec = unix_time, .tv_usec = 0 };
            settimeofday(&tv, nullptr);
            ESP_LOGI(TAG, "Manual time set: %ld", unix_time);
        }
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// ── OneWire Scan Handler ────────────────────────────────────────

esp_err_t HttpService::handle_get_ow_scan(httpd_req_t* req) {
    if (!check_auth(req)) return ESP_OK;
    auto* self = static_cast<HttpService*>(req->user_ctx);
    set_cors_headers(req);

    if (!self->hal_ || !self->config_) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "HAL not available");
        return ESP_FAIL;
    }

    // 1. bus (обов'язково) + driver (опційно) з query string
    char query[64] = {};
    char bus_id[16] = {};
    char drv_type[24] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing query: ?bus=ow_1");
        return ESP_FAIL;
    }
    if (httpd_query_key_value(query, "bus", bus_id, sizeof(bus_id)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'bus' parameter");
        return ESP_FAIL;
    }
    httpd_query_key_value(query, "driver", drv_type, sizeof(drv_type));  // optional

    const auto& bindings = self->config_->binding_table().bindings;

    // 2. Резолв discovery-функції через DriverRegistry — БЕЗ знання конкретного
    //    драйвера: явний ?driver= → тип із bindings цієї шини → єдина
    //    зареєстрована discovery. Драйвер вимкнено в menuconfig → його
    //    discovery просто не зареєстрована.
    modesp::DiscoveryFn scan = nullptr;
    const char* type = nullptr;
    if (drv_type[0]) {
        scan = modesp::DriverRegistry::find_discovery(drv_type);
        if (!scan) {
            httpd_resp_send_err(req, HTTPD_404_NOT_FOUND,
                                "Driver has no discovery (unknown or disabled)");
            return ESP_FAIL;
        }
        type = drv_type;
    }
    if (!scan) {
        for (const auto& b : bindings) {
            if (b.hardware_id.size() == strlen(bus_id) &&
                strncmp(b.hardware_id.c_str(), bus_id, b.hardware_id.size()) == 0) {
                scan = modesp::DriverRegistry::find_discovery(b.driver_type.c_str());
                if (scan) { type = b.driver_type.c_str(); break; }
            }
        }
    }
    if (!scan && modesp::DriverRegistry::discovery_count() == 1) {
        type = modesp::DriverRegistry::discovery_type_at(0);
        scan = modesp::DriverRegistry::discovery_fn_at(0);
    }
    if (!scan) {
        httpd_resp_send_err(req, HTTPD_501_METHOD_NOT_IMPLEMENTED,
                            "No discovery-capable driver registered");
        return ESP_OK;
    }

    // 3. Сканування через реєстр (драйвер сам резолвить шину в HAL)
    modesp::DiscoveredDevice devices[16];
    int found = scan(*self->hal_, bus_id, devices, 16);
    if (found < 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Bus not found");
        return ESP_FAIL;
    }
    size_t count = static_cast<size_t>(found);

    // 4. Побудувати JSON — для кожного пристрою: адреса, значення, role
    // Збираємо SKIP_ROM bindings на цій шині (без адреси)
    // Вони вже "займають" пристрій на шині, навіть без конкретної адреси
    struct SkipRomBinding { const char* role; bool matched; };
    SkipRomBinding skip_rom[4] = {};
    size_t skip_rom_count = 0;
    for (const auto& b : bindings) {
        if (b.address.empty() &&
            strcmp(b.driver_type.c_str(), type) == 0 &&
            b.hardware_id.size() == strlen(bus_id) &&
            strncmp(b.hardware_id.c_str(), bus_id, b.hardware_id.size()) == 0 &&
            skip_rom_count < 4) {
            skip_rom[skip_rom_count++] = { b.role.c_str(), false };
        }
    }

    char json[1536];
    int pos = snprintf(json, sizeof(json),
        "{\"bus\":\"%s\",\"driver\":\"%s\",\"devices\":[", bus_id, type);

    for (size_t i = 0; i < count; i++) {
        const char* addr_str = devices[i].address;

        // JSON-ключ "temperature" збережено для сумісності з webui;
        // семантично це generic value-превью від discovery-драйвера.
        float temp = devices[i].value;
        bool has_temp = devices[i].has_value;

        // Пошук в bindings: пристрій "зайнятий", якщо якась прив'язка посилається
        // на нього по address (OneWire ROM — per-role channel на спільній шині).
        const char* role = nullptr;
        for (const auto& b : bindings) {
            if (!b.address.empty() &&
                b.address.size() == strlen(addr_str) &&
                strncmp(b.address.c_str(), addr_str, b.address.size()) == 0) {
                role = b.role.c_str();
                break;
            }
        }

        // Якщо не знайдено по адресі — перевіряємо SKIP_ROM bindings на цій шині
        if (!role) {
            for (size_t s = 0; s < skip_rom_count; s++) {
                if (!skip_rom[s].matched) {
                    role = skip_rom[s].role;
                    skip_rom[s].matched = true;
                    break;
                }
            }
        }

        if (i > 0) pos += snprintf(json + pos, sizeof(json) - pos, ",");
        pos += snprintf(json + pos, sizeof(json) - pos,
            "{\"address\":\"%s\"", addr_str);
        if (has_temp) {
            pos += snprintf(json + pos, sizeof(json) - pos,
                ",\"temperature\":%.1f", static_cast<double>(temp));
        }
        if (devices[i].rssi != 0) {   // radios (BLE) report signal; wired buses leave 0
            pos += snprintf(json + pos, sizeof(json) - pos,
                ",\"rssi\":%d", static_cast<int>(devices[i].rssi));
        }
        if (role) {
            pos += snprintf(json + pos, sizeof(json) - pos,
                ",\"role\":\"%s\",\"status\":\"assigned\"", role);
        } else {
            pos += snprintf(json + pos, sizeof(json) - pos,
                ",\"role\":null,\"status\":\"new\"");
        }
        pos += snprintf(json + pos, sizeof(json) - pos, "}");

        if (pos >= (int)sizeof(json) - 128) break;  // Захист від overflow
    }
    pos += snprintf(json + pos, sizeof(json) - pos, "]}");

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, pos);
}

// ── Unified BLE Scan Handler ────────────────────────────────────
// ONE manual scan of every nearby BLE device our drivers understand. The transport
// identifies each device's driver TYPE by which registered decoder claimed its
// advertisement, and carries a short readings summary so the operator can tell two
// same-type sensors apart. Resolved generically via DriverRegistry ("ble" discovery
// registered by modesp_ble) — this layer never depends on the BLE component.
esp_err_t HttpService::handle_get_ble_scan(httpd_req_t* req) {
    if (!check_auth(req)) return ESP_OK;
    auto* self = static_cast<HttpService*>(req->user_ctx);
    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    modesp::DiscoveryFn scan = modesp::DriverRegistry::find_discovery("ble");
    if (!scan || !self->hal_) {
        httpd_resp_sendstr(req, "{\"devices\":[]}");   // BLE disabled — empty, not an error
        return ESP_OK;
    }

    modesp::DiscoveredDevice devices[16];
    int found = scan(*self->hal_, "", devices, 16);
    if (found < 0) found = 0;

    char json[2048];
    int pos = snprintf(json, sizeof(json), "{\"devices\":[");
    for (int i = 0; i < found; i++) {
        if (i > 0) pos += snprintf(json + pos, sizeof(json) - pos, ",");
        pos += snprintf(json + pos, sizeof(json) - pos,
            "{\"address\":\"%s\",\"type\":\"%s\",\"name\":\"%s\",\"summary\":\"%s\",\"rssi\":%d}",
            devices[i].address, devices[i].type, devices[i].name, devices[i].summary,
            static_cast<int>(devices[i].rssi));
        if (pos >= (int)sizeof(json) - 128) break;   // overflow guard
    }
    pos += snprintf(json + pos, sizeof(json) - pos, "]}");
    return httpd_resp_send(req, json, pos);
}

// ── Log API (/api/log*) ──────────────────────────────────────────────

esp_err_t HttpService::handle_get_log(httpd_req_t* req) {
    if (!check_auth(req)) return ESP_OK;
    set_cors_headers(req);

    const modesp::ILogSource* log = modesp::log_source::get();
    if (!log) {
        // Продукт без log-модуля в project.json — endpoint існує, джерела немає.
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Log source not available");
        return ESP_FAIL;
    }

    // Парсимо ?hours=24
    int hours = 24;
    char query[32];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[8];
        if (httpd_query_key_value(query, "hours", val, sizeof(val)) == ESP_OK) {
            int h = atoi(val);
            if (h > 0 && h <= 168) hours = h;
        }
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return log->serialize_log_chunked(req, hours);
}

esp_err_t HttpService::handle_get_log_summary(httpd_req_t* req) {
    if (!check_auth(req)) return ESP_OK;
    set_cors_headers(req);

    const modesp::ILogSource* log = modesp::log_source::get();
    if (!log) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Log source not available");
        return ESP_FAIL;
    }

    char buf[128];
    if (log->serialize_summary(buf, sizeof(buf))) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, buf, strlen(buf));
    }
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Serialize failed");
    return ESP_FAIL;
}

// ── Runtime log verbosity (plan item 11.4) ──────────────────────
//
// The panel ships at WARN with the INFO strings still in the binary, and a short allowlist
// of tags held at INFO because their INFO lines have each already settled a fault
// (modesp/log_policy.h). This pair is how that is moved without a reflash — which matters
// because the faults worth diagnosing are the ones nobody can reproduce on the bench.
//
// Nothing here is persisted. A reboot puts the shipping policy back, on purpose: a switch
// that survives a restart is a switch someone leaves on.

esp_err_t HttpService::handle_get_loglevel(httpd_req_t* req) {
    if (!check_auth(req)) return ESP_OK;
    set_cors_headers(req);

    char buf[1024];
    int o = snprintf(buf, sizeof(buf),
                     "{\"global\":\"%s\",\"shipping_default\":\"%s\",\"ceiling\":\"%s\",\"kept\":[",
                     log_policy::name_of(esp_log_level_get("*")),
                     log_policy::name_of(log_policy::shipping_default()),
                     log_policy::name_of(log_policy::ceiling()));
    bool first = true;
    for (const log_policy::Kept* k = log_policy::KEPT; k->tag; ++k) {
        const int n = snprintf(buf + o, sizeof(buf) - o, "%s{\"tag\":\"%s\",\"level\":\"%s\"}",
                               first ? "" : ",", k->tag,
                               log_policy::name_of(esp_log_level_get(k->tag)));
        if (n < 0 || (size_t)(o + n) >= sizeof(buf)) break;
        o += n;
        first = false;
    }
    if ((size_t)o + 3 < sizeof(buf)) o += snprintf(buf + o, sizeof(buf) - o, "]}");

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, o);
}

esp_err_t HttpService::handle_post_loglevel(httpd_req_t* req) {
    if (!check_auth(req)) return ESP_OK;

    // {"tag":"*","level":"info"} — small and fixed, so the body bound is small too.
    char body[160];
    const int len = req->content_len;
    if (len <= 0 || (size_t)len >= sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body must be a small JSON object");
        return ESP_FAIL;
    }
    int got = 0;
    int timeouts = 0;   // player-fork: consecutive stalls, bounded — see handle_post_ota()
    while (got < len) {
        const int r = httpd_req_recv(req, body + got, len - got);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) continue;
            httpd_resp_send_err(req, r == HTTPD_SOCK_ERR_TIMEOUT ? HTTPD_408_REQ_TIMEOUT
                                                                 : HTTPD_400_BAD_REQUEST,
                                "Receive error");
            return ESP_FAIL;
        }
        timeouts = 0;
        got += r;
    }
    body[got] = '\0';

    char tag[48]   = "*";
    char level[16] = "";
    jsmn_parser p;
    jsmntok_t tok[16];
    jsmn_init(&p);
    const int n = jsmn_parse(&p, body, got, tok, 16);
    if (n < 1 || tok[0].type != JSMN_OBJECT) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    for (int i = 1; i + 1 < n; i += 2) {
        if (tok[i].type != JSMN_STRING) break;
        const int klen = tok[i].end - tok[i].start;
        const int vlen = tok[i + 1].end - tok[i + 1].start;
        const char* k = body + tok[i].start;
        const char* v = body + tok[i + 1].start;
        if (klen == 3 && !strncmp(k, "tag", 3) && vlen > 0 && (size_t)vlen < sizeof(tag)) {
            memcpy(tag, v, vlen); tag[vlen] = '\0';
        } else if (klen == 5 && !strncmp(k, "level", 5) && vlen > 0 && (size_t)vlen < sizeof(level)) {
            memcpy(level, v, vlen); level[vlen] = '\0';
        }
    }

    if (!log_policy::set(tag, level)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "level must be none|error|warn|info and not above the build ceiling");
        return ESP_FAIL;
    }

    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    char out[128];
    const int o = snprintf(out, sizeof(out), "{\"ok\":true,\"tag\":\"%s\",\"level\":\"%s\"}",
                           tag, log_policy::name_of(esp_log_level_get(tag)));
    return httpd_resp_send(req, out, o);
}


// ── Static file handler ─────────────────────────────────────────

static const char* get_content_type(const char* path) {
    const char* ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    if (strcmp(ext, ".html") == 0) return "text/html";
    if (strcmp(ext, ".js") == 0)   return "application/javascript";
    if (strcmp(ext, ".css") == 0)  return "text/css";
    if (strcmp(ext, ".json") == 0) return "application/json";
    if (strcmp(ext, ".png") == 0)  return "image/png";
    if (strcmp(ext, ".ico") == 0)  return "image/x-icon";
    if (strcmp(ext, ".svg") == 0)  return "image/svg+xml";
    return "application/octet-stream";
}

esp_err_t HttpService::handle_static(httpd_req_t* req) {
    // Copy URI to local fixed-size buffer (silences -Wformat-truncation)
    char uri[52];
    strlcpy(uri, req->uri, sizeof(uri));
    if (strlen(req->uri) >= sizeof(uri)) {
        httpd_resp_send_404(req);
        return ESP_OK;
    }

    // AUDIT-040: захист від directory traversal (/../../../etc/passwd)
    if (strstr(uri, "..") != nullptr) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
        return ESP_OK;
    }

    // Build filesystem path
    char filepath[64];
    if (strcmp(uri, "/") == 0) {
        strlcpy(filepath, "/data/www/index.html", sizeof(filepath));
    } else {
        snprintf(filepath, sizeof(filepath), "/data/www%s", uri);
    }

    // Try .gz version first
    char gz_path[68];
    snprintf(gz_path, sizeof(gz_path), "%s.gz", filepath);

    struct stat st;
    bool use_gz = (stat(gz_path, &st) == 0);
    const char* actual_path = use_gz ? gz_path : filepath;

    FILE* f = fopen(actual_path, "r");
    if (!f) {
        // Fallback to index.html for SPA routing
        f = fopen("/data/www/index.html", "r");
        if (!f) {
            httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
            return ESP_FAIL;
        }
        snprintf(filepath, sizeof(filepath), "/data/www/index.html");
        use_gz = false;
    }

    httpd_resp_set_type(req, get_content_type(filepath));
    if (use_gz) {
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        // no-store: браузер завжди запитує свіжу версію (важливо при OTA/flash)
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    } else {
        httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    }

    // Stream file in chunks
    char buf[512];
    size_t read_bytes;
    while ((read_bytes = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, read_bytes) != ESP_OK) {
            fclose(f);
            httpd_resp_send_chunk(req, nullptr, 0);
            return ESP_FAIL;
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, nullptr, 0);
    return ESP_OK;
}

// ── Auth settings (GET + POST) ──────────────────────────────────

esp_err_t HttpService::handle_get_auth(httpd_req_t* req) {
    // GET /api/auth — публічний (без auth), ніколи не повертає пароль
    set_cors_headers(req);
    // player-fork: the name escaped, as POST unescapes it on the way in — a name holding a quote
    // or a backslash came back as broken JSON, and the Access page could not load at all.
    char user[net::json_escaped_capacity(sizeof(s_auth_user) - 1)];
    if (!net::json_escape(user, sizeof(user), s_auth_user)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Invalid username");
        return ESP_FAIL;
    }
    char buf[64 + sizeof(user)];
    snprintf(buf, sizeof(buf),
             "{\"enabled\":%s,\"username\":\"%s\"}",
             s_auth_enabled ? "true" : "false",
             user);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t HttpService::handle_post_auth(httpd_req_t* req) {
    if (!check_auth(req)) return ESP_OK;
    set_cors_headers(req);

    char buf[384];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    buf[len] = '\0';

    // Parse JSON: enabled, username, current_pass, new_pass, reset
    jsmn_parser parser;
    jsmntok_t tokens[16];
    jsmn_init(&parser);
    int r = jsmn_parse(&parser, buf, len, tokens, 16);
    if (r < 1 || tokens[0].type != JSMN_OBJECT) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    const char* current_pass = nullptr;
    const char* new_pass = nullptr;
    const char* username = nullptr;
    int enabled_val = -1;  // -1 = не вказано
    bool reset = false;

    for (int i = 1; i + 1 < r; i += 2) {
        if (tokens[i].type != JSMN_STRING) continue;
        buf[tokens[i].end] = '\0';
        buf[tokens[i + 1].end] = '\0';
        const char* key = buf + tokens[i].start;
        const char* val = buf + tokens[i + 1].start;

        // player-fork: as in handle_post_wifi — a password holding a quote or a backslash was
        // stored with the JSON's backslashes in it, while the browser signs in with the raw
        // characters, and every request after the change got 401. Unescaped in place.
        if ((strcmp(key, "current_pass") == 0 || strcmp(key, "new_pass") == 0 ||
             strcmp(key, "username") == 0) &&
            (tokens[i + 1].type != JSMN_STRING || !net::json_unescape(buf + tokens[i + 1].start))) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "username and passwords must be JSON strings");
            return ESP_FAIL;
        }
        if (strcmp(key, "current_pass") == 0) current_pass = val;
        else if (strcmp(key, "new_pass") == 0) new_pass = val;
        else if (strcmp(key, "username") == 0) username = val;
        else if (strcmp(key, "enabled") == 0) {
            enabled_val = (val[0] == 't') ? 1 : 0;
        }
        else if (strcmp(key, "reset") == 0) {
            reset = (val[0] == 't');
        }
    }

    // Скидання до заводських
    if (reset) {
        if (!current_pass || strcmp(current_pass, s_auth_pass) != 0) {
            httpd_resp_set_status(req, "403 Forbidden");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, "{\"error\":\"wrong_password\"}", HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }
        strncpy(s_auth_user, "admin", sizeof(s_auth_user) - 1);
        strncpy(s_auth_pass, "modesp", sizeof(s_auth_pass) - 1);
        s_auth_enabled = true;
        save_auth_to_nvs();
        ESP_LOGI(TAG, "Auth reset to factory defaults");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":true,\"reset\":true}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    // Зміна пароля — потрібен current_pass
    if (new_pass) {
        if (!current_pass || strcmp(current_pass, s_auth_pass) != 0) {
            httpd_resp_set_status(req, "403 Forbidden");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, "{\"error\":\"wrong_password\"}", HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }
        if (strlen(new_pass) < 4) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Password too short (min 4)");
            return ESP_FAIL;
        }
        // player-fork: refused, not cut to 63 behind an "ok" the browser then signs in against.
        if (strlen(new_pass) >= sizeof(s_auth_pass)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Password too long (max 63)");
            return ESP_FAIL;
        }
    }

    // player-fork: a name check_auth() could never match is refused (it splits user:pass at
    // the first ':'). Everything is checked before anything changes, so a refused request
    // leaves the stored credentials as they were.
    if (username && (username[0] == '\0' || strlen(username) >= sizeof(s_auth_user) ||
                     strchr(username, ':'))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Username must be 1-31 characters, no ':'");
        return ESP_FAIL;
    }

    if (new_pass) {
        strncpy(s_auth_pass, new_pass, sizeof(s_auth_pass) - 1);
        s_auth_pass[sizeof(s_auth_pass) - 1] = '\0';
    }

    // Зміна username
    if (username) {
        strncpy(s_auth_user, username, sizeof(s_auth_user) - 1);
        s_auth_user[sizeof(s_auth_user) - 1] = '\0';
    }

    // Toggle enabled
    if (enabled_val >= 0) {
        s_auth_enabled = (enabled_val == 1);
    }

    save_auth_to_nvs();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}
// ── Server setup ────────────────────────────────────────────────

void HttpService::register_api_handlers() {
    // GET endpoints
    struct {
        const char* uri;
        httpd_method_t method;
        esp_err_t (*handler)(httpd_req_t*);
    } endpoints[] = {
        {"/api/state",    HTTP_GET,  handle_get_state},
        {"/api/board",    HTTP_GET,  handle_get_board},
        {"/api/ui",       HTTP_GET,  handle_get_ui},
        {"/api/bindings", HTTP_GET,  handle_get_bindings},
        {"/api/bindings", HTTP_POST, handle_post_bindings},
        {"/api/devices",  HTTP_GET,  handle_get_devices},
        {"/api/devices",  HTTP_POST, handle_post_devices},
        {"/api/modules",  HTTP_GET,  handle_get_modules},
        {"/api/settings",   HTTP_POST, handle_post_settings},
        {"/api/wifi",       HTTP_POST, handle_post_wifi},
        {"/api/wifi/scan",  HTTP_GET,  handle_get_wifi_scan},
        {"/api/wifi/ap",    HTTP_GET,  handle_get_wifi_ap},
        {"/api/wifi/ap",    HTTP_POST, handle_post_wifi_ap},
        {"/api/restart",    HTTP_POST, handle_post_restart},
        {"/api/factory-reset", HTTP_POST, handle_post_factory_reset},
        {"/api/backup",  HTTP_GET,  handle_get_backup},
        {"/api/restore", HTTP_POST, handle_post_restore},
        {"/api/ota",        HTTP_GET,  handle_get_ota},
        {"/api/time",       HTTP_GET,  handle_get_time},
        {"/api/time",       HTTP_POST, handle_post_time},
        {"/api/onewire/scan", HTTP_GET, handle_get_ow_scan},   // legacy alias
        {"/api/drivers/scan", HTTP_GET, handle_get_ow_scan},   // generic: ?driver=<type>&bus=<hw>  (OneWire ROM discovery)
        {"/api/ble/scan",     HTTP_GET, handle_get_ble_scan},  // unified BLE scan: all identified nearby devices + readings
        {"/api/log",         HTTP_GET, handle_get_log},
        {"/api/log/summary", HTTP_GET, handle_get_log_summary},
        {"/api/auth", HTTP_GET,  handle_get_auth},
        {"/api/auth", HTTP_POST, handle_post_auth},
        // The panel ships quiet (plan item 11.4). This is how it is made to talk again
        // without a reflash, for a fault that only happens where the bench is not.
        {"/api/loglevel", HTTP_GET,  handle_get_loglevel},
        {"/api/loglevel", HTTP_POST, handle_post_loglevel},
    };

    // Реєструємо handlers + OPTIONS для CORS preflight
    // Використовуємо масив для відстеження URI, де OPTIONS вже зареєстрований
    const char* options_registered[24] = {};
    int options_count = 0;

    for (auto& ep : endpoints) {
        httpd_uri_t uri_handler = {};
        uri_handler.uri      = ep.uri;
        uri_handler.method   = ep.method;
        uri_handler.handler  = ep.handler;
        uri_handler.user_ctx = this;
        httpd_register_uri_handler(server_, &uri_handler);

        // OPTIONS для CORS — лише один раз per URI
        bool already = false;
        for (int j = 0; j < options_count; j++) {
            if (strcmp(options_registered[j], ep.uri) == 0) {
                already = true;
                break;
            }
        }
        if (!already && options_count < 16) {
            httpd_uri_t options = {};
            options.uri      = ep.uri;
            options.method   = HTTP_OPTIONS;
            options.handler  = handle_options;
            options.user_ctx = this;
            httpd_register_uri_handler(server_, &options);
            options_registered[options_count++] = ep.uri;
        }
    }

    // OTA POST registered separately (OPTIONS вже від GET /api/ota)
    httpd_uri_t ota_post = {};
    ota_post.uri      = "/api/ota";
    ota_post.method   = HTTP_POST;
    ota_post.handler  = handle_post_ota;
    ota_post.user_ctx = this;
    httpd_register_uri_handler(server_, &ota_post);

    ESP_LOGI(TAG, "API handlers registered (%d endpoints + OTA upload)",
             (int)(sizeof(endpoints) / sizeof(endpoints[0])));
}

// player-fork: httpd keeps its routes in a plain array that its own task walks for every request,
// with no lock. httpd_register_uri_handler() puts a slot in the array before filling it in, and
// httpd_unregister_uri_handler() frees one and shifts the rest down, so routes changed from
// another task while the server runs (http_routes::wildcard(): the DLNA renderer coming up) could
// hand a lookup a half-built or a freed slot, and every page asked for in between got a 404. The
// server's task is held still for the length of the change instead: a work item parks it between
// two requests until register_static_handler() puts the wildcard back, and a request that comes
// meanwhile waits for the routes to be whole.
static SemaphoreHandle_t s_parked = nullptr;   // given by the server's task once it holds still
static SemaphoreHandle_t s_resume = nullptr;   // given when the routes are whole again
static bool s_held = false;                    // the pair below runs on its one caller's task

static void hold_server(void*) {
    xSemaphoreGive(s_parked);
    xSemaphoreTake(s_resume, portMAX_DELAY);
}

void HttpService::unregister_static_handler() {
    if (!s_parked) s_parked = xSemaphoreCreateBinary();
    if (!s_resume) s_resume = xSemaphoreCreateBinary();
    s_held = s_parked && s_resume && httpd_queue_work(server_, hold_server, nullptr) == ESP_OK &&
             xSemaphoreTake(s_parked, portMAX_DELAY) == pdTRUE;
    if (!s_held) ESP_LOGW(TAG, "Server not held still: its routes change while it serves");
    httpd_unregister_uri_handler(server_, "/*", HTTP_GET);
    ESP_LOGI(TAG, "Static file handler removed (a late subsystem is registering routes)");
}

void HttpService::register_static_handler() {
    httpd_uri_t static_handler = {};
    static_handler.uri            = "/*";
    static_handler.method         = HTTP_GET;
    static_handler.handler        = handle_static;
    static_handler.user_ctx       = this;
    static_handler.is_websocket   = false;
    esp_err_t err = httpd_register_uri_handler(server_, &static_handler);
    if (s_held) {                  // player-fork: the routes are whole again; the server goes on
        s_held = false;
        xSemaphoreGive(s_resume);
    }
    if (err != ESP_OK) {
        // Without the wildcard every page is a 404 while the API still answers. It went
        // unnoticed once (W4, 12.10.2026: the handler table was full when DLNA put it back).
        ESP_LOGE(TAG, "Static file handler NOT registered (%s): no page will load - "
                      "raise max_uri_handlers", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "Static file handler registered (/* -> /data/www/)");
}

bool HttpService::start_server() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    // Handler budget: 27 API endpoints + 1 OTA POST + ≤16 OPTIONS (CORS pre-
    // flight) + 1 /ws + 2 MQTT + 1 static wildcard = 48 currently; 56 leaves
    // headroom for player endpoints. Each handler slot costs ~24 bytes SRAM.
    // 48 for the framework itself (27 API + OTA + <=16 CORS pre-flight + /ws + 2 MQTT +
    // the wildcard). UPnP adds seven more when the renderer is on — the device description
    // plus two per service — and 56 left exactly one spare, which is not headroom, it is a
    // coincidence waiting to be spent.
    // 72 was spent by W4 (12.10.2026): 67 at boot with the wildcard, and the renderer's seven
    // need 74. The last UPnP route and the wildcard's return failed, and every page was a 404.
    // 88 leaves 14 spare; a slot is one pointer until a handler takes it.
    config.max_uri_handlers = 88;
    // WebSocket and HTTP sessions together (Kconfig MODESP_HTTPD_MAX_OPEN_SOCKETS says why).
    // httpd_start() checks the 3 sockets httpd adds against lwIP at run time; this fails the build.
    static_assert(CONFIG_MODESP_HTTPD_MAX_OPEN_SOCKETS + 3 <= CONFIG_LWIP_MAX_SOCKETS,
                  "MODESP_HTTPD_MAX_OPEN_SOCKETS plus httpd's own 3 exceeds LWIP_MAX_SOCKETS");
    config.max_open_sockets = CONFIG_MODESP_HTTPD_MAX_OPEN_SOCKETS;
    config.stack_size = 8192;

    // WebSocket connections are long-lived. The default recv_wait_timeout
    // of 5s causes httpd to close idle WS clients (the recv() times out
    // and httpd interprets the next read as a malformed frame).
    // 30s stays well clear of the WS heartbeat (5 s, ws_service.h).
    config.recv_wait_timeout  = 30;
    config.send_wait_timeout  = 10;

    // When max_open_sockets is reached, close the oldest idle connection
    // instead of rejecting new ones. Essential when WS clients hold slots.
    config.lru_purge_enable   = true;

    esp_err_t err = httpd_start(&server_, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
        return false;
    }

    register_api_handlers();
    // NOTE: register_static_handler() must be called from main.cpp
    // AFTER all other handlers (WS, etc.) to avoid wildcard shadowing.

    ESP_LOGI(TAG, "HTTP server started on port %d", config.server_port);
    return true;
}

// ── Lifecycle ───────────────────────────────────────────────────

bool HttpService::on_init() {
    ESP_LOGI(TAG, "Starting HTTP service...");
    load_auth_from_nvs();
    return start_server();
}

void HttpService::on_update(uint32_t dt_ms) {
    (void)dt_ms;
    // httpd runs in its own task; only what must run on the main loop comes here.
    // player-fork: a join or a forget asked over POST /api/wifi (handle_post_wifi()).
    if (!wifi_) return;
    if (wifi_forget_.exchange(false, std::memory_order_acq_rel)) {
        wifi_->forget_credentials();
    }
    if (wifi_join_.load(std::memory_order_acquire)) {
        if (!wifi_->try_connect(wifi_join_ssid_, wifi_join_pass_)) {
            ESP_LOGW(TAG, "join from the web dropped: an attempt is already running");
        }
        memset(wifi_join_pass_, 0, sizeof(wifi_join_pass_));
        wifi_join_.store(false, std::memory_order_release);
    }
}

} // namespace modesp
