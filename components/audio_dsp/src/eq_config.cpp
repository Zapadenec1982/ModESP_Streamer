/**
 * @file eq_config.cpp
 * @brief jsmn parser and serialiser for /data/eq.json.
 */

#include "eq_config.h"
#include "eq_web.h"          // eq_web_clamp(): a band read from a file is held to the editors' limits
#include "safe_file.h"

#define JSMN_STATIC          // jsmn is header-only and linked in several places
#include "jsmn.h"
#include "jsmn_util.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace audio_core {

EqBus& eq_bus() {
    static EqBus bus;
    return bus;
}

void EqBus::publish(const EqBand* bands, int count) {
    if (count < 0) count = 0;
    if (count > MAX_BANDS) count = MAX_BANDS;
    const uint32_t s = seq_.load(std::memory_order_relaxed);
    seq_.store(s + 1, std::memory_order_release);          // odd: write in progress
    for (int i = 0; i < count; i++) bands_[i] = bands[i];
    count_ = count;
    seq_.store(s + 2, std::memory_order_release);          // even: complete
}

bool EqBus::read(EqBand* bands, int& count) const {
    for (int attempt = 0; attempt < 4; attempt++) {
        const uint32_t s0 = seq_.load(std::memory_order_acquire);
        if (s0 == 0) return false;             // nothing published yet
        if (s0 & 1u) continue;                 // writer mid-set
        const int n = count_;
        for (int i = 0; i < n && i < MAX_BANDS; i++) bands[i] = bands_[i];
        if (seq_.load(std::memory_order_acquire) == s0) { count = n; return true; }
    }
    return false;                              // the caller keeps what it has
}
namespace {

constexpr size_t MAX_FILE   = 4096;
constexpr int    MAX_TOKENS = 320;

using jsmn_util::key_is;
using jsmn_util::span;

void copy_str(const char* json, const jsmntok_t& t, char* out, size_t out_sz) {
    int n = t.end - t.start;
    if (n < 0) n = 0;
    if ((size_t)n >= out_sz) n = (int)out_sz - 1;
    memcpy(out, json + t.start, n);
    out[n] = '\0';
}

/// A number. jsmn is not strict here, so inf and nan pass as numbers, and strtof makes 1e39 an
/// inf too. No field can hold one — as a band's gain it designs NaN coefficients, and the audio
/// core silences every block they run in — so `finite` turns false and the file is refused.
float num(const char* json, const jsmntok_t& t, bool& finite) {
    char b[24];
    copy_str(json, t, b, sizeof(b));
    const float v = strtof(b, nullptr);
    if (!std::isfinite(v)) finite = false;
    return v;
}

bool truthy(const char* json, const jsmntok_t& t) {
    return (t.end - t.start) == 4 && !strncmp(json + t.start, "true", 4);
}

BandType parse_type(const char* json, const jsmntok_t& t) {
    char b[16];
    copy_str(json, t, b, sizeof(b));
    if (!strcmp(b, "lowshelf"))  return BandType::LOW_SHELF;
    if (!strcmp(b, "highshelf")) return BandType::HIGH_SHELF;
    if (!strcmp(b, "lowpass"))   return BandType::LOW_PASS;
    if (!strcmp(b, "highpass"))  return BandType::HIGH_PASS;
    if (!strcmp(b, "notch"))     return BandType::NOTCH;
    return BandType::PEAK;
}

const char* type_name(BandType t) {
    switch (t) {
    case BandType::LOW_SHELF:  return "lowshelf";
    case BandType::HIGH_SHELF: return "highshelf";
    case BandType::LOW_PASS:   return "lowpass";
    case BandType::HIGH_PASS:  return "highpass";
    case BandType::NOTCH:      return "notch";
    default:                   return "peak";
    }
}

/// The index of the value the key at `k` holds, or -1 when it holds none. jsmn here is not
/// strict and lets a key close with nothing after it, as in {"f":}: the token after it is then
/// something else's, or, for the last key, one jsmn never wrote — past the whole array when
/// the file filled it. A key's value is its one child, so a key with none is caught here.
int value_of(const jsmntok_t* tok, int count, int k) {
    return k + 1 < count && tok[k].size == 1 ? k + 1 : -1;
}

} // namespace

