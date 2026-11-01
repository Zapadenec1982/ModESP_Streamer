/**
 * @file test_radio_art.cpp
 * @brief Covers for a station's ICY title: which title is a song, which hit is that song.
 *
 * The search answers are Deezer's, recorded for titles the panel actually received or that
 * stand for a case: a hit and a cover version of it, a Ukrainian artist the service spells in
 * Latin, a namesake that must not be taken, a subtitle the station left off, and nothing found
 * at all.
 */

#include "doctest.h"
#include "radio_art.h"
#include "radio_catalog.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace radio_art;

namespace {

std::string fixture(const char* name) {
    const std::string path = std::string(MODESP_ROOT) + "/tests/host/fixtures/radio_catalog/" + name;
    std::string out;
    if (FILE* f = fopen(path.c_str(), "rb")) {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
        fclose(f);
    }
    return out;
}

int search(const std::string& json, Hit* hits, int max) {
    std::vector<long long> s((radio_catalog::scratch_bytes(json.data(), json.size()) + 7) / 8 + 1);
    return parse_search(json.data(), json.size(), s.data(), s.size() * sizeof(long long), hits, max);
}

std::string norm(const char* s) {
    char out[256];
    normalise(s, out, sizeof(out));
    return out;
}

Title title(const char* artist, const char* name) {
    Title t;
    snprintf(t.artist, sizeof(t.artist), "%s", artist);
    snprintf(t.title, sizeof(t.title), "%s", name);
    return t;
}

}  // namespace

TEST_CASE("an ICY title splits at its first dash, and a slogan is not a song") {
    Title t;
    REQUIRE(split("Ace of Base - All That She Wants", t));
    CHECK(std::string(t.artist) == "Ace of Base");
    CHECK(std::string(t.title) == "All That She Wants");

    REQUIRE(split("Океан Ельзи – Обійми", t));          // en dash
    CHECK(std::string(t.artist) == "Океан Ельзи");
    CHECK(std::string(t.title) == "Обійми");

    REQUIRE(split("A — B - C", t));                      // em dash first: the earliest wins
    CHECK(std::string(t.artist) == "A");
    CHECK(std::string(t.title) == "B - C");

    CHECK_FALSE(split("Hit FM", t));
    CHECK_FALSE(split(" - Only a title", t));
    CHECK_FALSE(split("Only an artist - ", t));
    CHECK_FALSE(split("Up-to-date", t));                 // a hyphen inside a word is not a separator
    CHECK_FALSE(split(nullptr, t));
}

TEST_CASE("normalising keeps what identifies a song and drops the rest") {
    CHECK(norm("All That She Wants (Radio Edit)") == "all that she wants");
    CHECK(norm("Stefania [Kalush Orchestra]") == "stefania");
    CHECK(norm("Don't Stop Me Now") == "dont stop me now");
    CHECK(norm("ОКЕАН ЕЛЬЗИ") == "океан ельзи");
    CHECK(norm("Їжак і Ґанок, Єва") == "їжак і ґанок єва");
    CHECK(norm("Artist feat. Guest") == "artist");
    CHECK(norm("Artist ft. Guest - Extra") == "artist");
    CHECK(norm("  Many    spaces  ") == "many spaces");
    CHECK(norm("Beyoncé") == "beyoncé");
    CHECK(norm("((nested) brackets) left") == "left");
}

TEST_CASE("transliteration spells a Ukrainian artist the way the service lists him") {
    char out[64];
    transliterate("океан ельзи", out, sizeof(out));
    CHECK(std::string(out) == "okean elzy");
    transliterate("гайтана", out, sizeof(out));
    CHECK(std::string(out) == "haitana");                // national; the matcher also tries "g"
}

TEST_CASE("the search asks for artist and title together, encoded") {
    char p[512];
    REQUIRE(search_path(title("Океан Ельзи", "Обійми"), p, sizeof(p)) > 0);
    const std::string s(p);
    CHECK(s.rfind("/search?q=%D0%9E%D0%BA", 0) == 0);
    CHECK(s.find("%20") != std::string::npos);
    CHECK(s.substr(s.size() - 9) == "&limit=10");
    CHECK(search_path(title("A", "B"), p, 12) == 0);     // does not fit: nothing
}

