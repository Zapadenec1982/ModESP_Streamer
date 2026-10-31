/**
 * @file radio_catalog.cpp
 * @brief Request paths, parsers and filters for the Radio Browser catalog. See radio_catalog.h.
 *
 * Every answer is parsed with jsmn into tokens the caller provides, because a page of 24
 * stations is ~27 KB of JSON and ~1 800 tokens: that belongs in PSRAM on the device, and a
 * static array here would put 30 KB of it in .bss whether the catalog is ever opened or not.
 */

#include "radio_catalog.h"

#include "json_util.h"   // jsmn and the helpers shared with radio_art.cpp
#include "stations.h"    // tidy_name(): a row's name is one line, as a station's

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

namespace radio_catalog {
namespace {

bool playable_codec(const char* c) {
    return !strcmp(c, "MP3") || !strcmp(c, "AAC") || !strcmp(c, "AAC+") || !strcmp(c, "FLAC");
}

/// The stream itself, without what distinguishes one listing of it from another: Shoutcast's
/// ";" and the tracking query strings directories append.
size_t stream_key_len(const char* url) {
    return strcspn(url, "?;");
}

bool same_stream(const char* a, const char* b) {
    const size_t na = stream_key_len(a);
    return na == stream_key_len(b) && !strncmp(a, b, na);
}

/// One station object → a row. False when it has no URL that fits a row: a truncated URL
/// is not a shorter URL, it is a broken one.
bool fill_row(const char* json, const jsmntok_t* tok, int count, int obj, Row& r) {
    char resolved[URL_LEN] = "";
    char plain[URL_LEN]    = "";
    bool resolved_fits = false, plain_fits = false;
    int j = obj + 1;
    for (int k = 0; k < tok[obj].size && j + 1 < count; k++) {
        const jsmntok_t& key = tok[j];
        const jsmntok_t& val = tok[j + 1];
        if (key_is(json, key, "stationuuid"))       copy_str(json, val, r.uuid, sizeof(r.uuid));
        else if (key_is(json, key, "name"))         copy_str(json, val, r.name, sizeof(r.name));
        else if (key_is(json, key, "url_resolved")) resolved_fits = copy_str(json, val, resolved, sizeof(resolved));
        else if (key_is(json, key, "url"))          plain_fits = copy_str(json, val, plain, sizeof(plain));
        else if (key_is(json, key, "favicon")) {
            // A logo URL cut short is a broken one: whole, or none.
            if (!copy_str(json, val, r.favicon, sizeof(r.favicon))) r.favicon[0] = '\0';
        }
        else if (key_is(json, key, "codec"))        copy_str(json, val, r.codec, sizeof(r.codec));
        else if (key_is(json, key, "countrycode"))  copy_str(json, val, r.country, sizeof(r.country));
        else if (key_is(json, key, "bitrate"))      r.bitrate = (uint16_t)to_uint(json, val);
        else if (key_is(json, key, "votes"))        r.votes = to_uint(json, val);
        else if (key_is(json, key, "clickcount"))   r.clicks = to_uint(json, val);
        else if (key_is(json, key, "hls"))          r.hls = to_uint(json, val) != 0;
        j += 1 + span(tok, count, j + 1);
    }
    // Names come from people typing into a web form: tabs, newlines, leading spaces. A row is
    // one line, as the list's are.
    radio_core::tidy_name(r.name);
    // Same size as r.url, and copy_str has said the whole text is in it.
    if (resolved_fits && resolved[0])   memcpy(r.url, resolved, sizeof(r.url));
    else if (plain_fits && plain[0])    memcpy(r.url, plain, sizeof(r.url));
    else return false;
    if (!r.name[0]) snprintf(r.name, sizeof(r.name), "%s", r.uuid);
    return true;
}

bool is_country(const char* s, size_t n) {
    return n == 2 && ((s[0] | 0x20) >= 'a' && (s[0] | 0x20) <= 'z') &&
                     ((s[1] | 0x20) >= 'a' && (s[1] | 0x20) <= 'z');
}

void set_country(char (&out)[3], const char* s) {
    out[0] = (char)(s[0] & ~0x20);
    out[1] = (char)(s[1] & ~0x20);
    out[2] = '\0';
}

struct Country { const char* code; const char* name[NAME_LANGS]; };
const Country COUNTRIES[] = {
#include "countries.inc"
};

}  // namespace

bool parse_spec(const char* spec, Query& out) {
    if (!spec) return false;
    while (*spec == ' ') spec++;
    const char* colon = strchr(spec, ':');
    if (!colon) return false;
    const size_t kw = (size_t)(colon - spec);
    const char* arg = colon + 1;
    while (*arg == ' ') arg++;
    size_t n = strlen(arg);
    while (n && arg[n - 1] == ' ') n--;
    if (!n) return false;

    auto word = [&](const char* w) { return strlen(w) == kw && !strncmp(spec, w, kw); };
    Query q = out;
    q.arg[0] = '\0';
    q.country[0] = '\0';
    if (word("top") || word("country")) {
        if (!is_country(arg, n)) return false;
        q.kind = word("top") ? Kind::TOP : Kind::COUNTRY;
        set_country(q.country, arg);
    } else if (word("tag") || word("name")) {
        q.kind = word("tag") ? Kind::TAG : Kind::NAME;
        // "jazz@UA": the last "@" followed by exactly a country code, and nothing after it.
        const char* at = nullptr;
        for (const char* p = arg; p < arg + n; p++) if (*p == '@') at = p;
        if (at && is_country(at + 1, (size_t)(arg + n - at - 1)) && at > arg) {
            set_country(q.country, at + 1);
            n = (size_t)(at - arg);
            while (n && arg[n - 1] == ' ') n--;
        }
        if (!n || n >= sizeof(q.arg)) return false;
        memcpy(q.arg, arg, n);
        q.arg[n] = '\0';
    } else {
        return false;
    }
    out = q;
    return true;
}

size_t url_encode(const char* in, char* out, size_t out_sz) {
    static const char HEX[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char* p = (const unsigned char*)in; *p; p++) {
        const unsigned char c = *p;
        const bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                           (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
        if (o + (plain ? 1u : 3u) >= out_sz) return 0;
        if (plain) {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = HEX[c >> 4];
            out[o++] = HEX[c & 15];
        }
    }
    if (o >= out_sz) return 0;
    out[o] = '\0';
    return o;
}

size_t search_path(const Query& q, uint32_t limit, char* out, size_t out_sz) {
    size_t n = 0;
    bool ok = true;
    auto add = [&](const char* fmt, auto... args) {
        if (!ok) return;
        const int w = snprintf(out + n, out_sz - n, fmt, args...);
        if (w < 0 || (size_t)w >= out_sz - n) { ok = false; return; }
        n += (size_t)w;
    };
    if (!out || !out_sz) return 0;
    char enc[3 * sizeof(q.arg)];
    add("%s", "/json/stations/search?hidebroken=true");
    switch (q.kind) {
    case Kind::TOP:
    case Kind::COUNTRY:
        if (!q.country[0]) return 0;
        add("&countrycode=%s", q.country);
        break;
    case Kind::TAG:
    case Kind::NAME:
        if (!q.arg[0] || !url_encode(q.arg, enc, sizeof(enc))) return 0;
        add(q.kind == Kind::TAG ? "&tag=%s" : "&name=%s", enc);
        if (q.country[0]) add("&countrycode=%s", q.country);
        break;
    }
    switch (q.sort) {
    case Sort::POPULAR: add("%s", "&order=clickcount&reverse=true"); break;
    case Sort::VOTES:   add("%s", "&order=votes&reverse=true"); break;
    case Sort::QUALITY: add("%s", "&order=bitrate&reverse=true"); break;
    case Sort::NAME:    add("%s", "&order=name"); break;
    }
    switch (q.quality) {
    case Quality::ANY:      break;
    case Quality::K128:     add("%s", "&bitrateMin=128"); break;
    case Quality::K192:     add("%s", "&bitrateMin=192"); break;
    case Quality::K320:     add("%s", "&bitrateMin=320"); break;
    case Quality::LOSSLESS: add("%s", "&codec=FLAC"); break;
    }
    add("&limit=%u&offset=%u", (unsigned)limit, (unsigned)q.offset);
    return ok ? n : 0;
}

bool keep(const Row& r, Quality q) {
    if (!r.url[0] || r.hls || !playable_codec(r.codec)) return false;
    const bool lossless = !strcmp(r.codec, "FLAC");
    switch (q) {
    case Quality::ANY:      return true;
    case Quality::LOSSLESS: return lossless;
    case Quality::K128:     return !lossless && r.bitrate >= 128;
    case Quality::K192:     return !lossless && r.bitrate >= 192;
    case Quality::K320:     return !lossless && r.bitrate >= 320;
    }
    return false;
}

size_t scratch_bytes(const char* json, size_t len) {
    if (!json) return 0;
    jsmn_parser p;
    jsmn_init(&p);
    const int n = jsmn_parse(&p, json, len, nullptr, 0);
    return n > 0 ? (size_t)n * sizeof(jsmntok_t) : 0;
}

int parse_page(const char* json, size_t len, void* scratch, size_t scratch_sz,
               Quality q, Row* out, int max, PageStats* stats) {
    PageStats st;
    const int n = tokenize(json, len, scratch, scratch_sz);
    const jsmntok_t* tok = static_cast<const jsmntok_t*>(scratch);
    int kept = 0;
    const bool array = n > 0 && for_each_object_in(tok, n, 0, [&](int obj) {
        st.seen++;
        if (kept >= max) return;                  // counted, not kept: the next page's
        st.consumed++;
        Row r;
        if (!fill_row(json, tok, n, obj, r) || r.hls || !playable_codec(r.codec)) {
            st.unplayable++;
            return;
        }
        if (!keep(r, q)) { st.below++; return; }
        for (int i = 0; i < kept; i++) {
            if (same_stream(out[i].url, r.url)) { st.repeats++; return; }
        }
        out[kept++] = r;
    });
    st.kept = kept;
    if (stats) *stats = st;
    return array ? kept : -1;
}

bool parse_favicon(const char* json, size_t len, void* scratch, size_t scratch_sz,
                   char* out, size_t out_sz) {
    if (!out || !out_sz) return false;
    out[0] = '\0';
    const int n = tokenize(json, len, scratch, scratch_sz);
    const jsmntok_t* tok = static_cast<const jsmntok_t*>(scratch);
    bool found = false;
    if (n > 0) {
        for_each_object_in(tok, n, 0, [&](int obj) {
            if (found) return;
            const int f = field(json, tok, n, obj, "favicon");
            found = f >= 0 && copy_str(json, tok[f], out, out_sz) && out[0];
            if (!found) out[0] = '\0';
        });
    }
    return found;
}

Image image_kind(const uint8_t* d, size_t len) {
    if (len >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) return Image::JPEG;
    if (len >= 8 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G' &&
        d[4] == 0x0D && d[5] == 0x0A && d[6] == 0x1A && d[7] == 0x0A) return Image::PNG;
    return Image::NONE;
}

int parse_servers(const char* json, size_t len, void* scratch, size_t scratch_sz,
                  char (*out)[HOST_LEN], int max) {
    const int n = tokenize(json, len, scratch, scratch_sz);
    const jsmntok_t* tok = static_cast<const jsmntok_t*>(scratch);
    int got = 0;
    const bool array = n > 0 && for_each_object_in(tok, n, 0, [&](int obj) {
        const int v = field(json, tok, n, obj, "name");
        if (v < 0 || got >= max) return;
        char host[HOST_LEN];
        if (!copy_str(json, tok[v], host, sizeof(host)) || !host[0]) return;
        for (int i = 0; i < got; i++) if (!strcmp(out[i], host)) return;
        snprintf(out[got++], HOST_LEN, "%s", host);
    });
    return array ? got : -1;
}

int parse_countrycodes(const char* json, size_t len, void* scratch, size_t scratch_sz,
                       CountryCount* out, int max) {
    const int n = tokenize(json, len, scratch, scratch_sz);
    const jsmntok_t* tok = static_cast<const jsmntok_t*>(scratch);
    int got = 0;
    const bool array = n > 0 && for_each_object_in(tok, n, 0, [&](int obj) {
        const int v = field(json, tok, n, obj, "name");
        const int c = field(json, tok, n, obj, "stationcount");
        if (v < 0 || got >= max) return;
        char code[8];
        if (!copy_str(json, tok[v], code, sizeof(code)) || !is_country(code, strlen(code))) return;
        set_country(out[got].code, code);
        out[got].stations = c >= 0 ? to_uint(json, tok[c]) : 0;
        got++;
    });
    return array ? got : -1;
}

int parse_genres(const char* json, size_t len, void* scratch, size_t scratch_sz,
                 Genre* out, int max) {
    const int n = tokenize(json, len, scratch, scratch_sz);
    const jsmntok_t* tok = static_cast<const jsmntok_t*>(scratch);
    if (n < 1 || tok[0].type != JSMN_OBJECT) return -1;
    const int list = field(json, tok, n, 0, "genres");
    if (list < 0 || tok[list].type != JSMN_ARRAY) return -1;
    int got = 0;
    int i = list + 1;
    for (int k = 0; k < tok[list].size && i < n; k++) {
        const int obj = i;
        i += span(tok, n, obj);
        if (tok[obj].type != JSMN_OBJECT || got >= max) continue;
        const int nm = field(json, tok, n, obj, "name");
        const int tg = field(json, tok, n, obj, "tag");
        if (nm < 0 || tg < 0) continue;
        Genre g;
        if (!copy_str(json, tok[nm], g.name[0], sizeof(g.name[0])) ||
            !copy_str(json, tok[tg], g.tag, sizeof(g.tag)) || !g.name[0][0] || !g.tag[0]) continue;
        // The other languages' names are optional, and one that does not fit is left out
        // rather than cut: the Ukrainian name shows instead.
        static constexpr const char* KEYS[NAME_LANGS] = {"name", "name_en", "name_de", "name_pl"};
        for (int l = 1; l < NAME_LANGS; l++) {
            const int nl = field(json, tok, n, obj, KEYS[l]);
            if (nl < 0 || !copy_str(json, tok[nl], g.name[l], sizeof(g.name[l]))) g.name[l][0] = '\0';
        }
        out[got++] = g;
    }
    return got;
}

const char* genre_name(const Genre& g, int lang) {
    if (lang < 0 || lang >= NAME_LANGS || !g.name[lang][0]) lang = 0;
    return g.name[lang];
}

const char* country_name(const char* iso2, int lang) {
    if (lang < 0 || lang >= NAME_LANGS) lang = 0;
    if (!iso2 || !is_country(iso2, strlen(iso2))) return nullptr;
    char code[3];
    set_country(code, iso2);
    int lo = 0, hi = (int)(sizeof(COUNTRIES) / sizeof(COUNTRIES[0])) - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        const int c = strcmp(COUNTRIES[mid].code, code);
        if (!c) return COUNTRIES[mid].name[lang];
        if (c < 0) lo = mid + 1; else hi = mid - 1;
    }
    return nullptr;
}

void CatalogStore::set(const Page& p) {
    volatile unsigned& g = *(volatile unsigned*)&gen_;
    g = g + 1;                               // odd: being written
    page_ = p;
    g = g + 1;                               // even: settled
}

uint32_t CatalogStore::generation() const {
    return (uint32_t)*(volatile unsigned*)&gen_;
}

int CatalogStore::count() const {
    return (*(volatile unsigned*)&gen_ & 1u) ? 0 : page_.count;
}

bool CatalogStore::at(int i, Row& out) const {
    uint32_t serial = 0;
    return at(i, out, serial);
}

bool CatalogStore::at(int i, Row& out, uint32_t& serial) const {
    for (int attempt = 0; attempt < 4; attempt++) {
        const unsigned g0 = *(volatile unsigned*)&gen_;
        if (g0 & 1u) continue;                        // mid-write
        if (i < 0 || i >= page_.count) return false;
        out    = page_.row[i];
        serial = page_.serial;
        if (*(volatile unsigned*)&gen_ == g0) return true;
    }
    return false;
}

bool CatalogStore::copy(Page& out) const {
    for (int attempt = 0; attempt < 4; attempt++) {
        const unsigned g0 = *(volatile unsigned*)&gen_;
        if (g0 & 1u) continue;
        out = page_;
        if (*(volatile unsigned*)&gen_ == g0) return true;
    }
    return false;
}

CatalogStore& catalog() {
    // ~7 KB: twelve rows with their URLs. In PSRAM on the device, for the reason stations()
    // gives — internal .bss is what the SDIO co-processor's receive pool and the I2S DMA
    // descriptors compete for. Written by the catalog task, read by the main and LVGL tasks:
    // all tasks, so the flash-cache-off rule for PSRAM does not bite.
#ifdef ESP_PLATFORM
    // No static fallback here: its storage would sit in .bss whether it were used or not.
    static CatalogStore* p = [] {
        void* mem = heap_caps_calloc_prefer(1, sizeof(CatalogStore), 2,
                                            MALLOC_CAP_SPIRAM   | MALLOC_CAP_8BIT,
                                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!mem) abort();   // a board with no memory at all has bigger problems than this
        return new (mem) CatalogStore();
    }();
    return *p;
#else
    static CatalogStore store;
    return store;
#endif
}

}  // namespace radio_catalog
