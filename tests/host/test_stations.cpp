/**
 * @file test_stations.cpp
 * @brief The station list, parsed and written back.
 *
 * A station list is edited by hand — from the web UI, or by dropping a file on the data
 * partition — so the cases worth testing are the ones a person creates: a trailing entry
 * with no URL, a preset pointing at nothing, a file that is not JSON at all. None of them
 * may cost somebody the rest of their stations.
 */

#include "doctest.h"
#include "stations.h"

#include <cstring>
#include <string>

TEST_CASE("a station list round-trips through JSON") {
    radio_core::StationList a;
    REQUIRE(a.add("Groove Salad", "http://ice1.somafm.com/groovesalad-128-mp3"));
    REQUIRE(a.add("Nightride", "http://stream.nightride.fm/nightride.mp3"));
    a.preset[0] = 1;
    a.preset[1] = 0;

    char buf[2048];
    const size_t n = radio_core::serialize(a, buf, sizeof(buf));
    REQUIRE(n > 0);

    radio_core::StationList b;
    REQUIRE(radio_core::parse(buf, n, b));
    REQUIRE(b.count == 2);
    CHECK(std::string(b.station[0].name) == "Groove Salad");
    CHECK(std::string(b.station[1].url)  == "http://stream.nightride.fm/nightride.mp3");
    CHECK(b.preset[0] == 1);
    CHECK(b.preset[1] == 0);
    CHECK(b.preset[2] == -1);
}

TEST_CASE("an entry without a url is dropped and the rest survive") {
    const char* json =
        "{\"stations\":["
        "{\"name\":\"Good\",\"url\":\"http://a/1\"},"
        "{\"name\":\"Typo\"},"
        "{\"name\":\"Also good\",\"url\":\"http://b/2\"}"
        "]}";
    radio_core::StationList l;
    REQUIRE(radio_core::parse(json, strlen(json), l));
    REQUIRE(l.count == 2);
    CHECK(std::string(l.station[0].name) == "Good");
    CHECK(std::string(l.station[1].name) == "Also good");
}

TEST_CASE("a station from the catalog keeps its uuid through the file; one typed in has none") {
    radio_core::StationList a;
    REQUIRE(a.add("Hit FM", "http://195.95.206.17/HitFM", "961949f3-0601-11e8-ae97-52543be04c81"));
    REQUIRE(a.add("Typed in", "http://a/1"));
    char buf[2048];
    const size_t n = radio_core::serialize(a, buf, sizeof(buf));
    REQUIRE(n > 0);
    CHECK(std::string(buf).find("\"uuid\": \"961949f3-0601-11e8-ae97-52543be04c81\"") != std::string::npos);
    CHECK(std::string(buf).find("\"uuid\": \"\"") == std::string::npos);   // no empty field

    radio_core::StationList b;
    REQUIRE(radio_core::parse(buf, n, b));
    REQUIRE(b.count == 2);
    CHECK(std::string(b.station[0].uuid) == "961949f3-0601-11e8-ae97-52543be04c81");
    CHECK(b.station[1].uuid[0] == '\0');
}

TEST_CASE("a file from before uuids reads as it always did") {
    const char* json = "{\"stations\":[{\"name\":\"Old\",\"url\":\"http://a/1\"}],\"presets\":[0]}";
    radio_core::StationList l;
    REQUIRE(radio_core::parse(json, strlen(json), l));
    REQUIRE(l.count == 1);
    CHECK(l.station[0].uuid[0] == '\0');
    CHECK(l.preset[0] == 0);
}

TEST_CASE("a full list with uuids fits the tokens") {
    radio_core::StationList a;
    for (int i = 0; i < radio_core::MAX_STATIONS; i++) {
        char name[32], url[64];
        snprintf(name, sizeof(name), "Station %d", i);
        snprintf(url, sizeof(url), "http://example.com/stream/%d", i);
        REQUIRE(a.add(name, url, "961949f3-0601-11e8-ae97-52543be04c81"));
    }
    std::string buf(32 * 1024, '\0');
    const size_t n = radio_core::serialize(a, &buf[0], buf.size());
    REQUIRE(n > 0);
    radio_core::StationList b;
    REQUIRE(radio_core::parse(buf.data(), n, b));
    CHECK(b.count == radio_core::MAX_STATIONS);
    CHECK(std::string(b.station[63].uuid) == "961949f3-0601-11e8-ae97-52543be04c81");
}

