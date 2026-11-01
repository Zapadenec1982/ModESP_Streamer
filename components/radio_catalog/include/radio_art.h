/**
 * @file radio_art.h
 * @brief A cover for what a station is playing, found by its ICY title (docs/NETWORK.md,
 *        "Covers and logos").
 *
 * Stations send no artwork: the stream carries "StreamTitle='Artist - Title'" and nothing
 * else. Deezer's search answers over plain HTTP and serves its JPEGs over HTTP too, so a
 * cover costs no TLS session — the one this panel has belongs to the station.
 *
 * Pure, like the rest of radio_catalog. What is worth testing is the judgement, not the
 * fetch: which title is a song at all, and which search hit is that song rather than a
 * cover version or a namesake. Checked with real titles: the advanced syntax
 * (artist:"…" track:"…") returns nothing over HTTP or HTTPS, the plain query works, and a
 * Ukrainian artist may be listed in Latin ("Океан Ельзи" is "Okean Elzy") — hence the
 * transliteration.
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace radio_art {

constexpr int FIELD   = 96;
constexpr int URL_LEN = 256;

struct Title {
    char artist[FIELD] = "";
    char title[FIELD]  = "";
};

/// "Artist - Title" → its two parts (also with an en or em dash). False when there is no
/// separator or a side is empty: a station name or a slogan is not a song.
bool split(const char* icy, Title& out);

/// For comparing, not for showing: lowercase (Latin and Cyrillic), anything in brackets
/// dropped ("(Radio Edit)", "[Remix]"), "feat. …" dropped, punctuation to spaces, spaces
/// collapsed.
void normalise(const char* in, char* out, size_t out_sz);

/// Cyrillic to Latin, Ukrainian national style, for matching an artist the service lists
/// in Latin. Input as normalise() leaves it.
void transliterate(const char* in, char* out, size_t out_sz);

/// The request: "/search?q=<artist title>&limit=10".
size_t search_path(const Title& t, char* out, size_t out_sz);

struct Hit {
    char artist[FIELD]  = "";
    char title[FIELD]   = "";
    char cover[URL_LEN] = "";   ///< album.cover_big (500 px), rewritten to http://
};

/// Deezer's /search answer → hits, in its order. −1 when it is not a search answer.
int parse_search(const char* json, size_t len, void* scratch, size_t scratch_sz,
                 Hit* out, int max);

/// The first hit that is this song: the same title, and the same artist directly, by one
/// starting the other (a credited guest), or through transliteration. −1 when none is.
int best(const Title& want, const Hit* hits, int n);

/// "https://…" → "http://…" in place. Deezer's image CDN serves both.
void plain_http(char* url);

/// "http://host[:port]/path?q" → host, port (80 unless given) and "/path?q". False for
/// anything that is not http://, or a part that does not fit.
bool split_url(const char* url, char* host, size_t host_sz, int& port, char* path, size_t path_sz);

}  // namespace radio_art
