/**
 * @file stations.cpp
 * @brief jsmn parser and serialiser for the station list. See stations.h.
 *
 * The shape on disk:
 *
 *     {"stations":[{"name":"...","url":"..."}, ...], "presets":[0,3,-1,-1,-1,-1]}
 *
 * Deliberately the same idea as /data/eq.json: one small object, jsmn, no allocation. A
 * station list is edited by hand often enough — from the web UI, or by copying a file onto
 * the card — that it is worth being readable and worth surviving a mistake in it.
 */

#include "stations.h"

#include <new>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

#define JSMN_STATIC          // jsmn is header-only and linked in several places
#include "jsmn.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace radio_core {
namespace {

/// Per station: the object and three keys with their values (name, url, uuid), and room for
/// one more field a hand-edited file may carry.
constexpr int MAX_TOKENS = 4 + MAX_STATIONS * 9 + PRESETS + 8;

bool key_is(const char* json, const jsmntok_t& t, const char* name) {
    const int n = t.end - t.start;
    return t.type == JSMN_STRING && (int)strlen(name) == n && !strncmp(json + t.start, name, n);
}

/// Copies a JSON string token, undoing the escapes.
///
/// jsmn hands back the raw span between the quotes, backslashes and all — so a station
/// called Rock "FM" comes out of the file with the backslashes still in its name, and
/// gains another pair every time the list is saved. Only the escapes a name or a URL can
/// actually contain are handled; \u sequences are passed through as written: this device
/// writes one only for a control character without a short form, which no name holds
/// (StationList::add) and no working URL does, and guessing at half a surrogate pair is
/// worse than leaving the text alone.
void copy_str(const char* json, const jsmntok_t& t, char* out, size_t out_sz) {
    int n = t.end - t.start;
    if (n < 0) n = 0;
    const char* in = json + t.start;
    size_t o = 0;
    for (int i = 0; i < n && o + 1 < out_sz; i++) {
        char c = in[i];
        if (c == '\\' && t.type == JSMN_STRING && i + 1 < n) {
            const char e = in[++i];
            switch (e) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case '"': case '\\': case '/': c = e; break;
            default:                 // not an escape we know: keep it as it was written
                out[o++] = '\\';
                if (o + 1 >= out_sz) { out[o] = '\0'; return; }
                c = e;
                break;
            }
        }
        out[o++] = c;
    }
    out[o] = '\0';
}

int to_int(const char* json, const jsmntok_t& t) {
    char buf[16];
    copy_str(json, t, buf, sizeof(buf));
    return (int)strtol(buf, nullptr, 10);
}

/// How many tokens one value occupies, including everything nested inside it. jsmn lays
/// them out depth-first, so skipping a value is a matter of counting its children.
///
/// A loop, not recursion, and bounded by `count`: a list restored through the web nested a
/// few hundred levels deep overflowed the httpd stack, and main's at every boot (deep
/// review 2026-10-12, RP-2). Each token takes the place of one child still owed and owes
/// its own in turn; the value ends when nothing is owed.
int span(const jsmntok_t* tok, int count, int i) {
    if (i >= count) return 1;
    int j = i + 1;
    for (int pending = tok[i].size; pending > 0 && j < count; ++j) pending += tok[j].size - 1;
    return j - i;
}

/// JSON string escaping: `"`, `\` and every control character, which JSON allows nowhere raw
/// in a string. A tab written raw into a name left GET /api/stations a body the web could not
/// parse. The five with a short form are written the way copy_str() reads them back.
size_t put_escaped(char* out, size_t cap, const char* s) {
    static constexpr char HEX[] = "0123456789abcdef";
    size_t n = 0;
    for (; *s; s++) {
        const unsigned char c = (unsigned char)*s;
        char e[6] = {'\\', (char)c};
        size_t w = 2;
        switch (c) {
        case '"': case '\\': break;
        case '\b': e[1] = 'b'; break;
        case '\f': e[1] = 'f'; break;
        case '\n': e[1] = 'n'; break;
        case '\r': e[1] = 'r'; break;
        case '\t': e[1] = 't'; break;
        default:
            if (c >= 0x20) { e[0] = (char)c; w = 1; break; }
            e[1] = 'u'; e[2] = '0'; e[3] = '0'; e[4] = HEX[c >> 4]; e[5] = HEX[c & 15]; w = 6;
            break;
        }
        if (n + w >= cap) break;
        memcpy(out + n, e, w);
        n += w;
    }
    if (n < cap) out[n] = '\0';
    return n;
}

}  // namespace

void tidy_name(char* s) {
    for (char* p = s; *p; p++) if ((unsigned char)*p < 0x20) *p = ' ';
    size_t n = strlen(s);
    while (n && s[n - 1] == ' ') s[--n] = '\0';
    size_t lead = 0;
    while (s[lead] == ' ') lead++;
    if (lead) memmove(s, s + lead, n - lead + 1);
}

