/**
 * @file test_radio_catalog.cpp
 * @brief The station catalog: requests, the page parser, the filters, the store.
 *
 * The page cases run on answers recorded from Radio Browser (tests/host/fixtures/radio_catalog),
 * and the numbers asserted were worked out from the same files by a separate script, not read
 * back from this parser. The edge cases are the ones the service actually produces: a null
 * url_resolved, \u escapes now and then, HLS and unknown codecs among the rows, the same stream
 * listed twice, a body cut off mid-read.
 */

#include "doctest.h"
#include "radio_catalog.h"
#include "radio_catalog_ui.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace radio_catalog;

namespace {

std::string slurp(const char* rel) {
    const std::string path = std::string(MODESP_ROOT) + "/" + rel;
    std::string out;
    if (FILE* f = fopen(path.c_str(), "rb")) {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
        fclose(f);
    }
    return out;
}

std::string fixture(const char* name) {
    return slurp((std::string("tests/host/fixtures/radio_catalog/") + name).c_str());
}

/// Scratch the way the module makes it: exactly what scratch_bytes() asks for.
struct Scratch {
    std::vector<long long> mem;
    explicit Scratch(const std::string& json)
        : mem((scratch_bytes(json.data(), json.size()) + sizeof(long long) - 1) / sizeof(long long)) {}
    void*  data() { return mem.data(); }
    size_t size() const { return mem.size() * sizeof(long long); }
};

int page(const std::string& json, Quality q, Row* out, int max, PageStats* st = nullptr) {
    Scratch s(json);
    return parse_page(json.data(), json.size(), s.data(), s.size(), q, out, max, st);
}

bool valid_utf8(const char* s) {
    const unsigned char* p = (const unsigned char*)s;
    while (*p) {
        const int n = (*p < 0x80) ? 1 : (*p >> 5) == 0x6 ? 2 : (*p >> 4) == 0xE ? 3 : (*p >> 3) == 0x1E ? 4 : 0;
        if (!n) return false;
        for (int i = 1; i < n; i++) if ((p[i] & 0xC0) != 0x80) return false;
        p += n;
    }
    return true;
}

std::string station(const std::string& fields) {
    return "{" + fields + "}";
}

}  // namespace

TEST_CASE("a request spec reads the five forms the state accepts") {
    Query q;
    q.sort = Sort::VOTES;                 // sort, quality and offset are not the spec's business
    q.quality = Quality::K320;

    REQUIRE(parse_spec("top:ua", q));
    CHECK(q.kind == Kind::TOP);
    CHECK(std::string(q.country) == "UA");
    CHECK(q.sort == Sort::VOTES);
    CHECK(q.quality == Quality::K320);

    REQUIRE(parse_spec("country:DE", q));
    CHECK(q.kind == Kind::COUNTRY);
    CHECK(std::string(q.country) == "DE");

    REQUIRE(parse_spec("tag:jazz", q));
    CHECK(q.kind == Kind::TAG);
    CHECK(std::string(q.arg) == "jazz");
    CHECK(q.country[0] == '\0');          // the world

    REQUIRE(parse_spec(" tag: classic rock @ua ", q));
    CHECK(std::string(q.arg) == "classic rock");
    CHECK(std::string(q.country) == "UA");

    REQUIRE(parse_spec("name:Хіт FM", q));
    CHECK(q.kind == Kind::NAME);
    CHECK(std::string(q.arg) == "Хіт FM");

    // An "@" that is not followed by a country code belongs to the name.
    REQUIRE(parse_spec("name:me@home", q));
    CHECK(std::string(q.arg) == "me@home");
    CHECK(q.country[0] == '\0');

    Query before = q;
    CHECK_FALSE(parse_spec("top", q));
    CHECK_FALSE(parse_spec("top:Ukraine", q));
    CHECK_FALSE(parse_spec("tag:", q));
    CHECK_FALSE(parse_spec("genre:jazz", q));
    CHECK_FALSE(parse_spec(nullptr, q));
    CHECK(std::string(q.arg) == before.arg);   // a refused spec leaves the query alone
}

TEST_CASE("query values are percent-encoded as UTF-8") {
    char out[64];
    REQUIRE(url_encode("Хіт FM", out, sizeof(out)) > 0);
    CHECK(std::string(out) == "%D0%A5%D1%96%D1%82%20FM");
    REQUIRE(url_encode("r&b/soul", out, sizeof(out)) > 0);
    CHECK(std::string(out) == "r%26b%2Fsoul");
    CHECK(url_encode("abcdef", out, 6) == 0);  // does not fit with its terminator
}