TEST_CASE("a field the list does not know is skipped whole, however it nests") {
    // Skipping counts a value's tokens in a loop rather than by recursion (deep review
    // 2026-10-12, RP-2). One token too few or too many and what follows is misread.
    const char* json =
        "{\"x\":[[1,{\"a\":[2,{}]}],[]],"
        "\"stations\":["
        "{\"name\":\"One\",\"extra\":{\"k\":[[3],{\"z\":null}]},\"url\":\"http://a/1\"},"
        "{\"name\":\"Two\",\"url\":\"http://b/2\"}"
        "],"
        "\"y\":{\"q\":[[[4]]],\"r\":{}},"
        "\"presets\":[1,0]}";
    radio_core::StationList l;
    REQUIRE(radio_core::parse(json, strlen(json), l));
    REQUIRE(l.count == 2);
    CHECK(std::string(l.station[0].name) == "One");
    CHECK(std::string(l.station[0].url)  == "http://a/1");
    CHECK(std::string(l.station[1].name) == "Two");
    CHECK(std::string(l.station[1].url)  == "http://b/2");
    CHECK(l.preset[0] == 1);
    CHECK(l.preset[1] == 0);
    CHECK(l.preset[2] == -1);
}

TEST_CASE("a station with no name is labelled by its host") {
    const char* json = "{\"stations\":[{\"url\":\"http://ice1.somafm.com/dronezone-128-mp3\"}]}";
    radio_core::StationList l;
    REQUIRE(radio_core::parse(json, strlen(json), l));
    REQUIRE(l.count == 1);
    CHECK(std::string(l.station[0].name) == "ice1.somafm.com/dronezone-128-mp3");
}

TEST_CASE("a preset pointing nowhere becomes empty rather than wild") {
    const char* json =
        "{\"stations\":[{\"name\":\"One\",\"url\":\"http://a/1\"}],"
        "\"presets\":[0, 900, -5, 2]}";
    radio_core::StationList l;
    REQUIRE(radio_core::parse(json, strlen(json), l));
    CHECK(l.preset[0] == 0);
    CHECK(l.preset[1] == -1);          // past MAX_STATIONS
    CHECK(l.preset[2] == -1);          // negative
    CHECK(l.preset[3] == 2);           // in range, even though the list is shorter today
    CHECK(l.preset[4] == -1);
}

TEST_CASE("text that is not a station list is refused, not half-read") {
    radio_core::StationList l;
    const char* junk = "this is not json at all";
    CHECK_FALSE(radio_core::parse(junk, strlen(junk), l));
    CHECK(l.count == 0);

    const char* empty = "{\"stations\":[]}";
    CHECK_FALSE(radio_core::parse(empty, strlen(empty), l));

    const char* other = "{\"bands\":[{\"f\":100}]}";
    CHECK_FALSE(radio_core::parse(other, strlen(other), l));
}

TEST_CASE("a quote in a name survives the round trip") {
    radio_core::StationList a;
    REQUIRE(a.add("Rock \"FM\"", "http://a/1"));
    char buf[512];
    const size_t n = radio_core::serialize(a, buf, sizeof(buf));
    REQUIRE(n > 0);

    radio_core::StationList b;
    REQUIRE(radio_core::parse(buf, n, b));
    REQUIRE(b.count == 1);
    CHECK(std::string(b.station[0].name) == "Rock \"FM\"");
}

namespace {

/// JSON allows no control character raw inside a string (RFC 8259, section 7); the file's own
/// line breaks stand between the values.
bool no_raw_control_in_strings(const std::string& json) {
    bool in = false;
    for (size_t i = 0; i < json.size(); i++) {
        const unsigned char c = (unsigned char)json[i];
        if (in && c < 0x20) return false;
        if (in && c == '\\') { i++; continue; }
        if (c == '"') in = !in;
    }
    return !in;
}

}  // namespace

