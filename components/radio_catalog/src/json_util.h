/**
 * @file json_util.h
 * @brief jsmn helpers shared by radio_catalog.cpp and radio_art.cpp. Internal to the component.
 *
 * Header-only and in an anonymous namespace, so each source file gets its own copy, as with
 * jsmn itself (JSMN_STATIC). What every parser needs (key_is, span, the UTF-8 writer) is
 * components/jsmn_util's; what is here is the catalog's own.
 */

#pragma once

#define JSMN_STATIC          // jsmn is header-only and linked in several places
#include "jsmn.h"
#include "jsmn_util.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace {

using jsmn_util::hex4;
using jsmn_util::key_is;
using jsmn_util::put_utf8;
using jsmn_util::span;

/// Copies a JSON string token, undoing every escape — \uXXXX and surrogate pairs included,
/// which Radio Browser does send now and then — and never cutting a UTF-8 sequence in half
/// when the text does not fit. Returns false if it had to cut.
inline bool copy_str(const char* json, const jsmntok_t& t, char* out, size_t cap) {
    if (!cap) return false;
    if (t.type != JSMN_STRING) {            // null, a number: no text, and not the word "null"
        out[0] = '\0';
        return true;
    }
    const char* p   = json + t.start;
    const char* end = json + t.end;
    size_t o = 0;
    bool whole = true;
    while (p < end) {
        const unsigned char c = (unsigned char)*p;
        if (c == '\\' && p + 1 < end) {
            uint32_t cp;
            size_t used = 2;
            switch (p[1]) {
            case 'n': cp = '\n'; break;
            case 't': cp = '\t'; break;
            case 'r': cp = '\r'; break;
            case 'b': cp = '\b'; break;
            case 'f': cp = '\f'; break;
            case 'u': {
                const int h = (end - p >= 6) ? hex4(p + 2) : -1;
                if (h < 0) { cp = 0xFFFD; break; }
                cp = (uint32_t)h;
                used = 6;
                if (cp >= 0xD800 && cp <= 0xDBFF) {          // high half: needs its low half
                    const int lo = (end - p >= 12 && p[6] == '\\' && p[7] == 'u') ? hex4(p + 8) : -1;
                    if (lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (uint32_t)(lo - 0xDC00);
                        used = 12;
                    } else {
                        cp = 0xFFFD;
                    }
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    cp = 0xFFFD;                              // a low half on its own
                }
                break;
            }
            default: cp = (unsigned char)p[1]; break;          // \" \\ \/ and anything else
            }
            if (!put_utf8(cp, out, o, cap)) { whole = false; break; }
            p += used;
            continue;
        }
        // A raw byte: the whole UTF-8 sequence it starts, or none of it.
        size_t n = (c < 0x80) ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
        if (n > (size_t)(end - p)) n = (size_t)(end - p);
        if (o + n >= cap) { whole = false; break; }
        memcpy(out + o, p, n);
        o += n;
        p += n;
    }
    out[o] = '\0';
    return whole;
}

inline uint32_t to_uint(const char* json, const jsmntok_t& t) {
    if (t.type != JSMN_PRIMITIVE) return 0;
    const char c = json[t.start];
    if (c == 't') return 1;                                  // true
    if (c < '0' || c > '9') return 0;                        // null, false, negative
    return (uint32_t)strtoul(json + t.start, nullptr, 10);
}

/// Runs jsmn into the caller's scratch. Returns the token count, or a negative jsmn error.
inline int tokenize(const char* json, size_t len, void* scratch, size_t scratch_sz) {
    if (!json || !scratch) return -1;
    jsmn_parser p;
    jsmn_init(&p);
    return jsmn_parse(&p, json, len, static_cast<jsmntok_t*>(scratch),
                      (unsigned)(scratch_sz / sizeof(jsmntok_t)));
}

/// Looks up `name` among an object's keys. Returns the value's index, or −1.
inline int field(const char* json, const jsmntok_t* tok, int count, int obj, const char* name) {
    if (obj < 0 || obj >= count || tok[obj].type != JSMN_OBJECT) return -1;
    int j = obj + 1;
    for (int k = 0; k < tok[obj].size && j + 1 < count; k++) {
        if (key_is(json, tok[j], name)) return j + 1;
        j += 1 + span(tok, count, j + 1);
    }
    return -1;
}

/// Calls each(object_index) for every object in the array at `arr`.
template <class Each>
bool for_each_object_in(const jsmntok_t* tok, int n, int arr, Each each) {
    if (arr < 0 || arr >= n || tok[arr].type != JSMN_ARRAY) return false;
    int i = arr + 1;
    for (int k = 0; k < tok[arr].size && i < n; k++) {
        const int obj = i;
        i += span(tok, n, obj);
        if (tok[obj].type == JSMN_OBJECT) each(obj);
    }
    return true;
}

}  // namespace
