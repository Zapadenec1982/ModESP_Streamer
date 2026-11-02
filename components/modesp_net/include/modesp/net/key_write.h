/**
 * @file key_write.h
 * @brief One check and one conversion for a key written from outside the firmware.
 *
 * POST /api/settings, POST /api/restore and an MQTT command each turned a text into a value of
 * the key's manifest type, and the three had drifted: MQTT read any bool other than "true" or "1"
 * as false (an "on" switched it off), and HTTP refused it. key_write() is the one rule now; the
 * caller decides what may be written (writable, persist) and stores what comes back, or posts it
 * when the key is a command.
 *
 * Header-only: modesp_mqtt and the host tests take it as it is.
 */

#pragma once

#include "state_meta.h"
#include "modesp/net/json_escape.h"
#include "esp_log.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace modesp {
namespace net {

/// How the value's text arrived.
enum class ValueFrom {
    JSON_STRING,     ///< a JSON string, escapes still in it
    JSON_PRIMITIVE,  ///< a JSON number, true, false or null
    MQTT_TEXT,       ///< an MQTT payload, bare text; "1" and "0" are booleans too
};

struct KeyWrite {
    enum Kind { REFUSED, INT, FLOAT, BOOL, STRING } kind = REFUSED;
    int32_t     i = 0;
    float       f = 0.0f;
    bool        b = false;
    const char* s = nullptr;   ///< STRING: unescaped in place, in the caller's buffer
};

/// The value of `key` as its manifest type, a number held to the manifest's range. `value` is
/// the caller's text, NUL-terminated; a JSON string is unescaped in place. Every refusal and
/// every clamp is a WARN naming `where`. A string is taken only as a JSON string: MQTT applies
/// no string keys.
inline KeyWrite key_write(const gen::StateMeta& meta, const char* key, char* value, ValueFrom from,
                          const char* where) {
    static const char* const TAG = "KeyWrite";
    KeyWrite w;
    const char* type = meta.type;

    if (strcmp(type, "string") == 0) {
        if (from != ValueFrom::JSON_STRING) {
            ESP_LOGW(TAG, "%s: refused '%s' (not a string)", where, key);
        } else if (!json_unescape(value)) {
            ESP_LOGW(TAG, "%s: refused '%s' (an escape that cannot be stored)", where, key);
        } else {
            w.kind = KeyWrite::STRING;
            w.s = value;
        }
        return w;
    }
    if (from == ValueFrom::JSON_STRING) {
        ESP_LOGW(TAG, "%s: refused '%s' (not a %s)", where, key, type);
        return w;
    }

    if (strcmp(type, "bool") == 0) {
        const bool text = from == ValueFrom::MQTT_TEXT;
        if (strcmp(value, "true") == 0 || (text && strcmp(value, "1") == 0)) {
            w.kind = KeyWrite::BOOL;
            w.b = true;
        } else if (strcmp(value, "false") == 0 || (text && strcmp(value, "0") == 0)) {
            w.kind = KeyWrite::BOOL;
            w.b = false;
        } else {
            ESP_LOGW(TAG, "%s: refused '%s' (not a bool)", where, key);
        }
        return w;
    }

    // "int", "float": a number, which JSON's null, true and false are not
    char* end = nullptr;
    const double requested = (value[0] == '-' || (value[0] >= '0' && value[0] <= '9'))
                                 ? strtod(value, &end) : 0.0;
    if (!end || end == value || !std::isfinite(requested)) {     // "-nan", "-inf" parse too
        ESP_LOGW(TAG, "%s: refused '%s' (not a %s)", where, key, type);
        return w;
    }
    const bool clamped = requested < meta.min_val || requested > meta.max_val;
    double v = requested;
    if (v < meta.min_val) v = meta.min_val;
    if (v > meta.max_val) v = meta.max_val;
    if (strcmp(type, "float") == 0) {
        w.kind = KeyWrite::FLOAT;
        w.f = static_cast<float>(v);
    } else {
        w.kind = KeyWrite::INT;
        w.i = static_cast<int32_t>(v);   // toward zero, as atoi() took "5.7"
    }
    // A clamp is never silent: the range is the manifest's word, and a request past it is either
    // a client bug or a manifest that lags the code (an audio.source of 4 landed on 3 for a while
    // because the options stopped at Bluetooth).
    if (clamped) {
        ESP_LOGW(TAG, "%s: %s=%g is outside [%g..%g], clamped to %g", where, key, requested,
                 static_cast<double>(meta.min_val), static_cast<double>(meta.max_val), v);
    }
    return w;
}

}  // namespace net
}  // namespace modesp