TEST_CASE("a search path carries the scope, the order, the floor and the page") {
    char p[512];
    Query q;
    REQUIRE(parse_spec("top:UA", q));
    q.offset = 13;
    REQUIRE(search_path(q, FETCH_ROWS, p, sizeof(p)) > 0);
    CHECK(std::string(p) ==
          "/json/stations/search?hidebroken=true&countrycode=UA&order=clickcount&reverse=true"
          "&bitrateMin=128&limit=24&offset=13");

    REQUIRE(parse_spec("tag:r&b@US", q));
    q.sort = Sort::NAME;
    q.quality = Quality::LOSSLESS;
    q.offset = 0;
    REQUIRE(search_path(q, 24, p, sizeof(p)) > 0);
    CHECK(std::string(p) ==
          "/json/stations/search?hidebroken=true&tag=r%26b&countrycode=US&order=name"
          "&codec=FLAC&limit=24&offset=0");

    q.kind = Kind::NAME;
    q.quality = Quality::ANY;
    q.sort = Sort::QUALITY;
    REQUIRE(search_path(q, 24, p, sizeof(p)) > 0);
    CHECK(std::string(p) ==
          "/json/stations/search?hidebroken=true&name=r%26b&countrycode=US&order=bitrate"
          "&reverse=true&limit=24&offset=0");

    CHECK(search_path(q, 24, p, 40) == 0);     // too small: nothing, not half a path
    Query none;                                 // TOP with no country to be the top of
    CHECK(search_path(none, 24, p, sizeof(p)) == 0);
}

TEST_CASE("keep() is what this device plays at the floor asked for") {
    Row r;
    snprintf(r.url, sizeof(r.url), "%s", "http://example.com/s");
    snprintf(r.codec, sizeof(r.codec), "%s", "MP3");
    r.bitrate = 128;
    CHECK(keep(r, Quality::ANY));
    CHECK(keep(r, Quality::K128));
    CHECK_FALSE(keep(r, Quality::K192));
    CHECK_FALSE(keep(r, Quality::LOSSLESS));

    snprintf(r.codec, sizeof(r.codec), "%s", "AAC+");
    r.bitrate = 64;
    CHECK(keep(r, Quality::ANY));              // HE-AAC plays (the radio sets aac_plus_enable)
    CHECK_FALSE(keep(r, Quality::K128));

    snprintf(r.codec, sizeof(r.codec), "%s", "FLAC");
    r.bitrate = 0;                              // what the service reports for FLAC
    CHECK(keep(r, Quality::ANY));
    CHECK(keep(r, Quality::LOSSLESS));
    CHECK_FALSE(keep(r, Quality::K128));

    snprintf(r.codec, sizeof(r.codec), "%s", "OGG");  // not compiled in
    r.bitrate = 320;
    CHECK_FALSE(keep(r, Quality::ANY));
    snprintf(r.codec, sizeof(r.codec), "%s", "UNKNOWN");
    CHECK_FALSE(keep(r, Quality::ANY));

    snprintf(r.codec, sizeof(r.codec), "%s", "MP3");
    r.hls = true;
    CHECK_FALSE(keep(r, Quality::ANY));
    r.hls = false;
    r.url[0] = '\0';
    CHECK_FALSE(keep(r, Quality::ANY));
}

TEST_CASE("the Ukrainian top page: twelve rows, and where the next page starts") {
    const std::string json = fixture("top_ua_24.json");
    REQUIRE(json.size() > 20000);
    Row rows[PAGE_ROWS];
    PageStats st;
    REQUIRE(page(json, Quality::K128, rows, PAGE_ROWS, &st) == 12);
    CHECK(st.seen == 24);
    CHECK(st.consumed == 13);                   // the next page asks from offset + 13
    CHECK(st.below == 1);
    CHECK(st.unplayable == 0);
    CHECK(st.repeats == 0);
    CHECK(st.kept == 12);

    CHECK(std::string(rows[0].name) == "Hit FM (UKraine) - 128kb/s");
    CHECK(std::string(rows[0].uuid) == "961949f3-0601-11e8-ae97-52543be04c81");
    CHECK(std::string(rows[0].url) == "http://195.95.206.17/HitFM");
    CHECK(std::string(rows[0].codec) == "MP3");
    CHECK(rows[0].bitrate == 128);
    CHECK(std::string(rows[0].country) == "UA");
    CHECK(rows[0].votes > 0);
    CHECK(std::string(rows[0].favicon) ==
          "https://www.hitfm.ua/static/img/fav-icon/apple-icon-120x120.png");

    // Raw UTF-8 in the answer arrives intact, and url_resolved is preferred: Люкс FM's
    // listed URL is a redirect, the resolved one is where the stream is.
    CHECK(std::string(rows[1].name) == "Люкс FМ 103.1");
    CHECK(std::string(rows[1].url) == "http://lux.radio.tvstitch.com/kyiv/lux_adv_sd");
    CHECK(std::string(rows[1].codec) == "AAC");
    for (const Row& r : rows) CHECK(valid_utf8(r.name));
}

