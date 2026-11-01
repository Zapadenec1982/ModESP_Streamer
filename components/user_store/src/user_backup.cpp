/**
 * @file user_backup.cpp
 * @brief See user_backup.h.
 */

#include "user_backup.h"
#include "user_store.h"

#define JSMN_STATIC          // jsmn is header-only and linked in several places
#include "jsmn.h"
#include "jsmn_util.h"

#include <cstdlib>
#include <cstring>

namespace modesp {
namespace user_backup {
namespace {

/// The envelope and five string values, with room for fields a later format may add.
constexpr int MAX_TOKENS = 64;

using jsmn_util::key_is;
using jsmn_util::span;

}  // namespace

bool parse(const char* body, size_t len, Entry* out, size_t cap, size_t& count) {
    count = 0;
    static jsmntok_t tok[MAX_TOKENS];
    jsmn_parser p;
    jsmn_init(&p);
    const int n = jsmn_parse(&p, body, len, tok, MAX_TOKENS);
    if (n < 1 || tok[0].type != JSMN_OBJECT) return false;

    int files = -1;
    for (int i = 1, f = 0; f < tok[0].size && i + 1 < n; f++) {
        const int v = i + 1;
        if (key_is(body, tok[i], "modesp_userdata")) {
            if (tok[v].type != JSMN_PRIMITIVE) return false;
            if (atoi(body + tok[v].start) > VERSION) return false;
        } else if (key_is(body, tok[i], "files")) {
            if (tok[v].type != JSMN_OBJECT) return false;
            files = v;
        }
        i = v + span(tok, n, v);
    }
    if (files < 0) return false;

    // In UserStore::FILES order, whatever order the body has them in.
    for (const char* const* name = UserStore::FILES; *name && count < cap; ++name) {
        for (int i = files + 1, f = 0; f < tok[files].size && i + 1 < n; f++) {
            const int v = i + 1;
            if (key_is(body, tok[i], *name)) {
                if (tok[v].type != JSMN_STRING) return false;
                out[count].name = *name;
                out[count].text = body + tok[v].start;
                out[count].len = (size_t)(tok[v].end - tok[v].start);
                count++;
                break;
            }
            i = v + span(tok, n, v);
        }
    }
    return count > 0;
}

bool unescape(const char* s, size_t n, char* out, size_t cap, size_t& len) {
    // The files are text with line breaks in it (eq.json is written a field to a line), which
    // the backup carries escaped.
    return jsmn_util::unescape(s, n, out, cap, len, jsmn_util::Controls::ESCAPED);
}

bool is_json_file(const char* name) {
    return name && strcmp(name, "last_track") != 0;
}

bool is_json_object(const char* s, size_t n) {
    size_t i = 0;
    while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++;
    if (i >= n || s[i] != '{') return false;
    // Without a token array jsmn only counts, which checks the syntax at no memory — but in that
    // mode it does not see a document cut short, so the brackets are counted here as well.
    int depth = 0;
    bool in_str = false, esc = false;
    for (size_t k = i; k < n; k++) {
        const char c = s[k];
        if (in_str) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') in_str = false;
        } else if (c == '"') {
            in_str = true;
        } else if (c == '{' || c == '[') {
            depth++;
        } else if (c == '}' || c == ']') {
            if (--depth < 0) return false;
        }
    }
    if (in_str || depth != 0) return false;
    jsmn_parser p;
    jsmn_init(&p);
    return jsmn_parse(&p, s, n, nullptr, 0) > 0;
}

size_t max_bytes(const char* name) {
    if (!name) return 0;
    // Each reader's buffer less its NUL. modesp_services depends on this component, and the
    // modules on both, so the constants are copied rather than included.
    static constexpr struct { const char* name; size_t max; } LIMITS[] = {
        {"eq.json",        4096 - 1},       // eq_config.cpp MAX_FILE, read as sizeof - 1
        {"stations.json",  24 * 1024 - 1},  // radio_module.cpp STATIONS_JSON_MAX, read as n - 1
        // TRACK_MAX as it was. The player keeps 255 bytes of it (last_dir_[256]) and writes
        // at most a Playlist::MAX_REL path; a longer one only fails to resume.
        {"last_track",     1024},
        {"devices.json",   4096 - 1},       // ConfigService::MAX_JSON_SIZE - 1, read_json_file
        {"eq_custom.json", 4096 - 1},       // as eq.json
    };
    for (const auto& l : LIMITS) {
        if (!strcmp(name, l.name)) return l.max;
    }
    return 0;
}

}  // namespace user_backup
}  // namespace modesp