TEST_CASE("a control character is written escaped, so GET /api/stations stays JSON") {
    // What the web's JSON.parse met in a name: written raw, the body was not JSON.
    radio_core::StationList a;
    REQUIRE(a.add("x", "http://a/1"));
    snprintf(a.station[0].name, radio_core::NAME_LEN, "%s", "T\tN\nR\rB\bF\f\x01" "1\x1f");
    snprintf(a.station[0].url, radio_core::URL_LEN, "%s", "http://a/\t1");
    char buf[512];
    const size_t n = radio_core::serialize(a, buf, sizeof(buf));
    REQUIRE(n > 0);
    const std::string out(buf, n);
    CHECK(no_raw_control_in_strings(out));
    CHECK(out.find(R"("name": "T\tN\nR\rB\bF\f\u00011\u001f")") != std::string::npos);
    CHECK(out.find(R"("url": "http://a/\t1")") != std::string::npos);
}

TEST_CASE("a name is one line: a tab or a line break is a space, and the ends are trimmed") {
    // As the web posts a name with a tab pasted into it: JSON.stringify writes it as \t.
    const char* posted = R"({"stations":[{"name":"  Jazz\tFM\r\nKyiv ","url":"http://a/1"},)"
                         R"({"name":"\t \n","url":"http://ice1.somafm.com/x"}],"presets":[0]})";
    radio_core::StationList l;
    REQUIRE(radio_core::parse(posted, strlen(posted), l));
    REQUIRE(l.count == 2);
    CHECK(std::string(l.station[0].name) == "Jazz FM  Kyiv");
    CHECK(std::string(l.station[1].name) == "ice1.somafm.com/x");   // nothing left: the host

    // and through the file and back, as JSON all the way
    char buf[512];
    const size_t n = radio_core::serialize(l, buf, sizeof(buf));
    REQUIRE(n > 0);
    CHECK(no_raw_control_in_strings(std::string(buf, n)));
    radio_core::StationList b;
    REQUIRE(radio_core::parse(buf, n, b));
    CHECK(std::string(b.station[0].name) == "Jazz FM  Kyiv");

    // A name from the catalog, kept or played, goes the same way (radio_catalog, catalog.cpp).
    char name[radio_core::NAME_LEN] = "\tRadio\nOne  ";
    radio_core::tidy_name(name);
    CHECK(std::string(name) == "Radio One");
    REQUIRE(l.add("Rock\tFM", "http://b/2"));
    CHECK(std::string(l.station[2].name) == "Rock FM");
}

TEST_CASE("the list stops at its capacity instead of running past it") {
    radio_core::StationList l;
    char url[64];
    for (int i = 0; i < radio_core::MAX_STATIONS + 10; i++) {
        snprintf(url, sizeof(url), "http://host/%d", i);
        l.add("x", url);
    }
    CHECK(l.count == radio_core::MAX_STATIONS);
}

TEST_CASE("a station is found by url, which is what survives a rename") {
    radio_core::StationList l;
    l.add("A", "http://a/1");
    l.add("B", "http://b/2");
    CHECK(l.find("http://b/2") == 1);
    CHECK(l.find("http://nowhere") == -1);
    CHECK(l.find("") == -1);
}

TEST_CASE("the built-in list is usable as it stands") {
    radio_core::StationList l;
    radio_core::defaults(l);
    CHECK(l.count > 0);
    for (int i = 0; i < l.count; i++) {
        CHECK(l.station[i].valid());
        CHECK(std::string(l.station[i].name).size() > 0);
        CHECK(std::string(l.station[i].url).rfind("http", 0) == 0);
    }
    CHECK(l.preset[0] == 0);
}

TEST_CASE("a buffer too small produces nothing rather than half a file") {
    radio_core::StationList l;
    radio_core::defaults(l);
    char tiny[40];
    CHECK(radio_core::serialize(l, tiny, sizeof(tiny)) == 0);
}

TEST_CASE("a snapshot from another task carries the generation it was taken at") {
    // GET /api/stations reads the list on the HTTP task; an edit sent back from it is checked
    // against this generation, so a preset stored on the panel in between is not overwritten.
    radio_core::StationStore store;
    radio_core::StationList l;
    radio_core::defaults(l);
    store.set(l);

    radio_core::StationList copy;
    uint32_t g1 = 0;
    REQUIRE(store.snapshot(copy, g1));
    CHECK(g1 % 2 == 0);
    CHECK(copy.count == l.count);
    CHECK(std::string(copy.station[0].url) == l.station[0].url);
    CHECK(copy.preset[0] == l.preset[0]);

    store.set_preset(5, 1);
    uint32_t g2 = 0;
    REQUIRE(store.snapshot(copy, g2));
    CHECK(g2 != g1);
    CHECK(g2 == store.generation());
    CHECK(copy.preset[5] == 1);
}
