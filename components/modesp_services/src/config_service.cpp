/**
 * @file config_service.cpp
 * @brief ConfigService implementation — LittleFS mount + jsmn JSON parsing
 *
 * Boot-time only: the parse buffers live on the heap for the duration of load() and are
 * freed before it returns. After on_init() only the parsed structs remain in memory.
 */

#include "modesp/services/config_service.h"
#include "user_store.h"
#include "esp_log.h"
#include "esp_littlefs.h"
#include "esp_heap_caps.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

// jsmn as header-only with JSMN_STATIC
#define JSMN_STATIC
#include "jsmn.h"

static const char* TAG = "ConfigSvc";

// Phase 1.2 (з v4_max): парс-буфери на heap тільки на час load() (~12 KB: MAX_JSON_SIZE
// тексту + MAX_TOKENS × 16 Б). Раніше були static у BSS — ноукошлоштовно жерли RAM назавжди.
// Подальші reads ідуть через board_config_ / binding_table_ ETL containers.
//
// File-scope so the three parsers share one allocation. The callers are serialised by
// construction: load() runs once, on the main task at boot; the static loaders serve one
// httpd request at a time (httpd handles requests sequentially) and boot is long over.
namespace {
static char*      s_json_buf    = nullptr;
static jsmntok_t* s_json_tokens = nullptr;

static bool allocate_parse_buffers() {
    s_json_buf = static_cast<char*>(
        heap_caps_malloc(modesp::ConfigService::MAX_JSON_SIZE,
                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    s_json_tokens = static_cast<jsmntok_t*>(
        heap_caps_malloc(modesp::ConfigService::MAX_TOKENS * sizeof(jsmntok_t),
                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!s_json_buf || !s_json_tokens) {
        ESP_LOGE("ConfigSvc", "Parse buffer alloc failed (need ~12 KB free internal heap)");
        if (s_json_buf)    { heap_caps_free(s_json_buf);    s_json_buf = nullptr; }
        if (s_json_tokens) { heap_caps_free(s_json_tokens); s_json_tokens = nullptr; }
        return false;
    }
    return true;
}

static void free_parse_buffers() {
    if (s_json_buf)    { heap_caps_free(s_json_buf);    s_json_buf = nullptr; }
    if (s_json_tokens) { heap_caps_free(s_json_tokens); s_json_tokens = nullptr; }
}

/// Scope guard: the buffers exist exactly as long as one load runs.
struct ParseBuffers {
    bool ok;
    ParseBuffers() : ok(allocate_parse_buffers()) {}
    ~ParseBuffers() { free_parse_buffers(); }
};
}  // namespace

namespace modesp {

// ═══════════════════════════════════════════════════════════════
// Helper: compare jsmn token string with a key
// ═══════════════════════════════════════════════════════════════

static bool jsoneq(const char* json, const jsmntok_t* tok, const char* key) {
    if (tok->type != JSMN_STRING) return false;
    int len = tok->end - tok->start;
    return (int)strlen(key) == len &&
           strncmp(json + tok->start, key, len) == 0;
}

// Helper: extract token as null-terminated string into buffer
static void tok_to_str(const char* json, const jsmntok_t* tok, char* buf, size_t buf_size) {
    int len = tok->end - tok->start;
    if ((size_t)len >= buf_size) len = (int)buf_size - 1;
    strncpy(buf, json + tok->start, len);
    buf[len] = '\0';
}

// Helper: extract token as integer
static int tok_to_int(const char* json, const jsmntok_t* tok) {
    char buf[12];
    tok_to_str(json, tok, buf, sizeof(buf));
    return atoi(buf);
}

// Helper: extract token as bool
static bool tok_to_bool(const char* json, const jsmntok_t* tok) {
    return strncmp(json + tok->start, "true", 4) == 0;
}

// Helper: підрахунок кількості токенів у значенні (для пропуску об'єктів/масивів)
static int skip_token(const jsmntok_t* tokens, int idx, int ntokens) {
    if (idx >= ntokens) return idx;
    if (tokens[idx].type == JSMN_PRIMITIVE || tokens[idx].type == JSMN_STRING) {
        return idx + 1;
    }
    // Об'єкт або масив: пропустити всі дочірні токени
    int children = tokens[idx].size;
    int j = idx + 1;
    for (int c = 0; c < children; c++) {
        if (tokens[idx].type == JSMN_OBJECT) {
            j = skip_token(tokens, j, ntokens);     // key
            j = skip_token(tokens, j, ntokens);     // value
        } else {
            j = skip_token(tokens, j, ntokens);     // array element
        }
    }
    return j;
}

// jsmn's negative results, in words — "parse error: -3" told nobody that the file had
// simply been cut short.
static const char* jsmn_err_str(int err) {
    switch (err) {
        case JSMN_ERROR_NOMEM: return "too many tokens (raise ConfigService::MAX_TOKENS)";
        case JSMN_ERROR_INVAL: return "invalid character";
        case JSMN_ERROR_PART:  return "document ends early (truncated?)";
        default:               return "unknown";
    }
}

enum class ReadStatus { OK, MISSING, TOO_BIG, IO_ERROR };

// Read a JSON file into the parse buffer. A file that does not fit is refused here, with
// its limit in the log: fread() would otherwise stop at the buffer and jsmn would report -3
// on a document that only looks truncated — the failure that cost a flashed panel on
// 18.09.2026, when nothing in the log mentioned size. The limit is in docs/CONCEPT.md,
// "Control plane — ModESP"; the root CMakeLists.txt guard catches the same thing at build
// time, from the same constant.
static ReadStatus read_json_file(const char* path, char* buf, size_t& len) {
    FILE* f = fopen(path, "rb");
    if (!f) return ReadStatus::MISSING;
    len = fread(buf, 1, ConfigService::MAX_JSON_SIZE - 1, f);
    const bool more = (len == ConfigService::MAX_JSON_SIZE - 1) && (fgetc(f) != EOF);
    const bool err  = ferror(f) != 0;
    fclose(f);
    buf[len] = '\0';
    if (err) {
        ESP_LOGE(TAG, "%s: read error", path);
        return ReadStatus::IO_ERROR;
    }
    if (more) {
        ESP_LOGE(TAG, "%s: does not fit the %u-byte parse buffer — refusing to parse it truncated "
                      "(shorten labels, or raise ConfigService::MAX_JSON_SIZE)",
                 path, (unsigned)ConfigService::MAX_JSON_SIZE);
        return ReadStatus::TOO_BIG;
    }
    return ReadStatus::OK;
}

// Append to a fixed-capacity table, or fail the parse. A dropped entry used to be silent:
// the board came up with fewer pins than its file says and nothing said so.
template <typename Vec, typename T>
static bool push_or_fail(Vec& v, const T& item, const char* file, const char* section) {
    if (v.full()) {
        ESP_LOGE(TAG, "%s: more than %u '%s' entries — refusing to drop the rest silently",
                 file, (unsigned)v.capacity(), section);
        return false;
    }
    v.push_back(item);
    return true;
}

static bool join_path(char* out, size_t cap, const char* dir, const char* file) {
    const int n = snprintf(out, cap, "%s/%s", dir, file);
    if (n < 0 || (size_t)n >= cap) {
        ESP_LOGE(TAG, "config path too long: %s/%s", dir, file);
        return false;
    }
    return true;
}

static void reset(BoardConfig& b) {
    b.board_name.clear();
    b.board_version.clear();
    b.gpio_outputs.clear();
    b.onewire_buses.clear();
    b.gpio_inputs.clear();
    b.adc_channels.clear();
    b.encoders.clear();
    b.i2c_buses.clear();
    b.i2c_expanders.clear();
    b.expander_outputs.clear();
    b.expander_inputs.clear();
    b.i2c_displays.clear();
    b.dsi_displays.clear();
    b.sdmmc_slots.clear();
    b.i2c_touch.clear();
    b.uart_buses.clear();
    b.i2s_buses.clear();
    b.remote_devices.clear();
}

// ═══════════════════════════════════════════════════════════════
// Lifecycle
// ═══════════════════════════════════════════════════════════════

bool ConfigService::on_init() {
    if (!mount_littlefs()) {
        ESP_LOGE(TAG, "LittleFS mount failed");
        return false;
    }

    // The user store is brought up here, between the two mounts and the parse, because the
    // order is the whole of its contract: `data` has to be mounted (it is what the migration
    // copies out of) and the parse has to come after (devices.json is read from wherever the
    // store says it lives). begin() never fails — a panel with no `userdata` partition falls
    // back to `/data` and boots exactly as it did before.
    UserStore::begin();

    // devices.json is a runtime registry someone builds by subscribing devices, so it lives
    // in the store; board.json and bindings.json are build output and stay in the image.
    return load(UserStore::IMAGE_DIR, UserStore::dir());
}

bool ConfigService::load(const char* dir, const char* user_dir) {
    if (!user_dir) user_dir = dir;
    ParseBuffers buffers;
    if (!buffers.ok) return false;

    char path[160];   // "/data/board.json" on the device; a repo path in the host tests
    if (!join_path(path, sizeof(path), dir, "board.json")) return false;
    reset(board_config_);
    if (!parse_board_json(path, board_config_)) {
        ESP_LOGE(TAG, "Failed to parse %s", path);
        return false;
    }

    // Runtime device registry (devices.json) — user-subscribed remote devices (BLE by MAC
    // today). Merges into board_config_.remote_devices, runtime-wins-by-id, so a subscribed
    // device becomes indistinguishable from a factory board.json one. An absent file is the
    // normal case (board.json remains the seed); a malformed or overfull one is logged and
    // ignored — the runtime registry must not keep the board from booting.
    if (!join_path(path, sizeof(path), user_dir, "devices.json")) return false;
    if (!parse_devices_json(path, board_config_)) {
        ESP_LOGW(TAG, "%s present but unusable — ignoring runtime devices", path);
    }

    if (!join_path(path, sizeof(path), dir, "bindings.json")) return false;
    binding_table_.bindings.clear();
    if (!parse_bindings_json(path, binding_table_)) {
        ESP_LOGE(TAG, "Failed to parse %s", path);
        return false;
    }

    ESP_LOGI(TAG, "Board: %s v%s (%d gpio_out, %d ow, %d gpio_in, %d adc, %d i2c_bus, %d i2c_exp)",
             board_config_.board_name.c_str(),
             board_config_.board_version.c_str(),
             (int)board_config_.gpio_outputs.size(),
             (int)board_config_.onewire_buses.size(),
             (int)board_config_.gpio_inputs.size(),
             (int)board_config_.adc_channels.size(),
             (int)board_config_.i2c_buses.size(),
             (int)board_config_.i2c_expanders.size());
    ESP_LOGI(TAG, "Loaded %d bindings",
             (int)binding_table_.bindings.size());

    return true;
}

bool ConfigService::load_board_file(const char* path, BoardConfig& board) {
    ParseBuffers buffers;
    if (!buffers.ok) return false;
    reset(board);
    return parse_board_json(path, board);
}

bool ConfigService::load_bindings_file(const char* path, BindingTable& table) {
    ParseBuffers buffers;
    if (!buffers.ok) return false;
    table.bindings.clear();
    return parse_bindings_json(path, table);
}

// ═══════════════════════════════════════════════════════════════
// LittleFS mount
// ═══════════════════════════════════════════════════════════════

bool ConfigService::mount_littlefs() {
    esp_vfs_littlefs_conf_t conf = {};
    conf.base_path = "/data";
    conf.partition_label = "data";
    conf.format_if_mount_failed = false;
    conf.dont_mount = false;

    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS register failed: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "Mounted LittleFS (partition 'data')");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// Parse board.json
// ═══════════════════════════════════════════════════════════════

bool ConfigService::parse_board_json(const char* path, BoardConfig& board) {
    char* buf = s_json_buf;
    jsmntok_t* tokens = s_json_tokens;

    size_t len = 0;
    const ReadStatus rs = read_json_file(path, buf, len);
    if (rs == ReadStatus::MISSING) {
        ESP_LOGE(TAG, "Cannot open %s", path);
        return false;
    }
    if (rs != ReadStatus::OK) return false;   // already logged

    jsmn_parser parser;
    jsmn_init(&parser);
    int ntokens = jsmn_parse(&parser, buf, len, tokens, MAX_TOKENS);
    if (ntokens < 0) {
        ESP_LOGE(TAG, "%s: parse error %d — %s", path, ntokens, jsmn_err_str(ntokens));
        return false;
    }

    // Root must be object
    if (ntokens < 1 || tokens[0].type != JSMN_OBJECT) {
        ESP_LOGE(TAG, "board.json: root is not object");
        return false;
    }

    char tmp[48];   // fits remote_devices identity<40> (+NUL); matches parse_devices_json

    // Iterate top-level keys
    for (int i = 1; i < ntokens; ) {
        if (jsoneq(buf, &tokens[i], "board")) {
            tok_to_str(buf, &tokens[i + 1], tmp, sizeof(tmp));
            board.board_name = tmp;
            i += 2;
        } else if (jsoneq(buf, &tokens[i], "version")) {
            tok_to_str(buf, &tokens[i + 1], tmp, sizeof(tmp));
            board.board_version = tmp;
            i += 2;
        } else if (jsoneq(buf, &tokens[i], "gpio_outputs")) {
            // tokens[i+1] is the array
            if (tokens[i + 1].type != JSMN_ARRAY) {
                ESP_LOGE(TAG, "board.json: 'gpio_outputs' is not array");
                return false;
            }
            int arr_size = tokens[i + 1].size;
            int j = i + 2;  // first element of array

            for (int elem = 0; elem < arr_size; elem++) {
                // Each element is an object
                if (tokens[j].type != JSMN_OBJECT) {
                    ESP_LOGE(TAG, "board.json: gpio_output entry is not object");
                    return false;
                }
                int obj_keys = tokens[j].size;
                j++;  // move into object

                GpioOutputConfig cfg = {};
                for (int k = 0; k < obj_keys; k++) {
                    if (jsoneq(buf, &tokens[j], "id")) {
                        tok_to_str(buf, &tokens[j + 1], tmp, sizeof(tmp));
                        cfg.id = tmp;
                        j += 2;
                    } else if (jsoneq(buf, &tokens[j], "gpio")) {
                        cfg.gpio = (gpio_num_t)tok_to_int(buf, &tokens[j + 1]);
                        j += 2;
                    } else if (jsoneq(buf, &tokens[j], "active_high")) {
                        cfg.active_high = tok_to_bool(buf, &tokens[j + 1]);
                        j += 2;
                    } else {
                        j += 2;  // skip unknown key
                    }
                }

                if (!push_or_fail(board.gpio_outputs, cfg, "board.json", "gpio_outputs")) return false;
            }
            i = j;
        } else if (jsoneq(buf, &tokens[i], "onewire_buses")) {
            if (tokens[i + 1].type != JSMN_ARRAY) {
                ESP_LOGE(TAG, "board.json: 'onewire_buses' is not array");
                return false;
            }
            int arr_size = tokens[i + 1].size;
            int j = i + 2;

            for (int elem = 0; elem < arr_size; elem++) {
                if (tokens[j].type != JSMN_OBJECT) {
                    ESP_LOGE(TAG, "board.json: ow_bus entry is not object");
                    return false;
                }
                int obj_keys = tokens[j].size;
                j++;

                OneWireBusConfig cfg = {};
                for (int k = 0; k < obj_keys; k++) {
                    if (jsoneq(buf, &tokens[j], "id")) {
                        tok_to_str(buf, &tokens[j + 1], tmp, sizeof(tmp));
                        cfg.id = tmp;
                        j += 2;
                    } else if (jsoneq(buf, &tokens[j], "gpio")) {
                        cfg.gpio = (gpio_num_t)tok_to_int(buf, &tokens[j + 1]);
                        j += 2;
                    } else {
                        j += 2;
                    }
                }

                if (!push_or_fail(board.onewire_buses, cfg, "board.json", "onewire_buses")) return false;
            }
            i = j;
        } else if (jsoneq(buf, &tokens[i], "gpio_inputs")) {
            if (tokens[i + 1].type != JSMN_ARRAY) {
                ESP_LOGE(TAG, "board.json: 'gpio_inputs' is not array");
                return false;
            }
            int arr_size = tokens[i + 1].size;
            int j = i + 2;

            for (int elem = 0; elem < arr_size; elem++) {
                if (tokens[j].type != JSMN_OBJECT) {
                    ESP_LOGE(TAG, "board.json: gpio_input entry is not object");
                    return false;
                }
                int obj_keys = tokens[j].size;
                j++;

                GpioInputConfig cfg = {};
                cfg.pull_up = true;  // default
                for (int k = 0; k < obj_keys; k++) {
                    if (jsoneq(buf, &tokens[j], "id")) {
                        tok_to_str(buf, &tokens[j + 1], tmp, sizeof(tmp));
                        cfg.id = tmp;
                        j += 2;
                    } else if (jsoneq(buf, &tokens[j], "gpio")) {
                        cfg.gpio = (gpio_num_t)tok_to_int(buf, &tokens[j + 1]);
                        j += 2;
                    } else if (jsoneq(buf, &tokens[j], "pull")) {
                        tok_to_str(buf, &tokens[j + 1], tmp, sizeof(tmp));
                        cfg.pull_up = (strcmp(tmp, "up") == 0);
                        j += 2;
                    } else if (jsoneq(buf, &tokens[j], "invert")) {
                        cfg.invert = tok_to_bool(buf, &tokens[j + 1]);
                        j += 2;
                    } else {
                        j += 2;
                    }
                }

                if (!push_or_fail(board.gpio_inputs, cfg, "board.json", "gpio_inputs")) return false;
            }
            i = j;
        } else if (jsoneq(buf, &tokens[i], "encoders")) {
            if (tokens[i + 1].type != JSMN_ARRAY) {
                ESP_LOGE(TAG, "board.json: 'encoders' is not array");
                return false;
            }
            int arr_size = tokens[i + 1].size;
            int j = i + 2;

            for (int elem = 0; elem < arr_size; elem++) {
                if (tokens[j].type != JSMN_OBJECT) {
                    ESP_LOGE(TAG, "board.json: encoder entry is not object");
                    return false;
                }
                int obj_keys = tokens[j].size;
                j++;

                EncoderConfig cfg = {};
                cfg.pull_up = true;  // default
                for (int k = 0; k < obj_keys; k++) {
                    if (jsoneq(buf, &tokens[j], "id")) {
                        tok_to_str(buf, &tokens[j + 1], tmp, sizeof(tmp));
                        cfg.id = tmp;
                        j += 2;
                    } else if (jsoneq(buf, &tokens[j], "clk")) {
                        cfg.clk = (gpio_num_t)tok_to_int(buf, &tokens[j + 1]);
                        j += 2;
                    } else if (jsoneq(buf, &tokens[j], "dt")) {
                        cfg.dt = (gpio_num_t)tok_to_int(buf, &tokens[j + 1]);
                        j += 2;
                    } else if (jsoneq(buf, &tokens[j], "pull")) {
                        tok_to_str(buf, &tokens[j + 1], tmp, sizeof(tmp));
                        cfg.pull_up = (strcmp(tmp, "up") == 0);
                        j += 2;
                    } else {
                        j += 2;
                    }
                }

                if (!push_or_fail(board.encoders, cfg, "board.json", "encoders")) return false;
            }
            i = j;
        } else if (jsoneq(buf, &tokens[i], "adc_channels")) {
            if (tokens[i + 1].type != JSMN_ARRAY) {
                ESP_LOGE(TAG, "board.json: 'adc_channels' is not array");
                return false;
            }
            int arr_size = tokens[i + 1].size;
            int j = i + 2;

            for (int elem = 0; elem < arr_size; elem++) {
                if (tokens[j].type != JSMN_OBJECT) {
                    ESP_LOGE(TAG, "board.json: adc_channel entry is not object");
                    return false;
                }
                int obj_keys = tokens[j].size;
                j++;

                AdcChannelConfig cfg = {};
                cfg.atten = 11;  // default: 0-3.3V
                for (int k = 0; k < obj_keys; k++) {
                    if (jsoneq(buf, &tokens[j], "id")) {
                        tok_to_str(buf, &tokens[j + 1], tmp, sizeof(tmp));
                        cfg.id = tmp;
                        j += 2;
                    } else if (jsoneq(buf, &tokens[j], "gpio")) {
                        cfg.gpio = (gpio_num_t)tok_to_int(buf, &tokens[j + 1]);
                        j += 2;
                    } else if (jsoneq(buf, &tokens[j], "atten")) {
                        cfg.atten = (uint8_t)tok_to_int(buf, &tokens[j + 1]);
                        j += 2;
                    } else {
                        j += 2;
                    }
                }

                if (!push_or_fail(board.adc_channels, cfg, "board.json", "adc_channels")) return false;
            }
            i = j;
        } else if (jsoneq(buf, &tokens[i], "i2c_buses")) {
            if (tokens[i + 1].type != JSMN_ARRAY) {
                ESP_LOGE(TAG, "board.json: 'i2c_buses' is not array");
                return false;
            }
            int arr_size = tokens[i + 1].size;
            int j = i + 2;

            for (int elem = 0; elem < arr_size; elem++) {
                if (tokens[j].type != JSMN_OBJECT) {
                    ESP_LOGE(TAG, "board.json: i2c_bus entry is not object");
                    return false;
                }
                int obj_keys = tokens[j].size;
                j++;

                I2CBusConfig cfg = {};
                cfg.freq_hz = 100000;  // default 100kHz
                for (int k = 0; k < obj_keys; k++) {
                    if (jsoneq(buf, &tokens[j], "id")) {
                        tok_to_str(buf, &tokens[j + 1], tmp, sizeof(tmp));
                        cfg.id = tmp;
                    } else if (jsoneq(buf, &tokens[j], "sda")) {
                        cfg.sda = (gpio_num_t)tok_to_int(buf, &tokens[j + 1]);
                    } else if (jsoneq(buf, &tokens[j], "scl")) {
                        cfg.scl = (gpio_num_t)tok_to_int(buf, &tokens[j + 1]);
                    } else if (jsoneq(buf, &tokens[j], "freq_hz")) {
                        cfg.freq_hz = (uint32_t)tok_to_int(buf, &tokens[j + 1]);
                    }
                    j += 2;
                }
                if (!push_or_fail(board.i2c_buses, cfg, "board.json", "i2c_buses")) return false;
            }
            i = j;
        } else if (jsoneq(buf, &tokens[i], "uart_buses")) {
            if (tokens[i + 1].type != JSMN_ARRAY) {
                ESP_LOGE(TAG, "board.json: 'uart_buses' is not array");
                return false;
            }
            int arr_size = tokens[i + 1].size;
            int j = i + 2;

            for (int elem = 0; elem < arr_size; elem++) {
                if (tokens[j].type != JSMN_OBJECT) {
                    ESP_LOGE(TAG, "board.json: uart_bus entry is not object");
                    return false;
                }
                int obj_keys = tokens[j].size;
                j++;

                UartBusConfig cfg = {};
                for (int k = 0; k < obj_keys; k++) {
                    if (jsoneq(buf, &tokens[j], "id")) {
                        tok_to_str(buf, &tokens[j + 1], tmp, sizeof(tmp));
                        cfg.id = tmp;
                    } else if (jsoneq(buf, &tokens[j], "port")) {
                        cfg.port = tok_to_int(buf, &tokens[j + 1]);
                    } else if (jsoneq(buf, &tokens[j], "tx")) {
                        cfg.tx = (gpio_num_t)tok_to_int(buf, &tokens[j + 1]);
                    } else if (jsoneq(buf, &tokens[j], "rx")) {
                        cfg.rx = (gpio_num_t)tok_to_int(buf, &tokens[j + 1]);
                    } else if (jsoneq(buf, &tokens[j], "baud")) {
                        cfg.baud_rate = (uint32_t)tok_to_int(buf, &tokens[j + 1]);
                    } else if (jsoneq(buf, &tokens[j], "rx_buffer")) {
                        cfg.rx_buffer = (uint16_t)tok_to_int(buf, &tokens[j + 1]);
                    }
                    j += 2;
                }
                if (!push_or_fail(board.uart_buses, cfg, "board.json", "uart_buses")) return false;
            }
            i = j;
        } else if (jsoneq(buf, &tokens[i], "i2s_buses")) {
            if (tokens[i + 1].type != JSMN_ARRAY) {
                ESP_LOGE(TAG, "board.json: 'i2s_buses' is not array");
                return false;
            }
            int arr_size = tokens[i + 1].size;
            int j = i + 2;

            for (int elem = 0; elem < arr_size; elem++) {
                if (tokens[j].type != JSMN_OBJECT) {
                    ESP_LOGE(TAG, "board.json: i2s_bus entry is not object");
                    return false;
                }
                int obj_keys = tokens[j].size;
                j++;

                I2SBusConfig cfg = {};   // defaults: sd=-1, sample_rate_hz=16000
                for (int k = 0; k < obj_keys; k++) {
                    if (jsoneq(buf, &tokens[j], "id")) {
                        tok_to_str(buf, &tokens[j + 1], tmp, sizeof(tmp));
                        cfg.id = tmp;
                    } else if (jsoneq(buf, &tokens[j], "bclk")) {
                        cfg.bclk = (gpio_num_t)tok_to_int(buf, &tokens[j + 1]);
                    } else if (jsoneq(buf, &tokens[j], "ws")) {
                        cfg.ws = (gpio_num_t)tok_to_int(buf, &tokens[j + 1]);
                    } else if (jsoneq(buf, &tokens[j], "dout")) {
                        cfg.dout = (gpio_num_t)tok_to_int(buf, &tokens[j + 1]);
                    } else if (jsoneq(buf, &tokens[j], "sd")) {
                        cfg.sd = (gpio_num_t)tok_to_int(buf, &tokens[j + 1]);
                    } else if (jsoneq(buf, &tokens[j], "sample_rate_hz")) {
                        cfg.sample_rate_hz = (uint32_t)tok_to_int(buf, &tokens[j + 1]);
                    } else if (jsoneq(buf, &tokens[j], "port")) {
                        cfg.port = (int8_t)tok_to_int(buf, &tokens[j + 1]);
                    } else if (jsoneq(buf, &tokens[j], "role")) {
                        tok_to_str(buf, &tokens[j + 1], tmp, sizeof(tmp));
                        cfg.master = (strcmp(tmp, "slave") != 0);
                    } else if (jsoneq(buf, &tokens[j], "mclk")) {
                        cfg.mclk = (gpio_num_t)tok_to_int(buf, &tokens[j + 1]);
                    } else if (jsoneq(buf, &tokens[j], "din")) {
                        cfg.din = (gpio_num_t)tok_to_int(buf, &tokens[j + 1]);
                    } else if (jsoneq(buf, &tokens[j], "slot_bits")) {
                        cfg.slot_bits = (uint8_t)tok_to_int(buf, &tokens[j + 1]);
                    } else if (jsoneq(buf, &tokens[j], "mclk_multiple")) {
                        cfg.mclk_multiple = (uint16_t)tok_to_int(buf, &tokens[j + 1]);
                    }
                    j += 2;
                }
                if (!push_or_fail(board.i2s_buses, cfg, "board.json", "i2s_buses")) return false;
            }
            i = j;
        } else if (jsoneq(buf, &tokens[i], "i2c_expanders")) {
            if (tokens[i + 1].type != JSMN_ARRAY) {
                ESP_LOGE(TAG, "board.json: 'i2c_expanders' is not array");
                return false;
            }
            int arr_size = tokens[i + 1].size;
            int j = i + 2;

            for (int elem = 0; elem < arr_size; elem++) {
                if (tokens[j].type != JSMN_OBJECT) {
                    ESP_LOGE(TAG, "board.json: i2c_expander entry is not object");
                    return false;
                }
                int obj_keys = tokens[j].size;
                j++;

                I2CExpanderConfig cfg = {};
                cfg.pin_count = 8;  // default PCF8574
                for (int k = 0; k < obj_keys; k++) {
                    if (jsoneq(buf, &tokens[j], "id")) {
                        tok_to_str(buf, &tokens[j + 1], tmp, sizeof(tmp));
                        cfg.id = tmp;
                    } else if (jsoneq(buf, &tokens[j], "bus")) {
                        tok_to_str(buf, &tokens[j + 1], tmp, sizeof(tmp));
                        cfg.bus_id = tmp;
                    } else if (jsoneq(buf, &tokens[j], "chip")) {
                        tok_to_str(buf, &tokens[j + 1], tmp, sizeof(tmp));
                        cfg.chip = tmp;
                    } else if (jsoneq(buf, &tokens[j], "address")) {
                        tok_to_str(buf, &tokens[j + 1], tmp, sizeof(tmp));
                        cfg.address = (uint8_t)strtol(tmp, nullptr, 16);
                    } else if (jsoneq(buf, &tokens[j], "pins")) {
                        cfg.pin_count = (uint8_t)tok_to_int(buf, &tokens[j + 1]);
                    }
                    j += 2;
                }
                if (!push_or_fail(board.i2c_expanders, cfg, "board.json", "i2c_expanders")) return false;
            }
            i = j;
        } else if (jsoneq(buf, &tokens[i], "expander_outputs")) {
            if (tokens[i + 1].type != JSMN_ARRAY) {
                ESP_LOGE(TAG, "board.json: 'expander_outputs' is not array");
                return false;
            }
            int arr_size = tokens[i + 1].size;
            int j = i + 2;

            for (int elem = 0; elem < arr_size; elem++) {
                if (tokens[j].type != JSMN_OBJECT) {
                    ESP_LOGE(TAG, "board.json: expander_output entry is not object");
                    return false;
                }
                int obj_keys = tokens[j].size;
                j++;

                I2CExpanderOutputConfig cfg = {};
                cfg.active_high = false;  // default: active-LOW (KC868-A6)
                for (int k = 0; k < obj_keys; k++) {
                    if (jsoneq(buf, &tokens[j], "id")) {
                        tok_to_str(buf, &tokens[j + 1], tmp, sizeof(tmp));
                        cfg.id = tmp;
                    } else if (jsoneq(buf, &tokens[j], "expander")) {
                        tok_to_str(buf, &tokens[j + 1], tmp, sizeof(tmp));
                        cfg.expander_id = tmp;
                    } else if (jsoneq(buf, &tokens[j], "pin")) {
                        cfg.pin = (uint8_t)tok_to_int(buf, &tokens[j + 1]);
                    } else if (jsoneq(buf, &tokens[j], "active_high")) {
                        cfg.active_high = tok_to_bool(buf, &tokens[j + 1]);
                    }
                    j += 2;
                }
                if (!push_or_fail(board.expander_outputs, cfg, "board.json", "expander_outputs")) return false;
            }
            i = j;
        } else if (jsoneq(buf, &tokens[i], "expander_inputs")) {
            if (tokens[i + 1].type != JSMN_ARRAY) {
                ESP_LOGE(TAG, "board.json: 'expander_inputs' is not array");
                return false;
            }
            int arr_size = tokens[i + 1].size;
            int j = i + 2;

            for (int elem = 0; elem < arr_size; elem++) {
                if (tokens[j].type != JSMN_OBJECT) {
                    ESP_LOGE(TAG, "board.json: expander_input entry is not object");
                    return false;
                }
                int obj_keys = tokens[j].size;
                j++;

                I2CExpanderInputConfig cfg = {};
                cfg.invert = false;  // default
                for (int k = 0; k < obj_keys; k++) {
                    if (jsoneq(buf, &tokens[j], "id")) {
                        tok_to_str(buf, &tokens[j + 1], tmp, sizeof(tmp));
                        cfg.id = tmp;
                    } else if (jsoneq(buf, &tokens[j], "expander")) {
                        tok_to_str(buf, &tokens[j + 1], tmp, sizeof(tmp));
                        cfg.expander_id = tmp;
                    } else if (jsoneq(buf, &tokens[j], "pin")) {
                        cfg.pin = (uint8_t)tok_to_int(buf, &tokens[j + 1]);
                    } else if (jsoneq(buf, &tokens[j], "invert")) {
                        cfg.invert = tok_to_bool(buf, &tokens[j + 1]);
                    }
                    j += 2;
                }
                if (!push_or_fail(board.expander_inputs, cfg, "board.json", "expander_inputs")) return false;
            }
            i = j;
        } else if (jsoneq(buf, &tokens[i], "i2c_displays")) {
            if (tokens[i + 1].type != JSMN_ARRAY) {
                ESP_LOGE(TAG, "board.json: 'i2c_displays' is not array");
                return false;
            }
            int arr_size = tokens[i + 1].size;
            int j = i + 2;

            for (int elem = 0; elem < arr_size; elem++) {
                if (tokens[j].type != JSMN_OBJECT) {
                    ESP_LOGE(TAG, "board.json: i2c_display entry is not object");
                    return false;
                }
                int obj_keys = tokens[j].size;
                j++;

                I2CDisplayConfig cfg = {};
                cfg.cols = 20;  // sane defaults — конкретику задає board.json
                cfg.rows = 10;
                for (int k = 0; k < obj_keys; k++) {
                    if (jsoneq(buf, &tokens[j], "id")) {
                        tok_to_str(buf, &tokens[j + 1], tmp, sizeof(tmp));
                        cfg.id = tmp;
                    } else if (jsoneq(buf, &tokens[j], "bus")) {
                        tok_to_str(buf, &tokens[j + 1], tmp, sizeof(tmp));
                        cfg.bus_id = tmp;
                    } else if (jsoneq(buf, &tokens[j], "chip")) {
                        tok_to_str(buf, &tokens[j + 1], tmp, sizeof(tmp));
                        cfg.chip = tmp;
                    } else if (jsoneq(buf, &tokens[j], "cols")) {
                        cfg.cols = (uint8_t)tok_to_int(buf, &tokens[j + 1]);
                    } else if (jsoneq(buf, &tokens[j], "rows")) {
                        cfg.rows = (uint8_t)tok_to_int(buf, &tokens[j + 1]);
                    } else if (jsoneq(buf, &tokens[j], "cal_x")) {
                        cfg.cal_x = (int8_t)tok_to_int(buf, &tokens[j + 1]);   // overscan-зсув OSD
                    } else if (jsoneq(buf, &tokens[j], "cal_y")) {
                        cfg.cal_y = (int8_t)tok_to_int(buf, &tokens[j + 1]);
                    }
                    j += 2;
                }
                if (!push_or_fail(board.i2c_displays, cfg, "board.json", "i2c_displays")) return false;
            }
            i = j;
        } else if (jsoneq(buf, &tokens[i], "dsi_displays")) {
            if (tokens[i + 1].type != JSMN_ARRAY) {
                ESP_LOGE(TAG, "board.json: 'dsi_displays' is not array");
                return false;
            }
            int arr_size = tokens[i + 1].size;
            int j = i + 2;

            for (int elem = 0; elem < arr_size; elem++) {
                if (tokens[j].type != JSMN_OBJECT) {
                    ESP_LOGE(TAG, "board.json: dsi_display entry is not object");
                    return false;
                }
                int obj_keys = tokens[j].size;
                j++;

                // Every timing is a panel fact and has no default any more (hal_types.h):
                // a missing one is refused by name rather than silently filled in with the
                // panel this firmware was first written for. `seen` is a bit per required
                // key, in the order of REQUIRED below.
                DsiDisplayConfig cfg;
                uint32_t seen = 0;
                static const char* const REQUIRED[] = {
                    "lanes", "lane_mbps", "dpi_mhz", "h_res", "v_res",
                    "hsync", "hbp", "hfp", "vsync", "vbp", "vfp", "color_bits",
                };
                constexpr uint32_t ALL_REQUIRED = (1u << 12) - 1u;
                for (int k = 0; k < obj_keys; k++) {
                    for (uint32_t r = 0; r < sizeof(REQUIRED) / sizeof(REQUIRED[0]); r++) {
                        if (jsoneq(buf, &tokens[j], REQUIRED[r])) { seen |= (1u << r); break; }
                    }
                    if (jsoneq(buf, &tokens[j], "id")) {
                        tok_to_str(buf, &tokens[j + 1], tmp, sizeof(tmp));
                        cfg.id = tmp;
                    } else if (jsoneq(buf, &tokens[j], "chip")) {
                        tok_to_str(buf, &tokens[j + 1], tmp, sizeof(tmp));
                        cfg.chip = tmp;
                    } else if (jsoneq(buf, &tokens[j], "lanes")) {
                        cfg.lanes = (uint8_t)tok_to_int(buf, &tokens[j + 1]);
                    } else if (jsoneq(buf, &tokens[j], "lane_mbps")) {
                        cfg.lane_mbps = (uint16_t)tok_to_int(buf, &tokens[j + 1]);
                    } else if (jsoneq(buf, &tokens[j], "dpi_mhz")) {
                        cfg.dpi_mhz = (uint16_t)tok_to_int(buf, &tokens[j + 1]);
                    } else if (jsoneq(buf, &tokens[j], "h_res")) {
                        cfg.h_res = (uint16_t)tok_to_int(buf, &tokens[j + 1]);
                    } else if (jsoneq(buf, &tokens[j], "v_res")) {
                        cfg.v_res = (uint16_t)tok_to_int(buf, &tokens[j + 1]);
                    } else if (jsoneq(buf, &tokens[j], "hsync")) {
                        cfg.hsync = (uint16_t)tok_to_int(buf, &tokens[j + 1]);
                    } else if (jsoneq(buf, &tokens[j], "hbp")) {
                        cfg.hbp = (uint16_t)tok_to_int(buf, &tokens[j + 1]);
                    } else if (jsoneq(buf, &tokens[j], "hfp")) {
                        cfg.hfp = (uint16_t)tok_to_int(buf, &tokens[j + 1]);
                    } else if (jsoneq(buf, &tokens[j], "vsync")) {
                        cfg.vsync = (uint16_t)tok_to_int(buf, &tokens[j + 1]);
                    } else if (jsoneq(buf, &tokens[j], "vbp")) {
                        cfg.vbp = (uint16_t)tok_to_int(buf, &tokens[j + 1]);
                    } else if (jsoneq(buf, &tokens[j], "vfp")) {
                        cfg.vfp = (uint16_t)tok_to_int(buf, &tokens[j + 1]);
                    } else if (jsoneq(buf, &tokens[j], "color_bits")) {
                        cfg.color_bits = (uint8_t)tok_to_int(buf, &tokens[j + 1]);
                    } else if (jsoneq(buf, &tokens[j], "reset_gpio")) {
                        cfg.reset_gpio = (gpio_num_t)tok_to_int(buf, &tokens[j + 1]);
                    } else if (jsoneq(buf, &tokens[j], "backlight_gpio")) {
                        cfg.backlight_gpio = (gpio_num_t)tok_to_int(buf, &tokens[j + 1]);
                    } else if (jsoneq(buf, &tokens[j], "backlight_active_high")) {
                        cfg.backlight_active_high = tok_to_bool(buf, &tokens[j + 1]);
                    } else if (jsoneq(buf, &tokens[j], "backlight_pwm_hz")) {
                        cfg.backlight_pwm_hz = (uint32_t)tok_to_int(buf, &tokens[j + 1]);
                    } else if (jsoneq(buf, &tokens[j], "ldo_chan")) {
                        cfg.ldo_chan = (int8_t)tok_to_int(buf, &tokens[j + 1]);
                    } else if (jsoneq(buf, &tokens[j], "ldo_mv")) {
                        cfg.ldo_mv = (uint16_t)tok_to_int(buf, &tokens[j + 1]);
                    } else if (jsoneq(buf, &tokens[j], "rotation")) {
                        cfg.rotation = (uint16_t)tok_to_int(buf, &tokens[j + 1]);
                    }
                    j += 2;
                }
                if (seen != ALL_REQUIRED) {
                    for (uint32_t r = 0; r < sizeof(REQUIRED) / sizeof(REQUIRED[0]); r++) {
                        if (!(seen & (1u << r)))
                            ESP_LOGE(TAG, "board.json: dsi_display '%s' has no '%s'",
                                     cfg.id.c_str(), REQUIRED[r]);
                    }
                    return false;
                }
                if (cfg.ldo_chan >= 0 && cfg.ldo_mv == 0) {
                    ESP_LOGE(TAG, "board.json: dsi_display '%s' sets ldo_chan but no ldo_mv",
                             cfg.id.c_str());
                    return false;
                }
                if (!push_or_fail(board.dsi_displays, cfg, "board.json", "dsi_displays")) return false;
            }
            i = j;
        } else if (jsoneq(buf, &tokens[i], "i2c_touch")) {
            if (tokens[i + 1].type != JSMN_ARRAY) {
                ESP_LOGE(TAG, "board.json: 'i2c_touch' is not array");
                return false;
            }
            int arr_size = tokens[i + 1].size;
            int j = i + 2;

            for (int elem = 0; elem < arr_size; elem++) {
                if (tokens[j].type != JSMN_OBJECT) {
                    ESP_LOGE(TAG, "board.json: i2c_touch entry is not object");
                    return false;
                }
                int obj_keys = tokens[j].size;
                j++;

                I2CTouchConfig cfg;   // defaults: GT911 @0x5D, INT/RST not wired
                for (int k = 0; k < obj_keys; k++) {
                    if (jsoneq(buf, &tokens[j], "id")) {
                        tok_to_str(buf, &tokens[j + 1], tmp, sizeof(tmp));
                        cfg.id = tmp;
                    } else if (jsoneq(buf, &tokens[j], "bus")) {
                        tok_to_str(buf, &tokens[j + 1], tmp, sizeof(tmp));
                        cfg.bus_id = tmp;
                    } else if (jsoneq(buf, &tokens[j], "chip")) {
                        tok_to_str(buf, &tokens[j + 1], tmp, sizeof(tmp));
                        cfg.chip = tmp;
                    } else if (jsoneq(buf, &tokens[j], "address")) {
                        tok_to_str(buf, &tokens[j + 1], tmp, sizeof(tmp));
                        cfg.address = (uint8_t)strtol(tmp, nullptr, 16);
                    } else if (jsoneq(buf, &tokens[j], "int_gpio")) {
                        cfg.int_gpio = (gpio_num_t)tok_to_int(buf, &tokens[j + 1]);
                    } else if (jsoneq(buf, &tokens[j], "rst_gpio")) {
                        cfg.rst_gpio = (gpio_num_t)tok_to_int(buf, &tokens[j + 1]);
                    } else if (jsoneq(buf, &tokens[j], "x_max")) {
                        cfg.x_max = (uint16_t)tok_to_int(buf, &tokens[j + 1]);
                    } else if (jsoneq(buf, &tokens[j], "y_max")) {
                        cfg.y_max = (uint16_t)tok_to_int(buf, &tokens[j + 1]);
                    } else if (jsoneq(buf, &tokens[j], "swap_xy")) {
                        cfg.swap_xy = tok_to_bool(buf, &tokens[j + 1]);
                    } else if (jsoneq(buf, &tokens[j], "mirror_x")) {
                        cfg.mirror_x = tok_to_bool(buf, &tokens[j + 1]);
                    } else if (jsoneq(buf, &tokens[j], "mirror_y")) {
                        cfg.mirror_y = tok_to_bool(buf, &tokens[j + 1]);
                    }
                    j += 2;
                }
                // The GT911 answers at 0x5D or 0x14 depending on how ADDR is strapped, and
                // that is a board fact. It used to be a default in hal_types.h, i.e. one
                // controller's wiring standing in for every board's.
                if (cfg.address == 0) {
                    ESP_LOGE(TAG, "board.json: i2c_touch '%s' has no 'address'", cfg.id.c_str());
                    return false;
                }
                if (!push_or_fail(board.i2c_touch, cfg, "board.json", "i2c_touch")) return false;
            }
            i = j;
        } else if (jsoneq(buf, &tokens[i], "sdmmc_slots")) {
            if (tokens[i + 1].type != JSMN_ARRAY) {
                ESP_LOGE(TAG, "board.json: 'sdmmc_slots' is not array");
                return false;
            }
            int arr_size = tokens[i + 1].size;
            int j = i + 2;

            for (int elem = 0; elem < arr_size; elem++) {
                if (tokens[j].type != JSMN_OBJECT) {
                    ESP_LOGE(TAG, "board.json: sdmmc_slots entry is not object");
                    return false;
                }
                int obj_keys = tokens[j].size;
                j++;

                SdmmcSlotConfig cfg;
                for (int k = 0; k < obj_keys; k++) {
                    if (jsoneq(buf, &tokens[j], "id")) {
                        tok_to_str(buf, &tokens[j + 1], tmp, sizeof(tmp));
                        cfg.id = tmp;
                    } else if (jsoneq(buf, &tokens[j], "mount_point")) {
                        tok_to_str(buf, &tokens[j + 1], tmp, sizeof(tmp));
                        cfg.mount_point = tmp;
                    } else if (jsoneq(buf, &tokens[j], "slot")) {
                        cfg.slot = (uint8_t)tok_to_int(buf, &tokens[j + 1]);
                    } else if (jsoneq(buf, &tokens[j], "width")) {
                        cfg.width = (uint8_t)tok_to_int(buf, &tokens[j + 1]);
                    } else if (jsoneq(buf, &tokens[j], "clk")) {
                        cfg.clk = (gpio_num_t)tok_to_int(buf, &tokens[j + 1]);
                    } else if (jsoneq(buf, &tokens[j], "cmd")) {
                        cfg.cmd = (gpio_num_t)tok_to_int(buf, &tokens[j + 1]);
                    } else if (jsoneq(buf, &tokens[j], "d0")) {
                        cfg.d0 = (gpio_num_t)tok_to_int(buf, &tokens[j + 1]);
                    } else if (jsoneq(buf, &tokens[j], "d1")) {
                        cfg.d1 = (gpio_num_t)tok_to_int(buf, &tokens[j + 1]);
                    } else if (jsoneq(buf, &tokens[j], "d2")) {
                        cfg.d2 = (gpio_num_t)tok_to_int(buf, &tokens[j + 1]);
                    } else if (jsoneq(buf, &tokens[j], "d3")) {
                        cfg.d3 = (gpio_num_t)tok_to_int(buf, &tokens[j + 1]);
                    } else if (jsoneq(buf, &tokens[j], "cd")) {
                        cfg.cd = (gpio_num_t)tok_to_int(buf, &tokens[j + 1]);
                    } else if (jsoneq(buf, &tokens[j], "freq_khz")) {
                        cfg.freq_khz = (uint32_t)tok_to_int(buf, &tokens[j + 1]);
                    } else if (jsoneq(buf, &tokens[j], "ldo_chan")) {
                        cfg.ldo_chan = (uint8_t)tok_to_int(buf, &tokens[j + 1]);
                    } else if (jsoneq(buf, &tokens[j], "ldo_mv")) {
                        cfg.ldo_mv = (uint16_t)tok_to_int(buf, &tokens[j + 1]);
                    }
                    j += 2;
                }
                if (!push_or_fail(board.sdmmc_slots, cfg, "board.json", "sdmmc_slots")) return false;
            }
            i = j;
        } else if (jsoneq(buf, &tokens[i], "remote_devices") ||
                   jsoneq(buf, &tokens[i], "ble_devices")) {   // "ble_devices" = legacy key alias
            if (tokens[i + 1].type != JSMN_ARRAY) {
                ESP_LOGE(TAG, "board.json: 'remote_devices' is not array");
                return false;
            }
            int arr_size = tokens[i + 1].size;
            int j = i + 2;

            for (int elem = 0; elem < arr_size; elem++) {
                if (tokens[j].type != JSMN_OBJECT) {
                    ESP_LOGE(TAG, "board.json: remote_device entry is not object");
                    return false;
                }
                int obj_keys = tokens[j].size;
                j++;

                RemoteDeviceConfig cfg = {};   // transport defaults "ble"
                for (int k = 0; k < obj_keys; k++) {
                    if (jsoneq(buf, &tokens[j], "id")) {
                        tok_to_str(buf, &tokens[j + 1], tmp, sizeof(tmp));
                        cfg.id = tmp;
                    } else if (jsoneq(buf, &tokens[j], "identity") ||
                               jsoneq(buf, &tokens[j], "mac")) {   // "mac" = legacy field alias for identity
                        tok_to_str(buf, &tokens[j + 1], tmp, sizeof(tmp));
                        cfg.identity = tmp;
                    } else if (jsoneq(buf, &tokens[j], "name")) {
                        tok_to_str(buf, &tokens[j + 1], tmp, sizeof(tmp));
                        cfg.name = tmp;
                    } else if (jsoneq(buf, &tokens[j], "transport")) {
                        tok_to_str(buf, &tokens[j + 1], tmp, sizeof(tmp));
                        cfg.transport = tmp;
                    }
                    j += 2;
                }
                if (!push_or_fail(board.remote_devices, cfg, "board.json", "remote_devices")) return false;
            }
            i = j;
        } else {
            // Пропустити невідомий ключ + значення (масиви/об'єкти коректно)
            i++;  // skip key
            i = skip_token(tokens, i, ntokens);  // skip value
        }
    }

    return true;
}

// ═══════════════════════════════════════════════════════════════
// Parse devices.json (runtime device registry)
// ═══════════════════════════════════════════════════════════════

// Runtime-writable sibling of board.json remote_devices. Rows: {id, transport, identity, name, label}
// (legacy aliases: hw_type→transport, mac→identity). Merged into board.remote_devices with
// runtime-wins-by-id, so a subscribed device resolves via find_remote_device exactly like a factory
// one — the role layer never sees an identity. Any transport is honoured (only "ble" is wired to a
// transport driver today; others sit inert until their driver lands). Absent file → no runtime
// devices (returns true, non-fatal).
bool ConfigService::parse_devices_json(const char* path, BoardConfig& board) {
    char* buf = s_json_buf;
    jsmntok_t* tokens = s_json_tokens;

    size_t len = 0;
    const ReadStatus rs = read_json_file(path, buf, len);
    if (rs == ReadStatus::MISSING) {
        return true;   // no runtime registry — board.json remote_devices are the seed
    }
    if (rs != ReadStatus::OK) return false;   // already logged

    jsmn_parser parser;
    jsmn_init(&parser);
    int ntokens = jsmn_parse(&parser, buf, len, tokens, MAX_TOKENS);
    if (ntokens < 0) {
        ESP_LOGE(TAG, "%s: parse error %d — %s", path, ntokens, jsmn_err_str(ntokens));
        return false;
    }
    if (ntokens < 1 || tokens[0].type != JSMN_OBJECT) {
        ESP_LOGE(TAG, "devices.json: root is not object");
        return false;
    }

    char tmp[48];   // fits identity<40> (+NUL); MAC is 17

    // jsmn is not strict: a member with no ':' and no value ({"devices"}, {"id":"a","mac"})
    // still counts towards its object's size, so the sizes below can promise more tokens than
    // were parsed — and past ntokens is the previous file's tokens. As in parse_bindings_json,
    // every step is checked against ntokens and a key must be a string owning one value. The
    // file is user data (POST /api/devices, a restored backup); where it breaks, the walk stops
    // and keeps the rows it has already read.
    auto bad_key = [&](int at) {
        return at + 1 >= ntokens || tokens[at].type != JSMN_STRING || tokens[at].size != 1;
    };

    for (int i = 1; i < ntokens; ) {
        if (jsoneq(buf, &tokens[i], "devices")) {
            if (i + 1 >= ntokens) {
                ESP_LOGE(TAG, "%s: 'devices' has no value", path);
                return false;
            }
            if (tokens[i + 1].type != JSMN_ARRAY) {
                ESP_LOGE(TAG, "devices.json: 'devices' is not array");
                return false;
            }
            int arr_size = tokens[i + 1].size;
            int j = i + 2;

            for (int elem = 0; elem < arr_size; elem++) {
                if (j >= ntokens) {
                    ESP_LOGW(TAG, "%s: 'devices' ends early — the rest ignored", path);
                    break;
                }
                // devices.json is runtime-written — a malformed element must not abort the
                // whole registry (that would drop already-parsed valid rows while the caller
                // logs "ignoring"). Skip the bad element and continue.
                if (tokens[j].type != JSMN_OBJECT) {
                    ESP_LOGW(TAG, "devices.json: device entry is not object — skipped");
                    j = skip_token(tokens, j, ntokens);
                    continue;
                }
                int obj_keys = tokens[j].size;
                j++;

                RemoteDeviceConfig cfg = {};   // transport defaults "ble"
                char transport[8] = "ble";     // "hw_type"/"transport" override; any value honoured
                bool broken = false;
                for (int k = 0; k < obj_keys; k++) {
                    if (bad_key(j)) {          // no value: nothing after it can be trusted
                        ESP_LOGW(TAG, "%s: device member without a value — the rest ignored", path);
                        broken = true;
                        break;
                    }
                    if (jsoneq(buf, &tokens[j], "id")) {
                        tok_to_str(buf, &tokens[j + 1], tmp, sizeof(tmp));
                        cfg.id = tmp;
                    } else if (jsoneq(buf, &tokens[j], "identity") ||
                               jsoneq(buf, &tokens[j], "mac")) {   // "mac" = legacy field alias
                        tok_to_str(buf, &tokens[j + 1], tmp, sizeof(tmp));
                        cfg.identity = tmp;
                    } else if (jsoneq(buf, &tokens[j], "name")) {
                        // Connect device's adv-name (the connect target). Distinct from the
                        // human "label" (UI-only, round-trips via GET) — must NOT be conflated
                        // or a panel's name would be overwritten by its display label.
                        tok_to_str(buf, &tokens[j + 1], tmp, sizeof(tmp));
                        cfg.name = tmp;
                    } else if (jsoneq(buf, &tokens[j], "transport") ||
                               jsoneq(buf, &tokens[j], "hw_type")) {   // "hw_type" = legacy field alias
                        tok_to_str(buf, &tokens[j + 1], transport, sizeof(transport));
                    }
                    // Advance past the key, then past the value SUBTREE. A blind j += 2 would
                    // desync the walk if a hand-crafted row carried a nested object/array
                    // value (mirrors parse_bindings_json's nested-settings handling).
                    j++;
                    j = skip_token(tokens, j, ntokens);
                }
                if (broken) {
                    j = ntokens;               // this row and every one after it are dropped
                    break;
                }
                cfg.transport = transport;

                // Valid when it has an identity: a broadcast/observer identity (BLE MAC) OR a
                // connect name. id is always required. Any transport is accepted — a row for a
                // transport whose driver is not present just sits inert (no driver resolves it).
                if (cfg.id.empty() || (cfg.identity.empty() && cfg.name.empty())) {
                    continue;   // missing id + identity — skip
                }

                // Merge: runtime row overrides a board.json device of the same id,
                // else appends. runtime-wins-by-id keeps one merged inventory.
                bool merged = false;
                for (auto& dev : board.remote_devices) {
                    if (dev.id == cfg.id) {
                        dev.identity  = cfg.identity;
                        dev.name      = cfg.name;
                        dev.transport = cfg.transport;
                        merged = true;
                        break;
                    }
                }
                if (!merged && !push_or_fail(board.remote_devices, cfg, "devices.json", "devices")) return false;
            }
            i = j;
        } else {
            i++;
            i = skip_token(tokens, i, ntokens);
        }
    }

    return true;
}

// ═══════════════════════════════════════════════════════════════
// Parse bindings.json
// ═══════════════════════════════════════════════════════════════

bool ConfigService::parse_bindings_json(const char* path, BindingTable& table) {
    char* buf = s_json_buf;
    jsmntok_t* tokens = s_json_tokens;

    size_t len = 0;
    const ReadStatus rs = read_json_file(path, buf, len);
    if (rs == ReadStatus::MISSING) {
        ESP_LOGE(TAG, "Cannot open %s", path);
        return false;
    }
    if (rs != ReadStatus::OK) return false;   // already logged

    jsmn_parser parser;
    jsmn_init(&parser);
    int ntokens = jsmn_parse(&parser, buf, len, tokens, MAX_TOKENS);
    if (ntokens < 0) {
        ESP_LOGE(TAG, "%s: parse error %d — %s", path, ntokens, jsmn_err_str(ntokens));
        return false;
    }

    if (ntokens < 1 || tokens[0].type != JSMN_OBJECT) {
        ESP_LOGE(TAG, "bindings.json: root is not object");
        return false;
    }

    char tmp[32];
    bool seen_bindings = false;

    // jsmn is not strict: a member with no ':' and no value ({"beta" 3900}, {"bindings"}) still
    // counts towards its object's size, so the sizes below can promise more tokens than were
    // parsed. Every step is checked against ntokens — past it is the previous parse's tokens or
    // uninitialised heap — and a key must be a string that owns exactly one value.
    auto bad_key = [&](int at) {
        return at + 1 >= ntokens || tokens[at].type != JSMN_STRING || tokens[at].size != 1;
    };

    for (int i = 1; i < ntokens; ) {
        if (jsoneq(buf, &tokens[i], "bindings")) {
            if (i + 1 >= ntokens) {
                ESP_LOGE(TAG, "%s: 'bindings' has no value", path);
                return false;
            }
            if (tokens[i + 1].type != JSMN_ARRAY) {
                ESP_LOGE(TAG, "bindings.json: 'bindings' is not array");
                return false;
            }
            seen_bindings = true;
            int arr_size = tokens[i + 1].size;
            int j = i + 2;

            for (int elem = 0; elem < arr_size; elem++) {
                if (j >= ntokens) {
                    ESP_LOGE(TAG, "%s: 'bindings' ends early", path);
                    return false;
                }
                if (tokens[j].type != JSMN_OBJECT) {
                    ESP_LOGE(TAG, "bindings.json: entry is not object");
                    return false;
                }
                int obj_keys = tokens[j].size;
                j++;

                Binding binding = {};
                for (int k = 0; k < obj_keys; k++) {
                    if (bad_key(j)) {
                        ESP_LOGE(TAG, "%s: binding member without a value", path);
                        return false;
                    }
                    if (jsoneq(buf, &tokens[j], "hardware")) {
                        tok_to_str(buf, &tokens[j + 1], tmp, sizeof(tmp));
                        binding.hardware_id = tmp;
                        j += 2;
                    } else if (jsoneq(buf, &tokens[j], "role")) {
                        tok_to_str(buf, &tokens[j + 1], tmp, sizeof(tmp));
                        binding.role = tmp;
                        j += 2;
                    } else if (jsoneq(buf, &tokens[j], "driver")) {
                        tok_to_str(buf, &tokens[j + 1], tmp, sizeof(tmp));
                        binding.driver_type = tmp;
                        j += 2;
                    } else if (jsoneq(buf, &tokens[j], "module")) {
                        tok_to_str(buf, &tokens[j + 1], tmp, sizeof(tmp));
                        binding.module_name = tmp;
                        j += 2;
                    } else if (jsoneq(buf, &tokens[j], "address")) {
                        tok_to_str(buf, &tokens[j + 1], tmp, sizeof(tmp));
                        binding.address = tmp;
                        j += 2;
                    } else if (jsoneq(buf, &tokens[j], "settings")) {
                        // Per-binding driver settings: {"beta": 3900, "offset": 0.5}.
                        // Object value, so the scalar "j += 2" fallback below would
                        // mis-walk the tokens — must consume the nested object here.
                        if (tokens[j + 1].type == JSMN_OBJECT) {
                            int nset = tokens[j + 1].size;
                            int sj = j + 2;
                            for (int s = 0; s < nset; s++) {
                                if (bad_key(sj)) {
                                    ESP_LOGE(TAG, "%s: setting without a value", path);
                                    return false;
                                }
                                char skey[24];   // longest key "read_interval_ms" = 16 + NUL
                                char sval[24];
                                tok_to_str(buf, &tokens[sj],     skey, sizeof(skey));
                                tok_to_str(buf, &tokens[sj + 1], sval, sizeof(sval));
                                BindingSetting bs;
                                    bs.key   = skey;
                                    bs.value = static_cast<float>(atof(sval));
                                    if (!push_or_fail(binding.settings, bs, "bindings.json", "settings")) return false;
                                sj += 2;
                            }
                            j = sj;
                        } else {
                            j += 2;
                        }
                    } else {
                        j += 2;
                    }
                }

                if (!push_or_fail(table.bindings, binding, "bindings.json", "bindings")) return false;
            }
            i = j;
        } else {
            i++;
            i = skip_token(tokens, i, ntokens);
        }
    }

    // A bindings file without a bindings array is a broken file, not an empty table — an
    // empty table is spelled "bindings": []. This is also what stands between a stray
    // upload and a board with no drivers (POST /api/bindings validates through here).
    if (!seen_bindings) {
        ESP_LOGE(TAG, "%s: no 'bindings' array", path);
        return false;
    }

    return true;
}

} // namespace modesp