TEST_CASE("the hit is the song, not a cover version that names it") {
    Hit hits[10];
    const int n = search(fixture("deezer_ace_of_base.json"), hits, 10);
    REQUIRE(n == 10);
    CHECK(std::string(hits[1].artist) == "한용진");        // someone else's recording of it
    REQUIRE(best(title("Ace of Base", "All That She Wants"), hits, n) == 0);
    CHECK(std::string(hits[0].cover) ==
          "http://cdn-images.dzcdn.net/images/cover/599c31e92ab91c01f8fb889a96e54544/500x500-000000-80-0-0.jpg");
    // The cover version alone is not taken.
    CHECK(best(title("Ace of Base", "All That She Wants"), hits + 1, 1) == -1);
}

TEST_CASE("a Ukrainian artist listed in Latin is still the same artist") {
    Hit hits[10];
    const int n = search(fixture("deezer_okean_elzy.json"), hits, 10);
    REQUIRE(n == 1);
    CHECK(std::string(hits[0].artist) == "Okean Elzy");
    CHECK(best(title("Океан Ельзи", "Обійми"), hits, n) == 0);

    Hit g;
    snprintf(g.artist, sizeof(g.artist), "%s", "Gaitana");
    snprintf(g.title, sizeof(g.title), "%s", "Be My Guest");
    CHECK(best(title("Гайтана", "Be My Guest"), &g, 1) == 0);   // the Russian-style spelling
}

TEST_CASE("a namesake is not taken, and nothing found is nothing") {
    Hit hits[10];
    int n = search(fixture("deezer_michael_a.json"), hits, 10);
    REQUIRE(n == 5);
    CHECK(std::string(hits[0].artist) == "Michael Nau");      // "Scatter", by someone else
    CHECK(best(title("Michael A", "Scatter (Matias Chilano Rmx)"), hits, n) == -1);

    n = search(fixture("deezer_none.json"), hits, 10);
    CHECK(n == 0);
    CHECK(best(title("Puff Dragon", "Refracted Light"), hits, n) == -1);
}

TEST_CASE("a subtitle the station left off still matches") {
    Hit hits[10];
    const int n = search(fixture("deezer_kalush.json"), hits, 10);
    REQUIRE(n == 5);
    CHECK(std::string(hits[0].title) == "Stefania (Kalush Orchestra)");
    CHECK(best(title("Kalush", "Stefania"), hits, n) == 0);
}

TEST_CASE("brackets that are part of the name match when kept as words") {
    // Radio ROKS sent "Everything I Do (I Do It For You)"; Deezer has
    // "(Everything I Do) I Do It For You". Dropped, the brackets leave nothing in common.
    Hit hits[10];
    const int n = search(fixture("deezer_bryan_adams.json"), hits, 10);
    REQUIRE(n == 10);
    CHECK(std::string(hits[0].title) == "(Everything I Do) I Do It For You");
    CHECK(best(title("Bryan Adams", "Everything I Do (I Do It For You)"), hits, n) == 0);
}

TEST_CASE("an error answer is not a search answer") {
    const std::string err = "{\"error\":{\"type\":\"Exception\",\"message\":\"Quota limit exceeded\",\"code\":4}}";
    Hit hits[2];
    CHECK(search(err, hits, 2) == -1);
}

TEST_CASE("cover URLs are plain HTTP, and split into host, port and path") {
    char u[128] = "https://cdn-images.dzcdn.net/images/cover/x/500x500.jpg";
    plain_http(u);
    CHECK(std::string(u) == "http://cdn-images.dzcdn.net/images/cover/x/500x500.jpg");
    char same[] = "http://a/b";
    plain_http(same);
    CHECK(std::string(same) == "http://a/b");

    char host[64], path[128];
    int port = 0;
    REQUIRE(split_url(u, host, sizeof(host), port, path, sizeof(path)));
    CHECK(std::string(host) == "cdn-images.dzcdn.net");
    CHECK(port == 80);
    CHECK(std::string(path) == "/images/cover/x/500x500.jpg");
    REQUIRE(split_url("http://h.example:8080/p?q=1", host, sizeof(host), port, path, sizeof(path)));
    CHECK(port == 8080);
    CHECK(std::string(path) == "/p?q=1");
    REQUIRE(split_url("http://h.example", host, sizeof(host), port, path, sizeof(path)));
    CHECK(std::string(path) == "/");
    CHECK_FALSE(split_url("https://h/p", host, sizeof(host), port, path, sizeof(path)));
}