TEST_CASE("with no floor the same page keeps 22 of 24: two are the same stream twice") {
    const std::string json = fixture("top_ua_24.json");
    Row rows[FETCH_ROWS];
    PageStats st;
    REQUIRE(page(json, Quality::ANY, rows, FETCH_ROWS, &st) == 22);
    CHECK(st.repeats == 2);
    CHECK(st.consumed == 24);

    REQUIRE(page(json, Quality::K320, rows, PAGE_ROWS, &st) == 7);
    CHECK(st.below == 16);
    CHECK(st.repeats == 1);
}

TEST_CASE("a genre page drops the rows this device cannot play") {
    const std::string json = fixture("tag_jazz_128_24.json");
    Row rows[PAGE_ROWS];
    PageStats st;
    REQUIRE(page(json, Quality::K128, rows, PAGE_ROWS, &st) == 12);
    CHECK(st.consumed == 14);
    CHECK(st.unplayable == 2);
    CHECK(std::string(rows[0].name) == "MajorFM");

    REQUIRE(page(json, Quality::LOSSLESS, rows, PAGE_ROWS, &st) == 0);
    CHECK(st.below == 22);
}

TEST_CASE("escapes, a null url_resolved, an oversized URL, HLS and a bad codec") {
    const std::string url_300 = "http://example.com/" + std::string(300, 'a');
    const std::string json = std::string("[") +
        station("\"stationuuid\":\"u1\",\"name\":\"\\u0420\\u0430\\u0434\\u0456\\u043e \\ud83c\\udfb5 \\\"One\\\"\","
                "\"url\":\"http://one.example/live\",\"url_resolved\":null,\"codec\":\"MP3\",\"bitrate\":128,"
                "\"hls\":0,\"countrycode\":\"UA\",\"votes\":5,\"clickcount\":7") + "," +
        station("\"stationuuid\":\"u2\",\"name\":\"Too long\",\"url\":\"" + url_300 + "\","
                "\"url_resolved\":\"" + url_300 + "\",\"codec\":\"MP3\",\"bitrate\":128,\"hls\":0") + "," +
        station("\"stationuuid\":\"u3\",\"name\":\"HLS\",\"url_resolved\":\"http://h.example/a.m3u8\","
                "\"codec\":\"AAC\",\"bitrate\":128,\"hls\":1") + "," +
        station("\"stationuuid\":\"u4\",\"name\":\"Odd\",\"url_resolved\":\"http://o.example/s\","
                "\"codec\":\"UNKNOWN\",\"bitrate\":128,\"hls\":0") + "," +
        station("\"stationuuid\":\"u5\",\"name\":\"\\t  Same stream \\n\",\"url_resolved\":\"http://one.example/live?src=dir\","
                "\"codec\":\"MP3\",\"bitrate\":128,\"hls\":0,\"tags\":[\"a\",{\"b\":1}]") +
        "]";
    Row rows[8];
    PageStats st;
    REQUIRE(page(json, Quality::ANY, rows, 8, &st) == 1);
    CHECK(st.unplayable == 3);                  // too long, HLS, UNKNOWN
    CHECK(st.repeats == 1);                     // u5 is u1 with a query string
    CHECK(std::string(rows[0].name) == "Радіо 🎵 \"One\"");
    CHECK(std::string(rows[0].url) == "http://one.example/live");   // url, since resolved was null
    CHECK(rows[0].clicks == 7);
}

TEST_CASE("a long Cyrillic name is cut at a character, not in the middle of one") {
    std::string name;
    for (int i = 0; i < 70; i++) name += "ї";   // 140 bytes
    const std::string json = "[" + station("\"stationuuid\":\"u\",\"name\":\"" + name +
        "\",\"url_resolved\":\"http://x.example/s\",\"codec\":\"MP3\",\"bitrate\":128,\"hls\":0") + "]";
    Row rows[1];
    REQUIRE(page(json, Quality::ANY, rows, 1) == 1);
    CHECK(strlen(rows[0].name) == 62);          // 31 whole characters in 63 bytes
    CHECK(valid_utf8(rows[0].name));
}

