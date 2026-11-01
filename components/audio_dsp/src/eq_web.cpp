/**
 * @file eq_web.cpp
 * @brief GET and POST /api/eq bodies. See eq_web.h.
 */

#include "eq_web.h"

#define JSMN_STATIC          // jsmn is header-only and linked in several places
#include "jsmn.h"
#include "jsmn_util.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace audio_core {
namespace {

/// A whole band set is 1 + 2 + 2 + 10 × 11 tokens; room for a stray field or two besides.
constexpr int MAX_TOKENS = 160;

using jsmn_util::key_is;
using jsmn_util::span;

bool number(const char* json, const jsmntok_t& t, double& out) {
    if (t.type != JSMN_PRIMITIVE) return false;
    char b[24];
    int n = t.end - t.start;
    if (n <= 0 || n >= (int)sizeof(b)) return false;
    memcpy(b, json + t.start, (size_t)n);
    b[n] = '\0';
    char* end = nullptr;
    out = strtod(b, &end);
    return end == b + n;
}

bool truthy(const char* json, const jsmntok_t& t) {
    return t.type == JSMN_PRIMITIVE && (t.end - t.start) == 4 && !strncmp(json + t.start, "true", 4);
}

float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

}  // namespace

void eq_web_clamp(EqBand& b) {
    // NaN fails every comparison, so it would pass a plain clamp untouched.
    if (!(b.freq_hz == b.freq_hz)) b.freq_hz = 1000.0f;
    if (!(b.gain_db == b.gain_db)) b.gain_db = 0.0f;
    if (!(b.q == b.q)) b.q = 1.0f;
    b.freq_hz = clampf(b.freq_hz, EQ_WEB_F_MIN, EQ_WEB_F_MAX);
    b.gain_db = clampf(b.gain_db, -EQ_WEB_GAIN_MAX, EQ_WEB_GAIN_MAX);
    const bool shelf = b.type == BandType::LOW_SHELF || b.type == BandType::HIGH_SHELF;
    b.q = clampf(b.q, EQ_WEB_Q_MIN, shelf ? SHELF_Q_MAX : EQ_WEB_Q_MAX);
}

size_t eq_web_json(const EqDoc& doc, uint32_t rev, float fs_hz, char* buf, size_t buf_sz) {
    // The same bounded append as eq_serialize: a buffer too small gives 0, never half a body.
    jsmn_util::Appender add(buf, buf_sz);
    add("{\"rev\":%lu,\"fs\":%.0f,\"preset\":\"%s\",\"presets\":[",
        (unsigned long)rev, (double)fs_hz, eq_preset(doc.preset).id);
    // the sounds offered, and a kept preset while it plays (eq_offered)
    for (int n = 0; n < eq_offered_count(doc.preset); n++) {
        add("%s\"%s\"", n ? "," : "", eq_preset(eq_offered(n, doc.preset)).id);
    }
    add("],\"bands\":[");
    const EqSection& sec = doc.cfg.sec[0];
    for (int i = 0; i < sec.count; i++) {
        const EqBand& b = sec.bands[i];
        // as eq.json writes them: %.6g keeps 62.5 Hz and Q 0.707 exact
        add("%s{\"type\":\"%s\",\"f\":%.6g,\"gain\":%.1f,\"q\":%.6g,\"on\":%s}", i ? "," : "",
            eq_type_name(b.type), (double)b.freq_hz, (double)b.gain_db, (double)b.q,
            b.enabled ? "true" : "false");
    }
    add("]}");
    return add.size();
}

bool eq_web_parse(const char* json, size_t len, EqWebEdit& out) {
    out = EqWebEdit{};
    static jsmntok_t tok[MAX_TOKENS];
    jsmn_parser p;
    jsmn_init(&p);
    const int n = jsmn_parse(&p, json, len, tok, MAX_TOKENS);
    if (n < 1 || tok[0].type != JSMN_OBJECT) return false;

    bool have_rev = false, keep = false, have_bands = false, have_preset = false;
    int i = 1;
    for (int f = 0; f < tok[0].size && i < n; f++) {
        const jsmntok_t& key = tok[i];
        const int vi = i + 1;
        if (vi >= n) return false;
        const jsmntok_t& v = tok[vi];
        if (key_is(json, key, "rev")) {
            double r = 0;
            if (!number(json, v, r) || r < 1 || r > (double)UINT32_MAX) return false;
            out.rev = (uint32_t)r;
            have_rev = true;
        } else if (key_is(json, key, "keep")) {
            keep = truthy(json, v);
        } else if (key_is(json, key, "preset")) {
            if (v.type != JSMN_STRING) return false;
            // any preset by id, a kept one too, or by an id it had before
            out.preset = eq_preset_find(json + v.start, (size_t)(v.end - v.start));
            if (out.preset < 0) return false;
            have_preset = true;
        } else if (key_is(json, key, "bands")) {
            // jsmn keeps a repeated key: a second "bands" is a malformed body, as two edits are
            if (have_bands || v.type != JSMN_ARRAY || v.size < 1 || v.size > MAX_BANDS) return false;
            int bi = vi + 1;
            for (int b = 0; b < v.size; b++) {
                if (bi >= n || tok[bi].type != JSMN_OBJECT) return false;
                if (out.count >= MAX_BANDS) return false;
                EqBand& band = out.bands[out.count++];
                int fi = bi + 1;
                for (int q = 0; q < tok[bi].size; q++) {
                    if (fi + 1 >= n) return false;
                    const jsmntok_t& bk = tok[fi];
                    const jsmntok_t& bv = tok[fi + 1];
                    double d = 0;
                    if (key_is(json, bk, "type")) {
                        if (bv.type != JSMN_STRING ||
                            !eq_type_from_name(json + bv.start, (size_t)(bv.end - bv.start), band.type)) return false;
                    } else if (key_is(json, bk, "f")) {
                        if (!number(json, bv, d)) return false;
                        band.freq_hz = (float)d;
                    } else if (key_is(json, bk, "gain")) {
                        if (!number(json, bv, d)) return false;
                        band.gain_db = (float)d;
                    } else if (key_is(json, bk, "q")) {
                        if (!number(json, bv, d)) return false;
                        band.q = (float)d;
                    } else if (key_is(json, bk, "on")) {
                        band.enabled = truthy(json, bv);
                    }
                    fi += 1 + span(tok, n, fi + 1);
                }
                eq_web_clamp(band);
                bi += span(tok, n, bi);
            }
            have_bands = true;
        }
        i = vi + span(tok, n, vi);
    }

    out.keep = keep;
    if (keep && !have_bands && !have_preset) { out.kind = EqWebEdit::KEEP; return true; }
    if (!have_rev || have_bands == have_preset) return false;   // exactly one edit, from a revision
    out.kind = have_bands ? EqWebEdit::BANDS : EqWebEdit::PRESET;
    return true;
}

}  // namespace audio_core
