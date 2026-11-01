/**
 * @file radio_catalog.h
 * @brief The station catalog: what to ask Radio Browser, what its answer means, and the
 *        page the screen reads (docs/NETWORK.md, "Radio Browser catalog").
 *
 * Pure: no IDF, no network, no filesystem. The radio module fetches and this decides the
 * rest — the request path, the parse, which rows this device can play, the repeats — which
 * is the part worth testing on the host against answers recorded from the real service.
 *
 * Why the filters live here and not only in the query: the server cannot do all of them.
 * `codec=` takes one value, HLS is not a search parameter at all, FLAC reports a bitrate
 * of 0, and the same stream is often listed several times under different names.
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace radio_catalog {

/// The same sizes as radio_core::Station, so a row becomes a favourite without truncation.
constexpr int NAME_LEN = 64;
constexpr int URL_LEN  = 224;
constexpr int UUID_LEN = 37;
/// Rows a page shows, and rows asked of the server for it: the filters drop some.
constexpr int PAGE_ROWS  = 12;
constexpr int FETCH_ROWS = 24;

enum class Kind : uint8_t { TOP, COUNTRY, TAG, NAME };
enum class Sort : uint8_t { POPULAR, VOTES, QUALITY, NAME };
/// ANY keeps an unknown bitrate; the K floors drop it, FLAC included (its bitrate is 0);
/// LOSSLESS asks for FLAC alone.
enum class Quality : uint8_t { ANY, K128, K192, K320, LOSSLESS };

struct Query {
    Kind     kind    = Kind::TOP;
    char     arg[64] = "";      ///< the tag or the name; unused for TOP and COUNTRY
    char     country[3] = "";   ///< TOP and COUNTRY: required; TAG and NAME: "" is the world
    Sort     sort    = Sort::POPULAR;
    Quality  quality = Quality::K128;
    uint32_t offset  = 0;       ///< server-side index of the first row asked for
};

struct Row {
    char     uuid[UUID_LEN] = "";
    char     name[NAME_LEN] = "";
    char     url[URL_LEN]   = "";   ///< url_resolved, or url when the service has no resolved one
    char     favicon[URL_LEN] = ""; ///< the station's logo, as listed; "" when none or too long
    char     codec[8]       = "";   ///< as the service spells it: "MP3", "AAC", "AAC+", "FLAC"
    char     country[3]     = "";
    uint16_t bitrate = 0;           ///< kbit/s; 0 is "not known", which is what FLAC reports
    uint32_t votes   = 0;
    uint32_t clicks  = 0;           ///< clickcount: plays in the last 24 hours
    bool     hls     = false;
};

/// Reads a request as the radio.cat_query state spells it:
///   "top:UA", "country:DE", "tag:jazz", "tag:jazz@UA", "name:kiss", "name:kiss@UA".
/// Sort, quality and offset are left as they are in `out`. False for anything else.
bool parse_spec(const char* spec, Query& out);

/// Percent-encodes UTF-8 text for a query value. Returns the length, or 0 if it does not fit.
size_t url_encode(const char* in, char* out, size_t out_sz);

/// The request path for one page: "/json/stations/search?...". Returns the length, or 0
/// if it does not fit.
size_t search_path(const Query& q, uint32_t limit, char* out, size_t out_sz);

/// Whether this device plays the row and it passes the quality floor: MP3, AAC, AAC+ or
/// FLAC (Ogg is not compiled in), not HLS, and a bitrate the floor allows.
bool keep(const Row& r, Quality q);

/// Bytes of scratch the parsers below need for this body — jsmn's tokens. 0 when the body
/// is not JSON at all. The caller allocates (PSRAM on the device) and passes it in.
size_t scratch_bytes(const char* json, size_t len);

struct PageStats {
    int seen     = 0;   ///< station objects in the answer
    int consumed = 0;   ///< of those, how many were looked at before `max` rows were kept
    int kept     = 0;
    int unplayable = 0; ///< codec, HLS or no usable URL
    int below    = 0;   ///< under the quality floor
    int repeats  = 0;   ///< the same stream already on the page
};

/// Parses a /json/stations/... answer into rows that keep() allows, without repeats (one
/// stream URL, ignoring its query string, once). Stops after `max` rows. Returns the rows
/// written, or -1 when the body is not a JSON array of stations.
int parse_page(const char* json, size_t len, void* scratch, size_t scratch_sz,
               Quality q, Row* out, int max, PageStats* stats = nullptr);

/// /json/stations/byuuid?uuids=… → the first station's favicon, whatever its codec (it is
/// a station already playing). False when there is none or it does not fit `out`.
bool parse_favicon(const char* json, size_t len, void* scratch, size_t scratch_sz,
                   char* out, size_t out_sz);

/// What an image body is, by its first bytes: the two formats components/cover decodes.
enum class Image : uint8_t { NONE, JPEG, PNG };
Image image_kind(const uint8_t* data, size_t len);

constexpr int HOST_LEN = 64;
/// /json/servers → host names, without repeats (the service lists one per address family).
int parse_servers(const char* json, size_t len, void* scratch, size_t scratch_sz,
                  char (*out)[HOST_LEN], int max);

struct CountryCount {
    char     code[3] = "";
    uint32_t stations = 0;
};
/// /json/countrycodes → codes with their station counts, in the order given.
int parse_countrycodes(const char* json, size_t len, void* scratch, size_t scratch_sz,
                       CountryCount* out, int max);

/// The languages a name is kept in, in ui.lang's order: uk, en, de, pl.
/// An index outside them is Ukrainian.
constexpr int NAME_LANGS = 4;

struct Genre {
    char name[NAME_LANGS][32] = {};   ///< what the screen shows, per language; [0] always set
    char tag[32]  = "";               ///< what is asked for (substring match on the service's tags)
};
/// data/radio_genres.json: {"genres":[{"name":"Джаз","name_en":"Jazz","name_de":"Jazz",
/// "name_pl":"Jazz","tag":"jazz"}, ...]}. "name" is required; a language without its own
/// name_<lang> shows it.
int parse_genres(const char* json, size_t len, void* scratch, size_t scratch_sz,
                 Genre* out, int max);

/// A genre's name in language `lang` (0..NAME_LANGS-1), the Ukrainian one where it has none.
const char* genre_name(const Genre& g, int lang);

/// A country's short name in language `lang` (Unicode CLDR; 0..NAME_LANGS-1, anything else is
/// Ukrainian), or nullptr for a code the table lacks — in every language alike.
const char* country_name(const char* iso2, int lang);

/// One page as the screen shows it.
struct Page {
    Row      row[PAGE_ROWS];
    int      count  = 0;
    int      index  = 0;       ///< 0-based page number within the current query
    bool     more   = false;   ///< whether a next page can exist
    uint32_t serial = 0;       ///< the request that filled it
};

/// The page shared between the module that fetches and the screen that draws.
///
/// Same contract as radio_core::StationStore: one writer, readers that must not block it,
/// a generation counter that is odd while the page is being written.
class CatalogStore {
public:
    void     set(const Page& p);
    uint32_t generation() const;
    int      count() const;
    /// Copies row i. False if out of range or the page moved under us.
    bool     at(int i, Row& out) const;
    /// The same, with the serial of the request the page answers: a row index means
    /// something only together with the page it was read from.
    bool     at(int i, Row& out, uint32_t& serial) const;
    /// Copies the whole page. False if it kept moving.
    bool     copy(Page& out) const;

private:
    Page     page_;
    unsigned gen_ = 0;
};

CatalogStore& catalog();

}  // namespace radio_catalog
