/**
 * @file state_json.cpp
 * @brief SharedState as one JSON object (state_json.h).
 */

#include "modesp/net/state_json.h"
#include "modesp/net/json_escape.h"

#include "esp_heap_caps.h"

#include <cmath>
#include <cstdio>

namespace modesp {
namespace net {

StateJson state_json_begin(char* buf, size_t size) {
    StateJson j = {buf, size, 0, true, false};
    if (size < 2) {
        j.failed = true;
    } else {
        buf[0] = '{';
        j.pos = 1;
    }
    return j;
}

void state_json_entry(const StateKey& key, const StateValue& value, const char* text, void* ud) {
    auto* j = static_cast<StateJson*>(ud);
    if (j->failed) return;

    const char* sep = j->first ? "" : ",";
    char* out = j->buf + j->pos;
    int n = -1;

    if (etl::holds_alternative<float>(value)) {
        const float v = etl::get<float>(value);
        n = std::isfinite(v) ? snprintf(out, j->remaining(), "%s\"%s\":%.2f", sep, key.c_str(),
                                        static_cast<double>(v))
                             : snprintf(out, j->remaining(), "%s\"%s\":null", sep, key.c_str());
    } else if (etl::holds_alternative<int32_t>(value)) {
        n = snprintf(out, j->remaining(), "%s\"%s\":%ld", sep, key.c_str(),
                     static_cast<long>(etl::get<int32_t>(value)));
    } else if (etl::holds_alternative<bool>(value)) {
        n = snprintf(out, j->remaining(), "%s\"%s\":%s", sep, key.c_str(),
                     etl::get<bool>(value) ? "true" : "false");
    } else if (etl::holds_alternative<StringValue>(value)) {
        // Escaped straight into the buffer: the key and its quotes first, then the value.
        n = snprintf(out, j->remaining(), "%s\"%s\":\"", sep, key.c_str());
        if (n < 0 || static_cast<size_t>(n) >= j->remaining()) {
            j->failed = true;
            return;
        }
        const size_t head = static_cast<size_t>(n);
        size_t escaped = 0;
        if (!json_escape(out + head, j->remaining() - head, text ? text : "", &escaped)) {
            j->failed = true;
            return;
        }
        size_t len = head + escaped;
        if (len + 1 >= j->remaining()) {
            j->failed = true;
            return;
        }
        out[len++] = '"';
        out[len] = '\0';
        n = static_cast<int>(len);
    }
    if (n < 0 || static_cast<size_t>(n) >= j->remaining()) {
        j->failed = true;
    } else {
        j->pos += static_cast<size_t>(n);
        j->first = false;
    }
}

bool state_json_end(StateJson& j) {
    if (j.failed || j.remaining() < 2) return false;
    j.buf[j.pos++] = '}';
    j.buf[j.pos] = '\0';
    return true;
}

static char* psram_buffer(char*& p) {
    if (!p) {
        p = static_cast<char*>(heap_caps_calloc_prefer(1, STATE_JSON_SIZE, 2,
                                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                                       MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    return p;
}

char* state_json_buffer_httpd() {
    static char* p = nullptr;
    return psram_buffer(p);
}

char* state_json_buffer_main() {
    static char* p = nullptr;
    return psram_buffer(p);
}

}  // namespace net
}  // namespace modesp