TEST_CASE("not a station array, an empty one, and a body cut off mid-read") {
    Row rows[4];
    CHECK(page("[]", Quality::ANY, rows, 4) == 0);

    const std::string object = "{\"stations\":[]}";
    CHECK(page(object, Quality::ANY, rows, 4) == -1);

    const std::string json = fixture("top_ua_24.json");
    const std::string cut = json.substr(0, json.size() / 2);
    CHECK(scratch_bytes(cut.data(), cut.size()) == 0);
    std::vector<long long> some(4096);
    CHECK(parse_page(cut.data(), cut.size(), some.data(), some.size() * sizeof(long long),
                     Quality::ANY, rows, 4) == -1);
    CHECK(parse_page(json.data(), json.size(), nullptr, 0, Quality::ANY, rows, 4) == -1);
}

TEST_CASE("a body nested deeper than any stack is skipped, not recursed into") {
    // Pages come over plain HTTP, so how deep a body nests is up to whoever answers.
    // Skipping a value recursed once per level, and a few hundred levels overflowed the
    // 8 KB catalog task and rebooted the panel.
    Row rows[4];
    std::string nest(5000, '[');
    nest += std::string(5000, ']');
    CHECK(page(nest, Quality::ANY, rows, 4) == 0);

    // jsmn hangs whatever follows a ':' on the token before it, inside an array too, so a
    // chain of them is as deep as it is long — and, unlike brackets, parses in one pass.
    // 300 000 levels is more than a host's 8 MB stack holds when each one is a call.
    std::string chain = "[";
    for (int i = 0; i < 300000; i++) chain += "\"a\":";
    chain += "\"a\"]";
    CHECK(page(chain, Quality::ANY, rows, 4) == 0);
}

TEST_CASE("a station's favicon comes out of a byuuid answer, whatever its codec") {
    // The byuuid answer has the same shape as a page; the first station's favicon is all
    // that is wanted, and a codec the page filters would drop does not matter here.
    const std::string json = std::string("[") +
        station("\"stationuuid\":\"u\",\"name\":\"N\",\"url\":\"http://x/s\",\"codec\":\"UNKNOWN\","
                "\"favicon\":\"https://example.com/logo.png\"") + "]";
    Scratch s(json);
    char fav[URL_LEN];
    REQUIRE(parse_favicon(json.data(), json.size(), s.data(), s.size(), fav, sizeof(fav)));
    CHECK(std::string(fav) == "https://example.com/logo.png");

    const std::string none = "[" + station("\"stationuuid\":\"u\",\"favicon\":\"\"") + "]";
    Scratch s2(none);
    CHECK_FALSE(parse_favicon(none.data(), none.size(), s2.data(), s2.size(), fav, sizeof(fav)));
    CHECK(fav[0] == '\0');
}

TEST_CASE("an image is told by its first bytes") {
    const uint8_t jpeg[] = {0xFF, 0xD8, 0xFF, 0xE0};
    const uint8_t png[]  = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0};
    const uint8_t webp[] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P'};
    const uint8_t ico[]  = {0, 0, 1, 0, 1, 0};
    CHECK(image_kind(jpeg, sizeof(jpeg)) == Image::JPEG);
    CHECK(image_kind(png, sizeof(png)) == Image::PNG);
    CHECK(image_kind(webp, sizeof(webp)) == Image::NONE);    // components/cover cannot
    CHECK(image_kind(ico, sizeof(ico)) == Image::NONE);
    CHECK(image_kind(jpeg, 2) == Image::NONE);
}

TEST_CASE("the server list names one host, listed once per address family") {
    const std::string json = fixture("servers.json");
    Scratch s(json);
    char hosts[8][HOST_LEN];
    REQUIRE(parse_servers(json.data(), json.size(), s.data(), s.size(), hosts, 8) == 1);
    CHECK(std::string(hosts[0]) == "de1.api.radio-browser.info");
}