bool eq_parse(const char* json, size_t len, EqConfig& cfg) {
    cfg = EqConfig{};

    jsmn_parser p;
    jsmntok_t tok[MAX_TOKENS];
    jsmn_init(&p);
    const int n = jsmn_parse(&p, json, len, tok, MAX_TOKENS);
    if (n < 1 || tok[0].type != JSMN_OBJECT) return false;

    // Schema gate, before anything is read out of the file. eq_serialize
    // has always stamped "version": 2 and nothing ever looked at it, so a file written by a
    // LATER build would be read with today's rules: the fields this parser does not know
    // would be dropped silently, and the first touch of a slider would write the file back
    // without them. Refusing costs a flat EQ and a log line; the alternative costs the file.
    // Compared as read, not cast to int: an int cast of a float out of its range is undefined.
    bool finite = true;   // every number read so far (num())
    {
        float file_version = EQ_SCHEMA_VERSION;
        for (int i = 1, f = 0; f < tok[0].size && i < n; f++) {
            const int vi = i + 1;
            if (vi >= n) break;
            if (key_is(json, tok[i], "version")) { file_version = num(json, tok[vi], finite); break; }
            i = vi + span(tok, n, vi);
        }
        if (!finite || file_version > EQ_SCHEMA_VERSION) return false;
    }

    // A key without a value, or a number no float holds, is a file this parser cannot read, at
    // any depth, and is refused like any other: cfg back at its defaults (eq_config.h).
    auto malformed = [&cfg] { cfg = EqConfig{}; return false; };

    int i = 1;
    for (int f = 0; f < tok[0].size && i < n; f++) {
        const jsmntok_t& key = tok[i];
        const int vi = value_of(tok, n, i);
        if (vi < 0) return malformed();

        if (key_is(json, key, "mode")) {
            copy_str(json, tok[vi], cfg.mode, sizeof(cfg.mode));
        } else if (key_is(json, key, "preset")) {
            // An id is [a-z0-9_]; anything else is not one, and would not survive a rewrite
            // copied as it is (a cut escape, a quote), so it is dropped.
            const int len = tok[vi].end - tok[vi].start;
            bool plain = tok[vi].type == JSMN_STRING && len > 0 && len < (int)sizeof(cfg.preset);
            for (int c = 0; plain && c < len; c++) {
                const char ch = json[tok[vi].start + c];
                plain = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_';
            }
            if (plain) copy_str(json, tok[vi], cfg.preset, sizeof(cfg.preset));
        } else if (key_is(json, key, "crossover") && tok[vi].type == JSMN_OBJECT) {
            // one nested object: pick freq_hz out of it
            int ci = vi + 1;
            for (int c = 0; c < tok[vi].size && ci < n; c++) {
                const int cv = value_of(tok, n, ci);
                if (cv < 0) return malformed();
                if (key_is(json, tok[ci], "freq_hz")) cfg.crossover_hz = num(json, tok[cv], finite);
                ci += 1 + span(tok, n, ci + 1);
            }
        } else if (key_is(json, key, "outputs") && tok[vi].type == JSMN_ARRAY) {
            int si = vi + 1;
            for (int s = 0; s < tok[vi].size && si < n; s++) {
                if (cfg.sections < MAX_SECTIONS && tok[si].type == JSMN_OBJECT) {
                    EqSection& sec = cfg.sec[cfg.sections];
                    int ki = si + 1;
                    for (int k = 0; k < tok[si].size && ki < n; k++) {
                        const jsmntok_t& sk = tok[ki];
                        const int svi = value_of(tok, n, ki);
                        if (svi < 0) return malformed();
                        if      (key_is(json, sk, "id"))       copy_str(json, tok[svi], sec.id, sizeof(sec.id));
                        else if (key_is(json, sk, "label"))    copy_str(json, tok[svi], sec.label, sizeof(sec.label));
                        else if (key_is(json, sk, "gain_db"))  sec.gain_db  = num(json, tok[svi], finite);
                        else if (key_is(json, sk, "delay_ms")) sec.delay_ms = num(json, tok[svi], finite);
                        else if (key_is(json, sk, "invert"))   sec.invert   = truthy(json, tok[svi]);
                        else if (key_is(json, sk, "bands") && tok[svi].type == JSMN_ARRAY) {
                            int bi = svi + 1;
                            for (int b = 0; b < tok[svi].size && bi < n; b++) {
                                if (sec.count < MAX_BANDS && tok[bi].type == JSMN_OBJECT) {
                                    EqBand& band = sec.bands[sec.count];
                                    int fi = bi + 1;
                                    for (int q = 0; q < tok[bi].size && fi < n; q++) {
                                        const jsmntok_t& bk = tok[fi];
                                        const int bv = value_of(tok, n, fi);
                                        if (bv < 0) return malformed();
                                        if      (key_is(json, bk, "type")) band.type    = parse_type(json, tok[bv]);
                                        else if (key_is(json, bk, "f"))    band.freq_hz = num(json, tok[bv], finite);
                                        else if (key_is(json, bk, "gain")) band.gain_db = num(json, tok[bv], finite);
                                        else if (key_is(json, bk, "q"))    band.q       = num(json, tok[bv], finite);
                                        else if (key_is(json, bk, "on"))   band.enabled = truthy(json, tok[bv]);
                                        fi += 1 + span(tok, n, fi + 1);
                                    }
                                    // What the panel's page and the web can set: a file is no
                                    // way past them, and a gain of hundreds of dB, designed as
                                    // written, took the volume down to nothing.
                                    eq_web_clamp(band);
                                    sec.count++;
                                }
                                bi += span(tok, n, bi);
                            }
                        }
                        ki += 1 + span(tok, n, ki + 1);
                    }
                    cfg.sections++;
                }
                si += span(tok, n, si);
            }
        }
        i = vi + span(tok, n, vi);
    }
    if (!finite) return malformed();
    return cfg.sections > 0;
}

