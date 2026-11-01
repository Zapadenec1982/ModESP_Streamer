/**
 * @file bt_web.cpp
 * @brief GET and POST /api/bt bodies. See bt_web.h.
 */

#include "bt_web.h"
#include "device_name.h"
#include "modesp/net/json_escape.h"

#define JSMN_STATIC          // jsmn is header-only and linked in several places
#include "jsmn.h"
#include "jsmn_util.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace bt_web {
namespace {

constexpr int MAX_TOKENS = 48;

using jsmn_util::key_is;

bool boolean(const char* json, const jsmntok_t& t, bool& out) {
    const int n = t.end - t.start;
    if (t.type != JSMN_PRIMITIVE) return false;
    if (n == 4 && !strncmp(json + t.start, "true", 4))  { out = true;  return true; }
    if (n == 5 && !strncmp(json + t.start, "false", 5)) { out = false; return true; }
    return false;
}

bool integer(const char* json, const jsmntok_t& t, long long lo, long long hi, long long& out) {
    if (t.type != JSMN_PRIMITIVE) return false;
    char b[24];
    const int n = t.end - t.start;
    if (n <= 0 || n >= (int)sizeof(b)) return false;
    memcpy(b, json + t.start, (size_t)n);
    b[n] = '\0';
    char* end = nullptr;
    const long long v = strtoll(b, &end, 10);
    if (end != b + n || v < lo || v > hi) return false;
    out = v;
    return true;
}

}  // namespace

void addr_text(const uint8_t a[6], char* out, size_t n) {
    snprintf(out, n, "%02X:%02X:%02X:%02X:%02X:%02X", a[0], a[1], a[2], a[3], a[4], a[5]);
}

bool parse_addr(const char* s, size_t n, uint8_t out[6]) {
    if (n != 17) return false;
    for (int i = 0; i < 6; i++) {
        const int hi = jsmn_util::hex_digit(s[i * 3]), lo = jsmn_util::hex_digit(s[i * 3 + 1]);
        if (hi < 0 || lo < 0) return false;
        if (i < 5 && s[i * 3 + 2] != ':') return false;
        out[i] = (uint8_t)(hi * 16 + lo);
    }
    return true;
}

size_t json(const bt_settings::View& v, const bt_settings::Pairing& p, bool confirmer,
            char* buf, size_t buf_sz) {
    // too small a buffer gives 0, never half a body
    jsmn_util::Appender add(buf, buf_sz);
    char name[modesp::net::json_escaped_capacity(bt_settings::NAME_CAP - 1)];
    auto esc = [&](const char* s) {
        if (!modesp::net::json_escape(name, sizeof(name), s)) name[0] = '\0';
        return name;
    };
    const char* const T = "true";
    const char* const F = "false";

    add("{\"linked\":%s,\"settings\":%s,\"known\":%s,", v.linked ? T : F, v.settings ? T : F, v.known ? T : F);
    add("\"radio\":%s,\"discoverable\":%s,\"auto_reconnect\":%s,", v.radio ? T : F,
        v.discoverable ? T : F, v.auto_reconnect ? T : F);
    add("\"codecs_supported\":%u,\"codecs_enabled\":%u,", (unsigned)v.codecs_supported, (unsigned)v.codecs_enabled);
    add("\"name\":\"%s\",\"total\":%u,\"bonds\":[", esc(v.name), (unsigned)v.total);
    const unsigned count = v.count < bt_settings::BONDS ? v.count : (unsigned)bt_settings::BONDS;
    for (unsigned i = 0; i < count; i++) {
        const bt_settings::Bond& b = v.bonds[i];
        char addr[18];
        addr_text(b.addr, addr, sizeof(addr));
        add("%s{\"addr\":\"%s\",\"name\":\"%s\",\"connected\":%s,\"last\":%s}", i ? "," : "", addr,
            esc(b.name), (b.flags & 1u) ? T : F, (b.flags & 2u) ? T : F);   // sat::BOND_CONNECTED / BOND_LAST
    }
    add("],\"pairing\":{\"seq\":%lu,\"active\":%s,\"kind\":%u,\"name\":\"%s\",", (unsigned long)p.seq,
        p.active ? T : F, (unsigned)p.kind, esc(p.name));
    add("\"code\":%lu,\"result\":%u,\"by\":%u,\"accepted\":%s},", (unsigned long)(p.active ? p.passkey : 0),
        (unsigned)p.result, (unsigned)p.by, p.accepted ? T : F);
    add("\"confirmer\":%s}", confirmer ? T : F);
    return add.size();
}

bool parse(const char* json, size_t len, Edit& out) {
    out = Edit{};
    static jsmntok_t tok[MAX_TOKENS];
    jsmn_parser p;
    jsmn_init(&p);
    const int n = jsmn_parse(&p, json, len, tok, MAX_TOKENS);
    if (n < 1 || tok[0].type != JSMN_OBJECT) return false;

    bool have_seq = false;
    int i = 1;
    for (int f = 0; f < tok[0].size; f++) {
        if (i + 1 >= n) return false;
        const jsmntok_t& key = tok[i];
        const jsmntok_t& v = tok[i + 1];
        if (v.type == JSMN_OBJECT || v.type == JSMN_ARRAY) return false;   // nothing here nests
        bool b = false;
        long long x = 0;
        if (key_is(json, key, "radio")) {
            if (!boolean(json, v, b)) return false;
            out.radio = b;
        } else if (key_is(json, key, "discoverable")) {
            if (!boolean(json, v, b)) return false;
            out.discoverable = b;
        } else if (key_is(json, key, "auto_reconnect")) {
            if (!boolean(json, v, b)) return false;
            out.auto_reconnect = b;
        } else if (key_is(json, key, "codecs")) {
            if (!integer(json, v, 0, 255, x)) return false;
            out.codecs = (int16_t)x;
        } else if (key_is(json, key, "name")) {
            // Held to the satellite's rule here, before anything is queued: a name it refuses
            // never comes back in its BT_CONFIG, and the panel must not keep one that cannot.
            // A name is one line: no control character, escaped or not.
            size_t got = 0;
            if (v.type != JSMN_STRING ||
                !jsmn_util::unescape(json + v.start, (size_t)(v.end - v.start), out.name, sizeof(out.name),
                                     got, jsmn_util::Controls::NONE) ||
                !device_name::valid(out.name, got)) return false;
            out.rename = true;
        } else if (key_is(json, key, "forget")) {
            if (v.type != JSMN_STRING) return false;
            const size_t vn = (size_t)(v.end - v.start);
            if (vn == 3 && !strncmp(json + v.start, "all", 3)) out.forget_all = true;
            else if (!parse_addr(json + v.start, vn, out.forget_addr)) return false;
            out.forget = true;
        } else if (key_is(json, key, "disconnect")) {
            if (!boolean(json, v, b)) return false;
            out.disconnect = b;
        } else if (key_is(json, key, "refresh")) {
            if (!boolean(json, v, b)) return false;
            out.refresh = b;
        } else if (key_is(json, key, "pair")) {
            if (key_is(json, v, "accept")) out.accept = true;
            else if (!key_is(json, v, "reject")) return false;
            out.pair = true;
        } else if (key_is(json, key, "seq")) {
            if (!integer(json, v, 1, UINT32_MAX, x)) return false;
            out.seq = (uint32_t)x;
            have_seq = true;
        }
        i += 2;
    }
    if (out.pair && !have_seq) return false;
    return out.any_switch() || out.rename || out.forget || out.disconnect || out.refresh || out.pair;
}

}  // namespace bt_web