TEST_CASE("country codes come with their station counts") {
    const std::string json = fixture("countrycodes.json");
    Scratch s(json);
    std::vector<CountryCount> cc(300);
    const int n = parse_countrycodes(json.data(), json.size(), s.data(), s.size(), cc.data(), 300);
    REQUIRE(n == 241);
    CHECK(std::string(cc[0].code) == "AD");
    CHECK(cc[0].stations == 12);
    bool ua = false;
    for (int i = 0; i < n; i++) {
        if (!strcmp(cc[i].code, "UA")) { ua = true; CHECK(cc[i].stations == 320); }
    }
    CHECK(ua);
}

TEST_CASE("the shipped genre table parses, every genre with a tag") {
    const std::string json = slurp("data/www/radio_genres.json");
    REQUIRE(!json.empty());
    Scratch s(json);
    Genre g[40];
    const int n = parse_genres(json.data(), json.size(), s.data(), s.size(), g, 40);
    REQUIRE(n == 26);
    CHECK(std::string(genre_name(g[0], 0)) == "Поп");
    CHECK(std::string(g[0].tag) == "pop");
    CHECK(std::string(g[4].tag) == "jazz");
    CHECK(std::string(genre_name(g[4], 1)) == "Jazz");
    CHECK(std::string(genre_name(g[6], 2)) == "Klassik");       // Класика
    CHECK(std::string(genre_name(g[22], 3)) == "Wiadomości");   // Новини
    for (int i = 0; i < n; i++) {
        CHECK(g[i].tag[0] != '\0');
        for (int l = 0; l < NAME_LANGS; l++) CHECK(genre_name(g[i], l)[0] != '\0');
        // every shipped genre is named in every language, not left to the Ukrainian fallback
        for (int l = 1; l < NAME_LANGS; l++) CHECK(g[i].name[l][0] != '\0');
    }
}

TEST_CASE("a genre's other names are optional, and one that does not fit is left out, not cut") {
    const std::string json =
        R"({"genres":[{"name":"Джаз","tag":"jazz"},)"
        R"({"name":"Рок","name_de":"Rock","name_pl":"Rockowa muzyka bardzo, bardzo długa nazwa","tag":"rock"}]})";
    Scratch s(json);
    Genre g[4];
    REQUIRE(parse_genres(json.data(), json.size(), s.data(), s.size(), g, 4) == 2);
    CHECK(std::string(genre_name(g[0], 1)) == "Джаз");          // no name_en: the Ukrainian one
    CHECK(std::string(genre_name(g[1], 2)) == "Rock");
    CHECK(std::string(genre_name(g[1], 3)) == "Рок");           // 41 bytes do not fit 32
    CHECK(std::string(genre_name(g[1], 9)) == "Рок");           // no such language
}

TEST_CASE("countries are named in four languages, with the short form where there is one") {
    CHECK(std::string(country_name("UA", 0)) == "Україна");
    CHECK(std::string(country_name("ua", 0)) == "Україна");
    CHECK(std::string(country_name("US", 0)) == "США");
    CHECK(std::string(country_name("DE", 0)) == "Німеччина");
    CHECK(std::string(country_name("DE", 1)) == "Germany");
    CHECK(std::string(country_name("DE", 2)) == "Deutschland");
    CHECK(std::string(country_name("DE", 3)) == "Niemcy");
    CHECK(std::string(country_name("US", 2)) == "USA");
    // CLDR's Polish short form for GB is "Wlk. Bryt.": an abbreviation loses to the full name.
    CHECK(std::string(country_name("GB", 3)) == "Wielka Brytania");
    CHECK(std::string(country_name("UA", 7)) == "Україна");      // no such language: Ukrainian
    CHECK(country_name("QQ", 0) == nullptr);
    CHECK(country_name("QQ", 2) == nullptr);
    CHECK(country_name("", 0) == nullptr);
    CHECK(country_name("UKR", 0) == nullptr);
    CHECK(country_name(nullptr, 1) == nullptr);
}

TEST_CASE("the store hands out whole pages and settles on an even generation") {
    CatalogStore store;
    CHECK(store.count() == 0);
    Page p;
    p.count = 2;
    snprintf(p.row[0].name, NAME_LEN, "%s", "A");
    snprintf(p.row[1].name, NAME_LEN, "%s", "B");
    p.index = 3;
    p.more = true;
    const uint32_t g0 = store.generation();
    store.set(p);
    CHECK(store.generation() == g0 + 2);
    CHECK(store.count() == 2);
    Row r;
    REQUIRE(store.at(1, r));
    CHECK(std::string(r.name) == "B");
    CHECK_FALSE(store.at(2, r));
    Page out;
    REQUIRE(store.copy(out));
    CHECK(out.index == 3);
    CHECK(out.more);
}