bool StationList::add(const char* name, const char* url, const char* uuid) {
    if (count >= MAX_STATIONS || !url || !url[0]) return false;
    char* const label = station[count].name;
    snprintf(label, NAME_LEN, "%s", name ? name : "");
    tidy_name(label);
    snprintf(station[count].url, URL_LEN, "%s", url);
    snprintf(station[count].uuid, UUID_LEN, "%s", uuid ? uuid : "");
    if (!label[0]) {
        // No name given: the host is a better label than an empty row.
        const char* p = strstr(url, "://");
        snprintf(label, NAME_LEN, "%s", p ? p + 3 : url);
        tidy_name(label);
    }
    count++;
    return true;
}

int StationList::find(const char* url) const {
    if (!url || !url[0]) return -1;
    for (int i = 0; i < count; i++) {
        if (!strcmp(station[i].url, url)) return i;
    }
    return -1;
}

bool parse(const char* json, size_t len, StationList& out) {
    out = StationList{};
    if (!json || len == 0) return false;

    // The tokens for a full list are 9 KB. They were a static array — internal .bss held for
    // the life of the firmware to parse one file at boot — and are now borrowed for the parse
    // only, from PSRAM on the device.
#ifdef ESP_PLATFORM
    jsmntok_t* tok = (jsmntok_t*)heap_caps_malloc_prefer(MAX_TOKENS * sizeof(jsmntok_t), 2,
                                                         MALLOC_CAP_SPIRAM   | MALLOC_CAP_8BIT,
                                                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
    jsmntok_t* tok = (jsmntok_t*)malloc(MAX_TOKENS * sizeof(jsmntok_t));
#endif
    if (!tok) return false;
    struct Free { jsmntok_t* t; ~Free() { free(t); } } release{tok};

    jsmn_parser p;
    jsmn_init(&p);
    const int n = jsmn_parse(&p, json, len, tok, MAX_TOKENS);
    if (n < 1 || tok[0].type != JSMN_OBJECT) return false;

    int i = 1;
    for (int f = 0; f < tok[0].size && i < n; f++) {
        const jsmntok_t& key = tok[i];
        const int v = i + 1;
        if (v >= n) break;

        if (key_is(json, key, "stations") && tok[v].type == JSMN_ARRAY) {
            int e = v + 1;
            for (int s = 0; s < tok[v].size && e < n; s++) {
                const int obj = e;
                if (tok[obj].type == JSMN_OBJECT) {
                    char name[NAME_LEN] = "", url[URL_LEN] = "", uuid[UUID_LEN] = "";
                    int m = obj + 1;
                    for (int k = 0; k < tok[obj].size && m + 1 < n; k++) {
                        if      (key_is(json, tok[m], "name")) copy_str(json, tok[m + 1], name, sizeof(name));
                        else if (key_is(json, tok[m], "url"))  copy_str(json, tok[m + 1], url,  sizeof(url));
                        else if (key_is(json, tok[m], "uuid")) copy_str(json, tok[m + 1], uuid, sizeof(uuid));
                        m += 1 + span(tok, n, m + 1);
                    }
                    // An entry with no URL is not a station. Skipping it rather than
                    // failing keeps the rest of somebody's list alive.
                    if (url[0]) out.add(name, url, uuid);
                }
                e = obj + span(tok, n, obj);
            }
        } else if (key_is(json, key, "presets") && tok[v].type == JSMN_ARRAY) {
            int e = v + 1;
            for (int s = 0; s < tok[v].size && s < PRESETS && e < n; s++) {
                const int idx = to_int(json, tok[e]);
                out.preset[s] = (idx >= 0 && idx < MAX_STATIONS) ? idx : -1;
                e += span(tok, n, e);
            }
        }
        i = v + span(tok, n, v);
    }
    // A file that parsed as JSON but held no stations is not a station list; saying so lets
    // the caller fall back to the defaults instead of showing an empty radio.
    return out.count > 0;
}

size_t serialize(const StationList& list, char* out, size_t out_sz) {
    if (!out || out_sz < 32) return 0;
    size_t o = 0;
    auto put = [&](const char* s) {
        while (*s && o + 1 < out_sz) out[o++] = *s++;
        return o + 1 < out_sz;
    };

    if (!put("{\n  \"stations\": [")) return 0;
    for (int i = 0; i < list.count; i++) {
        if (!put(i ? ",\n    {\"name\": \"" : "\n    {\"name\": \"")) return 0;
        o += put_escaped(out + o, out_sz - o, list.station[i].name);
        if (!put("\", \"url\": \"")) return 0;
        o += put_escaped(out + o, out_sz - o, list.station[i].url);
        if (list.station[i].uuid[0]) {
            if (!put("\", \"uuid\": \"")) return 0;
            o += put_escaped(out + o, out_sz - o, list.station[i].uuid);
        }
        if (!put("\"}")) return 0;
    }
    if (!put(list.count ? "\n  ],\n  \"presets\": [" : "],\n  \"presets\": [")) return 0;
    for (int i = 0; i < PRESETS; i++) {
        char num[8];
        snprintf(num, sizeof(num), "%s%d", i ? ", " : "", list.preset[i]);
        if (!put(num)) return 0;
    }
    if (!put("]\n}\n")) return 0;
    if (o >= out_sz) return 0;
    out[o] = '\0';
    return o;
}

void defaults(StationList& out) {
    out = StationList{};
    // Long-lived public streams, plain HTTP where the station offers it — one TLS session
    // held open for hours is the expensive part of a network radio (docs/CONCEPT.md, "Network"),
    // and these do not need one.
    out.add("SomaFM Groove Salad", "http://ice1.somafm.com/groovesalad-128-mp3");
    out.add("SomaFM Drone Zone",   "http://ice1.somafm.com/dronezone-128-mp3");
    out.add("SomaFM Deep Space",   "http://ice1.somafm.com/deepspaceone-128-mp3");
    out.add("Radio Paradise Main", "http://stream.radioparadise.com/mp3-128");
    out.add("Radio Paradise Mellow", "http://stream.radioparadise.com/mellow-128");
    out.add("Nightride FM",        "http://stream.nightride.fm/nightride.mp3");
    for (int i = 0; i < PRESETS && i < out.count; i++) out.preset[i] = i;
}

// ── the shared store ──────────────────────────────────────────────────────────────────────
//
// A plain generation counter rather than <atomic>: this component is compiled for the host
// too, where it is single-threaded, and on the device the writer is one task and the reads
// are of a structure that changes a handful of times in a session. The volatile reads and
// the retry are what make a reader notice it read a list mid-write.

StationStore& stations() {
    // 18 KB: 64 x (64-byte name + 224-byte URL) plus the presets. On the device it lives in
    // PSRAM — as internal .bss it permanently held memory the SDIO co-processor's receive
    // pool and the I2S DMA descriptors compete for, to keep a list that changes when
    // somebody edits it. Written by the radio module, read by the LVGL task; both are tasks,
    // so the flash-cache-off rule for PSRAM does not bite (a task touching PSRAM during a
    // flash write is suspended for the duration, not faulted).
    //
    // Behind ESP_PLATFORM because this component is pure by design and the host suite builds
    // it with no ESP-IDF at all.
#ifdef ESP_PLATFORM
    static StationStore* p = [] {
        void* mem = heap_caps_calloc_prefer(1, sizeof(StationStore), 2,
                                            MALLOC_CAP_SPIRAM   | MALLOC_CAP_8BIT,
                                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        // The first caller is the radio module on the main task, long before the screen
        // reads it, so there is no allocation here for the reader to race with.
        return mem ? new (mem) StationStore() : new StationStore();
    }();
    return *p;
#else
    static StationStore s;
    return s;
#endif
}

void StationStore::set(const StationList& l) {
    ++*(volatile unsigned*)&gen_;            // odd: being written
    list_ = l;
    ++*(volatile unsigned*)&gen_;            // even: settled
}

uint32_t StationStore::generation() const {
    return (uint32_t)*(volatile unsigned*)&gen_;
}

int StationStore::count() const {
    return (*(volatile unsigned*)&gen_ & 1u) ? 0 : list_.count;
}

bool StationStore::at(int i, Station& out) const {
    for (int attempt = 0; attempt < 4; attempt++) {
        const unsigned g0 = *(volatile unsigned*)&gen_;
        if (g0 & 1u) continue;                        // mid-write
        if (i < 0 || i >= list_.count) return false;
        out = list_.station[i];
        if (*(volatile unsigned*)&gen_ == g0) return true;
    }
    return false;
}

int StationStore::preset(int slot) const {
    if (slot < 0 || slot >= PRESETS) return -1;
    const int v = list_.preset[slot];
    return (v >= 0 && v < list_.count) ? v : -1;
}

void StationStore::set_preset(int slot, int station) {
    if (slot < 0 || slot >= PRESETS) return;
    ++*(volatile unsigned*)&gen_;
    list_.preset[slot] = (station >= 0 && station < list_.count) ? station : -1;
    ++*(volatile unsigned*)&gen_;
}

void StationStore::copy(StationList& out) const { out = list_; }

bool StationStore::snapshot(StationList& out, uint32_t& gen) const {
    for (int attempt = 0; attempt < 4; attempt++) {
        const unsigned g0 = *(volatile unsigned*)&gen_;
        if (g0 & 1u) continue;                        // mid-write
        out = list_;
        if (*(volatile unsigned*)&gen_ == g0) {
            gen = (uint32_t)g0;
            return true;
        }
    }
    return false;
}

}  // namespace radio_core
