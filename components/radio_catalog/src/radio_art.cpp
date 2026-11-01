/**
 * @file radio_art.cpp
 * @brief Which ICY title is a song, and which search hit is that song. See radio_art.h.
 */

#include "radio_art.h"
#include "radio_catalog.h"   // url_encode

#include "json_util.h"

#include <cstdio>
#include <cstring>

namespace radio_art {
namespace {

/// Next code point of a UTF-8 string; advances `p`. Bad bytes come back as U+FFFD, one each.
uint32_t next_cp(const unsigned char*& p) {
    const unsigned char c = *p;
    int n = (c < 0x80) ? 0 : (c >> 5) == 0x6 ? 1 : (c >> 4) == 0xE ? 2 : (c >> 3) == 0x1E ? 3 : -1;
    if (n < 0) { p++; return 0xFFFD; }
    uint32_t cp = n == 0 ? c : n == 1 ? (c & 0x1F) : n == 2 ? (c & 0x0F) : (c & 0x07);
    p++;
    for (int i = 0; i < n; i++, p++) {
        if ((*p & 0xC0) != 0x80) return 0xFFFD;   // truncated: stop at the byte that broke it
        cp = (cp << 6) | (*p & 0x3F);
    }
    return cp;
}

uint32_t lower(uint32_t cp) {
    if (cp >= 'A' && cp <= 'Z')         return cp + 0x20;
    if (cp >= 0x0410 && cp <= 0x042F)   return cp + 0x20;   // А..Я
    if (cp >= 0x0400 && cp <= 0x040F)   return cp + 0x50;   // Ѐ..Џ: Ё Є І Ї
    if (cp == 0x0490)                   return 0x0491;      // Ґ
    if (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7) return cp + 0x20;   // Latin-1 capitals
    return cp;
}

bool is_word(uint32_t cp) {
    return (cp >= 'a' && cp <= 'z') || (cp >= '0' && cp <= '9') ||
           (cp >= 0x0430 && cp <= 0x045F) || cp == 0x0491 ||  // Cyrillic, lowercase
           (cp >= 0xDF && cp <= 0xFF && cp != 0xF7);          // Latin-1 lowercase
}

bool word_prefix(const char* a, const char* b) {
    const size_t n = strlen(b);
    return n && !strncmp(a, b, n) && (a[n] == '\0' || a[n] == ' ');
}

/// Drops a "feat ..." / "ft ..." / "featuring ..." tail, given normalised text.
void drop_featuring(char* s) {
    static const char* const WORDS[] = {" feat ", " ft ", " featuring "};
    for (const char* w : WORDS) {
        if (char* at = strstr(s, w)) *at = '\0';
    }
    static const char* const ENDS[] = {" feat", " ft", " featuring"};
    const size_t n = strlen(s);
    for (const char* w : ENDS) {
        const size_t k = strlen(w);
        if (n > k && !strcmp(s + n - k, w)) s[n - k] = '\0';
    }
}

struct Translit { uint32_t cp; const char* nat; const char* alt; };
/// Ukrainian national transliteration (2010) and the Russian-style one record labels often
/// use instead ("Гайтана" is "Gaitana"); a hit matches either.
const Translit TABLE[] = {
    {0x0430, "a", "a"},  {0x0431, "b", "b"},  {0x0432, "v", "v"},   {0x0433, "h", "g"},
    {0x0491, "g", "g"},  {0x0434, "d", "d"},  {0x0435, "e", "e"},   {0x0454, "ie", "ye"},
    {0x0436, "zh", "zh"},{0x0437, "z", "z"},  {0x0438, "y", "i"},   {0x0456, "i", "i"},
    {0x0457, "i", "yi"}, {0x0439, "i", "y"},  {0x043A, "k", "k"},   {0x043B, "l", "l"},
    {0x043C, "m", "m"},  {0x043D, "n", "n"},  {0x043E, "o", "o"},   {0x043F, "p", "p"},
    {0x0440, "r", "r"},  {0x0441, "s", "s"},  {0x0442, "t", "t"},   {0x0443, "u", "u"},
    {0x0444, "f", "f"},  {0x0445, "kh", "kh"},{0x0446, "ts", "ts"}, {0x0447, "ch", "ch"},
    {0x0448, "sh", "sh"},{0x0449, "shch", "sch"}, {0x044C, "", ""}, {0x044E, "iu", "yu"},
    {0x044F, "ia", "ya"},{0x0451, "e", "yo"}, {0x044B, "y", "y"},   {0x044D, "e", "e"},
    {0x044A, "", ""},
};

/// mode 0: national; 1: Russian-style; 2: national with г as "g" ("Гайтана" is "Gaitana" —
/// neither of the first two).
void translit(const char* in, char* out, size_t out_sz, int mode) {
    size_t o = 0;
    const unsigned char* p = (const unsigned char*)in;
    while (*p && o + 1 < out_sz) {
        const unsigned char* start = p;
        const uint32_t cp = next_cp(p);
        const char* rep = nullptr;
        for (const Translit& t : TABLE) {
            if (t.cp == cp) {
                rep = mode == 1 ? t.alt : (mode == 2 && cp == 0x0433) ? "g" : t.nat;
                break;
            }
        }
        if (rep) {
            const size_t k = strlen(rep);
            if (o + k >= out_sz) break;
            memcpy(out + o, rep, k);
            o += k;
        } else {
            const size_t k = (size_t)(p - start);
            if (o + k >= out_sz) break;
            memcpy(out + o, start, k);
            o += k;
        }
    }
    out[o] = '\0';
}

bool title_matches(const char* hit, const char* want) {
    if (!hit[0] || !want[0]) return false;
    if (!strcmp(hit, want)) return true;
    // "Stefania" against "Stefania Kalush Orchestra Version": a subtitle the station left off.
    // Only at a word boundary, and only for a title long enough not to be a common word.
    return strlen(want) >= 4 && (word_prefix(hit, want) || word_prefix(want, hit));
}

bool artist_matches(const char* hit, const char* want) {
    if (!hit[0] || !want[0]) return false;
    if (!strcmp(hit, want) || word_prefix(hit, want) || word_prefix(want, hit)) return true;
    char t[FIELD * 2];
    for (int mode = 0; mode < 3; mode++) {
        translit(want, t, sizeof(t), mode);
        if (!strcmp(hit, t) || word_prefix(hit, t) || word_prefix(t, hit)) return true;
    }
    return false;
}

}  // namespace

bool split(const char* icy, Title& out) {
    out = Title{};
    if (!icy) return false;
    static const char* const SEPS[] = {" - ", " – ", " — "};
    const char* at = nullptr;
    size_t sep = 0;
    for (const char* s : SEPS) {
        const char* f = strstr(icy, s);
        if (f && (!at || f < at)) { at = f; sep = strlen(s); }
    }
    if (!at) return false;
    auto copy = [](const char* b, const char* e, char* o, size_t cap) {
        while (b < e && *b == ' ') b++;
        while (e > b && e[-1] == ' ') e--;
        size_t n = (size_t)(e - b);
        if (n >= cap) {                         // cut before a character, not inside one
            n = cap - 1;
            while (n && ((unsigned char)b[n] & 0xC0) == 0x80) n--;
        }
        memcpy(o, b, n);
        o[n] = '\0';
        return n;
    };
    const size_t a = copy(icy, at, out.artist, sizeof(out.artist));
    const size_t t = copy(at + sep, icy + strlen(icy), out.title, sizeof(out.title));
    return a && t;
}

namespace {

/// keep_brackets: what is in brackets stays, as words. Needed because a bracket is not always
/// a suffix to drop: the song Radio ROKS sent as "Everything I Do (I Do It For You)" is
/// "(Everything I Do) I Do It For You" on Deezer, and dropping the brackets leaves the two
/// with no word in common.
void normalise_as(const char* in, char* out, size_t out_sz, bool keep_brackets) {
    if (!out_sz) return;
    size_t o = 0;
    int depth = 0;
    bool space = true;                          // no leading space
    const unsigned char* p = (const unsigned char*)(in ? in : "");
    while (*p) {
        uint32_t cp = next_cp(p);
        if (!keep_brackets) {
            if (cp == '(' || cp == '[') { depth++; continue; }
            if (cp == ')' || cp == ']') { if (depth) depth--; continue; }
            if (depth) continue;
        }
        if (cp == '\'' || cp == 0x2019 || cp == 0x02BC) continue;   // apostrophes join
        cp = lower(cp);
        if (is_word(cp)) {
            if (!put_utf8(cp, out, o, out_sz)) break;
            space = false;
        } else if (!space) {
            if (o + 1 >= out_sz) break;
            out[o++] = ' ';
            space = true;
        }
    }
    while (o && out[o - 1] == ' ') o--;
    out[o] = '\0';
    drop_featuring(out);
    size_t n = strlen(out);
    while (n && out[n - 1] == ' ') out[--n] = '\0';
}

}  // namespace

void normalise(const char* in, char* out, size_t out_sz) {
    normalise_as(in, out, out_sz, false);
}

void transliterate(const char* in, char* out, size_t out_sz) {
    translit(in, out, out_sz, 0);
}

size_t search_path(const Title& t, char* out, size_t out_sz) {
    char q[FIELD * 2 + 2];
    snprintf(q, sizeof(q), "%s %s", t.artist, t.title);
    char enc[sizeof(q) * 3];
    if (!radio_catalog::url_encode(q, enc, sizeof(enc))) return 0;
    const int n = snprintf(out, out_sz, "/search?q=%s&limit=10", enc);
    return (n > 0 && (size_t)n < out_sz) ? (size_t)n : 0;
}

int parse_search(const char* json, size_t len, void* scratch, size_t scratch_sz,
                 Hit* out, int max) {
    const int n = tokenize(json, len, scratch, scratch_sz);
    const jsmntok_t* tok = static_cast<const jsmntok_t*>(scratch);
    if (n < 1 || tok[0].type != JSMN_OBJECT) return -1;
    const int data = field(json, tok, n, 0, "data");
    int got = 0;
    if (!for_each_object_in(tok, n, data, [&](int obj) {
            if (got >= max) return;
            Hit h;
            int t = field(json, tok, n, obj, "title_short");
            if (t < 0) t = field(json, tok, n, obj, "title");
            const int ar = field(json, tok, n, field(json, tok, n, obj, "artist"), "name");
            const int al = field(json, tok, n, obj, "album");
            int cv = field(json, tok, n, al, "cover_big");
            if (cv < 0) cv = field(json, tok, n, al, "cover_medium");
            if (t < 0 || ar < 0 || cv < 0) return;
            copy_str(json, tok[t], h.title, sizeof(h.title));
            copy_str(json, tok[ar], h.artist, sizeof(h.artist));
            if (!copy_str(json, tok[cv], h.cover, sizeof(h.cover)) || !h.cover[0]) return;
            plain_http(h.cover);
            out[got++] = h;
        })) {
        return -1;
    }
    return got;
}

int best(const Title& want, const Hit* hits, int n) {
    char wa[FIELD * 2], wt[FIELD * 2], wk[FIELD * 2];
    normalise(want.artist, wa, sizeof(wa));
    normalise(want.title, wt, sizeof(wt));
    normalise_as(want.title, wk, sizeof(wk), true);
    if (!wa[0] || (!wt[0] && !wk[0])) return -1;
    for (int i = 0; i < n; i++) {
        char ha[FIELD * 2], ht[FIELD * 2], hk[FIELD * 2];
        normalise(hits[i].artist, ha, sizeof(ha));
        normalise(hits[i].title, ht, sizeof(ht));
        normalise_as(hits[i].title, hk, sizeof(hk), true);
        // A title matches with its brackets dropped ("(Radio Edit)") or kept as words.
        const bool title_ok = title_matches(ht, wt) || title_matches(hk, wk);
        if (title_ok && artist_matches(ha, wa)) return i;
    }
    return -1;
}

void plain_http(char* url) {
    if (!strncmp(url, "https://", 8)) memmove(url + 4, url + 5, strlen(url + 5) + 1);
}

bool split_url(const char* url, char* host, size_t host_sz, int& port, char* path, size_t path_sz) {
    if (!url || strncmp(url, "http://", 7)) return false;
    const char* h = url + 7;
    const char* slash = strchr(h, '/');
    const char* end = slash ? slash : h + strlen(h);
    const char* colon = (const char*)memchr(h, ':', (size_t)(end - h));
    const char* host_end = colon ? colon : end;
    const size_t hn = (size_t)(host_end - h);
    if (!hn || hn >= host_sz) return false;
    memcpy(host, h, hn);
    host[hn] = '\0';
    port = colon ? atoi(colon + 1) : 80;
    if (port <= 0 || port > 65535) return false;
    const char* pth = slash ? slash : "/";
    if (strlen(pth) >= path_sz) return false;
    snprintf(path, path_sz, "%s", pth);
    return true;
}

}  // namespace radio_art