size_t eq_serialize(const EqConfig& cfg, char* buf, size_t buf_sz) {
    // Every write checks the remaining room, so a too-small buffer returns 0 rather than
    // producing half a JSON document.
    jsmn_util::Appender add(buf, buf_sz);

    add("{\n  \"_comment\": \"%s\",\n",
        "Written by the player. DSP config, sectioned by output — see docs/CONCEPT.md, \\\"Outputs\\\". "
        "Band type: peak | lowshelf | highshelf | lowpass | highpass | notch.");
    add("  \"version\": %d,\n  \"mode\": \"%s\",\n", EQ_SCHEMA_VERSION, cfg.mode);
    // A new field and not a new schema: an older build skips it and reads the bands as ever.
    if (cfg.preset[0]) add("  \"preset\": \"%s\",\n", cfg.preset);
    add("  \"crossover\": { \"freq_hz\": %.0f, \"slope\": \"LR24\" },\n", (double)cfg.crossover_hz);
    add("  \"outputs\": [\n");
    for (int s = 0; s < cfg.sections; s++) {
        const EqSection& sec = cfg.sec[s];
        add("    {\n      \"id\": \"%s\",\n      \"label\": \"%s\",\n", sec.id, sec.label);
        add("      \"gain_db\": %.1f,\n      \"delay_ms\": %.2f,\n      \"invert\": %s,\n",
            (double)sec.gain_db, (double)sec.delay_ms, sec.invert ? "true" : "false");
        add("      \"bands\": [\n");
        for (int b = 0; b < sec.count; b++) {
            const EqBand& x = sec.bands[b];
            // Frequency and Q as %.6g, not rounded to a fixed width: the presets put a band at
            // 62.5 Hz and the shelves at Q 0.707, and "%.0f"/"%.2f" brought them back as 62 and
            // 0.71 - still the preset to the ear, but no longer equal to it, so the EQ screen
            // showed the user's own set after every reboot (customer, 04.10.2026).
            add("        {\"type\": \"%s\", \"f\": %.6g, \"gain\": %.1f, \"q\": %.6g, \"on\": %s}%s\n",
                type_name(x.type), (double)x.freq_hz, (double)x.gain_db, (double)x.q,
                x.enabled ? "true" : "false", b + 1 < sec.count ? "," : "");
        }
        add("      ]\n    }%s\n", s + 1 < cfg.sections ? "," : "");
    }
    add("  ]\n}\n");
    return add.size();
}

bool eq_load(const char* path, EqConfig& cfg) {
    cfg = EqConfig{};
    FILE* f = fopen(path, "r");
    if (!f) return false;
    static char buf[MAX_FILE];
    const size_t len = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[len] = '\0';
    return eq_parse(buf, len, cfg);
}

/// eq_save()'s and eq_copy_file()'s: static, because they run on the main task, which has
/// little stack to spare, and one at a time (the EQ's owner calls both).
static char s_out[MAX_FILE];

bool eq_save(const char* path, const EqConfig& cfg) {
    char* const buf = s_out;
    const size_t len = eq_serialize(cfg, buf, sizeof(s_out));
    if (len == 0) return false;

    // One implementation of write -> read back -> rename, shared with the station list, the
    // resume point and POST /api/bindings. This used to be its own copy,
    // and that copy did `remove(path)` before the rename on the belief that LittleFS would
    // not replace an existing target. It does — lfs_rename commits the replace in one
    // metadata operation — so the remove bought nothing and opened a window in which the EQ
    // existed nowhere, around a flash erase. safe_file removes first only if the rename
    // actually refuses.
    //
    // No validator: these bytes come out of eq_serialize, and what eq_parse makes of
    // eq_serialize's output is settled by the host round-trip tests, not by re-parsing on
    // the device. Re-parsing here would also put eq_parse's 5 KB token array on the caller's
    // stack, and eq_save is called from the main loop.
    return safe_file::save(path, buf, len);
}

bool eq_copy_file(const char* src, const char* dst) {
    // Not safe_file::copy: that streams through two half-kilobyte buffers on the caller's
    // stack, and this runs where eq_save does, on the main task. A file that is not whole in
    // the buffer is not one eq_load() reads either, and is not copied.
    FILE* f = fopen(src, "rb");
    if (!f) return false;
    const size_t len = fread(s_out, 1, sizeof(s_out), f);
    const bool whole = len < sizeof(s_out) && !ferror(f);
    fclose(f);
    return whole && safe_file::save(dst, s_out, len);
}

const char* eq_type_name(BandType t) { return type_name(t); }

bool eq_type_from_name(const char* name, size_t len, BandType& out) {
    static constexpr struct { const char* s; BandType t; } NAMES[] = {
        {"peak", BandType::PEAK}, {"lowshelf", BandType::LOW_SHELF}, {"highshelf", BandType::HIGH_SHELF},
        {"lowpass", BandType::LOW_PASS}, {"highpass", BandType::HIGH_PASS}, {"notch", BandType::NOTCH},
    };
    for (const auto& n : NAMES) {
        if (strlen(n.s) == len && !strncmp(name, n.s, len)) { out = n.t; return true; }
    }
    return false;
}

} // namespace audio_core