TEST_CASE("a row read with its page's serial") {
    CatalogStore store;
    Page p;
    p.count = 1;
    p.serial = 41;
    snprintf(p.row[0].name, NAME_LEN, "%s", "A");
    store.set(p);
    Row r;
    uint32_t serial = 0;
    REQUIRE(store.at(0, r, serial));
    CHECK(serial == 41);
    CHECK_FALSE(store.at(1, r, serial));
}

// ── what the screen and the radio module share (radio_catalog_ui.h) ──

TEST_CASE("countries for the screen: named ones, Ukraine first, then by stations") {
    const std::string json = fixture("countrycodes.json");
    Scratch s(json);
    std::vector<CountryCount> cc(300);
    const int n = parse_countrycodes(json.data(), json.size(), s.data(), s.size(), cc.data(), 300);
    REQUIRE(n == 241);
    const int kept = order_countries(cc.data(), n, "UA");
    // 241 listed: "XX" has no name, and "gr" is "GR" typed in lower case — one country.
    REQUIRE(kept == 239);
    CHECK(std::string(cc[0].code) == "UA");
    CHECK(cc[0].stations == 320);
    CHECK(std::string(cc[1].code) == "US");
    CHECK(cc[1].stations == 7162);
    CHECK(std::string(cc[2].code) == "DE");
    CHECK(std::string(cc[kept - 1].code) == "TV");
    int greece = 0;
    for (int i = 0; i < kept; i++) {
        CHECK(country_name(cc[i].code, 0) != nullptr);
        CHECK(cc[i].stations > 0);
        if (i > 1) CHECK(cc[i].stations <= cc[i - 1].stations);
        if (!strcmp(cc[i].code, "GR")) { greece++; CHECK(cc[i].stations == 2135); }
    }
    CHECK(greece == 1);
}

TEST_CASE("countries: no first, nothing named, nothing at all") {
    CountryCount cc[3];
    snprintf(cc[0].code, sizeof(cc[0].code), "%s", "PL"); cc[0].stations = 5;
    snprintf(cc[1].code, sizeof(cc[1].code), "%s", "UA"); cc[1].stations = 2;
    snprintf(cc[2].code, sizeof(cc[2].code), "%s", "DE"); cc[2].stations = 0;
    REQUIRE(order_countries(cc, 3, nullptr) == 2);
    CHECK(std::string(cc[0].code) == "PL");
    CHECK(std::string(cc[1].code) == "UA");
    CHECK(order_countries(cc, 0, "UA") == 0);
}

TEST_CASE("the screen's asks come out in order, eight at most waiting") {
    Asks q;
    Ask a;
    CHECK_FALSE(q.take(a));
    for (int i = 0; i < 8; i++) {
        Ask in;
        in.kind  = AskKind::PLAY;
        in.value = i;
        in.serial = 100u + (uint32_t)i;
        REQUIRE(q.post(in));
    }
    Ask extra;
    extra.kind = AskKind::QUERY;
    CHECK_FALSE(q.post(extra));                  // the ninth is dropped
    for (int i = 0; i < 8; i++) {
        REQUIRE(q.take(a));
        CHECK(a.kind == AskKind::PLAY);
        CHECK(a.value == i);
        CHECK(a.serial == 100u + (uint32_t)i);
    }
    CHECK_FALSE(q.take(a));
    // and round the ring again, a question with its words
    for (int lap = 0; lap < 3; lap++) {
        Ask in;
        in.kind = AskKind::QUERY;
        snprintf(in.spec, sizeof(in.spec), "tag:jazz%d", lap);
        REQUIRE(q.post(in));
        REQUIRE(q.take(a));
        CHECK(std::string(a.spec) == std::string(in.spec));
        CHECK(a.slot == -1);
    }
}

TEST_CASE("a published value comes back whole, and its generation moves by two") {
    Published<Status> p;
    Status out;
    REQUIRE(p.get(out));
    CHECK(out.state == State::IDLE);
    const uint32_t g0 = p.generation();
    Status s;
    s.state = State::READY;
    snprintf(s.spec, sizeof(s.spec), "%s", "country:DE");
    s.page = 2;
    s.more = true;
    s.serial = 9;
    p.set(s);
    CHECK(p.generation() == g0 + 2);
    REQUIRE(p.get(out));
    CHECK(out.state == State::READY);
    CHECK(std::string(out.spec) == "country:DE");
    CHECK(out.page == 2);
    CHECK(out.more);
    CHECK(out.serial == 9);
}
