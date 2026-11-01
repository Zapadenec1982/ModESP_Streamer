/**
 * @file test_shared_state.cpp
 * @brief SharedState: the inline string, the side table, and what iteration hands over.
 *
 * The store is the contract between every module on this board, and what it can hold is a
 * memory decision: the inline string came down from 256 to 64 so that 189 map slots stop
 * paying for a value only a dozen keys ever need, and the keys that DO need it — the paths and
 * the URIs, marked `"long": true` in their manifest — moved to a side table.
 *
 * What is worth testing about that is not the sizes. It is the three places where the split
 * could lie about a value:
 *   - a write longer than the inline string must come back WHOLE for a long key, and cut for
 *     a short one (and the cut must be visible, not silent);
 *   - change detection must compare the side table, because two URLs that differ only in a
 *     query string share their first 64 characters and "unchanged" there means a stale screen
 *     and a track whose completion is never noticed;
 *   - iteration must hand a serialiser the whole text, since the variant it also gets holds
 *     only the prefix.
 */

#include "doctest.h"

#include "mocks/freertos_mock.h"
#include "mocks/esp_log_mock.h"

#include "modesp/shared_state.h"
#include "state_meta.h"

#include <cstring>
#include <string>
#include <vector>

using namespace modesp;

namespace {

/// A path of the length the file browser can actually build: Browse::path (160) + '/' +
/// Entry::name (96). This is the value MODESP_MAX_LONG_STRING_LENGTH was chosen for.
std::string long_path(size_t n) {
    std::string s = "/sd/";
    while (s.size() < n) s += 'a' + (char)(s.size() % 26);
    s.resize(n);
    return s;
}

struct Seen {
    std::string key;
    std::string text;
    bool        is_string = false;
};

void collect(const StateKey& key, const StateValue& value, const char* text, void* ud) {
    auto* out = static_cast<std::vector<Seen>*>(ud);
    Seen s;
    s.key       = key.c_str();
    s.is_string = etl::holds_alternative<StringValue>(value);
    s.text      = text ? text : "";
    out->push_back(s);
}

const Seen* find(const std::vector<Seen>& v, const char* key) {
    for (const auto& s : v) if (s.key == key) return &s;
    return nullptr;
}

}  // namespace

TEST_CASE("the build gives SharedState the firmware's string sizes") {
    // tests/host/CMakeLists.txt must stay in step with the root CMakeLists. If this fires,
    // the suite is testing a store the device does not have — which is how the old host copy
    // of shared_state.cpp managed to be green about a 32-char value.
    CHECK(MODESP_MAX_STRING_VALUE_LENGTH == 64);
    CHECK(MODESP_MAX_LONG_STRING_LENGTH == 256);
    CHECK(sizeof(StringValue) < sizeof(LongStringValue));
}

TEST_CASE("the manifests mark the keys that carry a path or a URI") {
    // Not a list to keep in two places — a floor. These are the keys where a cut value is a
    // file that cannot be opened or a URI that stops comparing equal, and dlna_module.cpp
    // decides a track finished by strcmp'ing the last two.
    for (const char* key : {"player.track", "player.dir",
                            "radio.url", "radio.play_url", "radio.next_url",
                            "radio.completed_uri", "radio.promoted_uri",
                            "dlna.uri", "dlna.art_uri"}) {
        INFO("key: ", key);
        CHECK(gen::is_long_state_key(key));
    }
    // And a few that must NOT be, or the side table pays for labels.
    for (const char* key : {"audio.state", "radio.mode", "player.format", "ui.screen",
                            "bt.artist", "wifi.ssid"}) {
        INFO("key: ", key);
        CHECK_FALSE(gen::is_long_state_key(key));
    }
    // Not paths, but titles someone reads whole: an audiobook chapter name ran past the
    // 64-byte inline value on 12.10.2026, so the Bluetooth title is long like AirPlay's.
    for (const char* key : {"bt.title", "airplay.title", "dlna.title", "player.title"}) {
        INFO("key: ", key);
        CHECK(gen::is_long_state_key(key));
    }
    // The side table is sized from that list, so it can never be too small for it.
    CHECK(MODESP_LONG_STATE_ENTRIES >= (int)gen::STATE_LONG_KEY_COUNT);
}

TEST_CASE("a long key keeps the whole path; a short one is cut at the inline width") {
    SharedState st;
    const std::string path = long_path(200);

    REQUIRE(st.set("player.track", path.c_str()));
    char out[MODESP_MAX_LONG_STRING_LENGTH + 1] = "";
    REQUIRE(st.get_string("player.track", out, sizeof out));
    CHECK(std::string(out) == path);            // the whole thing, not a prefix

    // The same text under a key that is not long stops at the inline width. This is the
    // behaviour every other key has always had; what matters is that it is bounded and
    // NUL-terminated rather than overrunning.
    REQUIRE(st.set("radio.name", path.c_str()));
    char shortened[MODESP_MAX_LONG_STRING_LENGTH + 1] = "";
    REQUIRE(st.get_string("radio.name", shortened, sizeof shortened));
    CHECK(strlen(shortened) == MODESP_MAX_STRING_VALUE_LENGTH);
    CHECK(path.compare(0, MODESP_MAX_STRING_VALUE_LENGTH, shortened) == 0);
}

TEST_CASE("the map still answers for a long key, with the prefix as its value") {
    // has()/size()/get() keep working: the map holds the inline prefix as the type tag, so
    // a reader that has not been taught about the side table degrades to a short string
    // rather than to nothing.
    SharedState st;
    const std::string path = long_path(200);
    REQUIRE(st.set("player.track", path.c_str()));

    CHECK(st.has("player.track"));
    CHECK(st.size() == 1);
    auto v = st.get("player.track");
    REQUIRE(v.has_value());
    const auto* sp = etl::get_if<StringValue>(&v.value());
    REQUIRE(sp != nullptr);
    CHECK(sp->size() == MODESP_MAX_STRING_VALUE_LENGTH);
    CHECK(path.compare(0, MODESP_MAX_STRING_VALUE_LENGTH, sp->c_str()) == 0);
}

TEST_CASE("change detection compares the side table, not the prefix") {
    // Two URLs that share a long prefix and differ in the query string. Comparing the map's
    // 64-char value would call the second write "unchanged": no version bump, no WS delta,
    // and in dlna_module a track whose completion is never recognised.
    SharedState st;
    const std::string base = long_path(120);
    const std::string a = base + "?session=1111";
    const std::string b = base + "?session=2222";

    REQUIRE(st.set("radio.play_url", a.c_str()));
    const uint32_t v1 = st.version();

    REQUIRE(st.set("radio.play_url", b.c_str()));
    CHECK(st.version() == v1 + 1);

    char out[MODESP_MAX_LONG_STRING_LENGTH + 1] = "";
    REQUIRE(st.get_string("radio.play_url", out, sizeof out));
    CHECK(std::string(out) == b);

    // Writing the same text again is not a change.
    const uint32_t v2 = st.version();
    REQUIRE(st.set("radio.play_url", b.c_str()));
    CHECK(st.version() == v2);
}

TEST_CASE("iteration hands the serialiser the whole text") {
    SharedState st;
    const std::string path = long_path(200);
    REQUIRE(st.set("player.track", path.c_str()));
    REQUIRE(st.set("player.format", "FLAC 16/44.1"));
    REQUIRE(st.set("audio.volume", (int32_t)30));

    std::vector<Seen> seen;
    st.for_each(collect, &seen);
    CHECK(seen.size() == 3);

    const Seen* track = find(seen, "player.track");
    REQUIRE(track != nullptr);
    CHECK(track->is_string);
    CHECK(track->text == path);                 // the side table's copy, not the prefix

    const Seen* fmt = find(seen, "player.format");
    REQUIRE(fmt != nullptr);
    CHECK(fmt->text == "FLAC 16/44.1");         // a short key: the variant's own buffer

    const Seen* vol = find(seen, "audio.volume");
    REQUIRE(vol != nullptr);
    CHECK_FALSE(vol->is_string);
    CHECK(vol->text.empty());                   // nullptr for a number
}

TEST_CASE("the delta iteration carries long values too") {
    SharedState st;
    const std::string path = long_path(180);
    REQUIRE(st.set("dlna.uri", path.c_str()));

    std::vector<Seen> seen;
    CHECK(st.for_each_changed_and_clear(collect, &seen));
    REQUIRE(seen.size() == 1);
    CHECK(seen[0].text == path);

    seen.clear();
    CHECK_FALSE(st.for_each_changed_and_clear(collect, &seen));
}

TEST_CASE("removing a long key removes its side-table entry") {
    // The side table is the value, not a cache of it. An entry left behind would be handed
    // back by get_string() after the key was removed and re-set.
    SharedState st;
    REQUIRE(st.set("dlna.art_uri", long_path(150).c_str()));
    CHECK(st.long_count() == 1);

    CHECK(st.remove("dlna.art_uri"));
    CHECK(st.long_count() == 0);
    char out[64] = "sentinel";
    CHECK_FALSE(st.get_string("dlna.art_uri", out, sizeof out));
    CHECK(out[0] == 0);

    REQUIRE(st.set("dlna.art_uri", "http://short/art.jpg"));
    REQUIRE(st.get_string("dlna.art_uri", out, sizeof out));
    CHECK(std::string(out) == "http://short/art.jpg");
}

TEST_CASE("clear() empties the side table with the map") {
    SharedState st;
    REQUIRE(st.set("player.track", long_path(120).c_str()));
    REQUIRE(st.set("player.dir", long_path(100).c_str()));
    CHECK(st.long_count() == 2);
    st.clear();
    CHECK(st.size() == 0);
    CHECK(st.long_count() == 0);
}

TEST_CASE("the side table holds one entry per key, not one per write") {
    SharedState st;
    for (int i = 0; i < 50; i++) {
        REQUIRE(st.set("player.track", (long_path(120) + std::to_string(i)).c_str()));
    }
    CHECK(st.long_count() == 1);
    CHECK(st.set_failures() == 0);
}

TEST_CASE("get_string bounds its output and always terminates") {
    SharedState st;
    const std::string path = long_path(200);
    REQUIRE(st.set("player.track", path.c_str()));

    char small[16];
    memset(small, 'X', sizeof small);
    CHECK(st.get_string("player.track", small, sizeof small));
    CHECK(strlen(small) == sizeof(small) - 1);
    CHECK(path.compare(0, sizeof(small) - 1, small) == 0);

    // Missing key: false, and the buffer emptied rather than left with the caller's junk.
    char out[8] = "keepme";
    CHECK_FALSE(st.get_string("player.nothing", out, sizeof out));
    CHECK(out[0] == 0);

    // A non-string value is not a string, however much the caller wants one.
    REQUIRE(st.set("audio.volume", (int32_t)30));
    CHECK_FALSE(st.get_string("audio.volume", out, sizeof out));
}

TEST_CASE("a NULL string value is stored as empty rather than dereferenced") {
    SharedState st;
    CHECK(st.set("player.track", (const char*)nullptr));
    char out[32] = "x";
    CHECK(st.get_string("player.track", out, sizeof out));
    CHECK(out[0] == 0);
}

// ── Text that is not ASCII ──
//
// Seen on 12.10.2026: bt.title over the WebSocket ended in "…Слаане�". The BT module cut the
// phone's title to 63 bytes on a byte count, the cut fell inside "ц", and the JSON writer
// replaced the half letter with U+FFFD. Every string reaches the store through
// set(const char*), so that is where the cut is made on a character boundary — for the
// inline keys (bt.artist, radio.name, ...) and the long ones (bt.title, dlna.*, player.*) alike.

namespace {

/// Cyrillic, two bytes a letter. `lead` ASCII bytes in front move the letter boundaries to
/// odd offsets, so a cut at an even width (64, 256) lands inside a letter.
std::string cyrillic(size_t letters, size_t lead = 0) {
    static const char* const word[] = {"С", "л", "а", "а", "н", "е", "ц", "ь"};
    std::string s(lead, 'a');
    for (size_t i = 0; i < letters; ++i) s += word[i % 8];
    return s;
}

/// `got` is a prefix of `whole` that ends where a character starts (or at its end).
bool whole_letters_of(const std::string& got, const std::string& whole) {
    if (whole.compare(0, got.size(), got) != 0) return false;
    return got.size() == whole.size() || ((unsigned char)whole[got.size()] & 0xC0) != 0x80;
}

}  // namespace

TEST_CASE("a short key cuts a long name before the letter that does not fit") {
    SharedState st;
    const std::string title = cyrillic(60, 1);   // 121 bytes; letters start at odd offsets
    REQUIRE(st.set("bt.artist", title.c_str()));

    char out[MODESP_MAX_LONG_STRING_LENGTH + 1] = "";
    REQUIRE(st.get_string("bt.artist", out, sizeof out));
    CHECK(strlen(out) == MODESP_MAX_STRING_VALUE_LENGTH - 1);   // 64 would end inside a letter
    CHECK(whole_letters_of(out, title));
}

TEST_CASE("a name the source already cut mid-letter is stored without the half") {
    // Exactly what the BT module used to hand over: 63 bytes of a longer name, the 63rd
    // being the first byte of a letter. The store has room for 64; the letter is still half.
    SharedState st;
    const std::string title = cyrillic(60);
    const std::string cut = title.substr(0, MODESP_MAX_STRING_VALUE_LENGTH - 1);
    REQUIRE(((unsigned char)cut.back() & 0xC0) == 0xC0);       // it IS a lead byte

    REQUIRE(st.set("bt.artist", cut.c_str()));
    char out[MODESP_MAX_LONG_STRING_LENGTH + 1] = "";
    REQUIRE(st.get_string("bt.artist", out, sizeof out));
    CHECK(strlen(out) == MODESP_MAX_STRING_VALUE_LENGTH - 2);
    CHECK(whole_letters_of(out, title));

    // And it is the same value as the clean cut: no second WS delta for the same name.
    const uint32_t v = st.version();
    REQUIRE(st.set("bt.artist", title.substr(0, MODESP_MAX_STRING_VALUE_LENGTH - 2).c_str()));
    CHECK(st.version() == v);

    // The same on a long key, where the half letter sits far inside the room.
    REQUIRE(st.set("bt.title", cut.c_str()));
    REQUIRE(st.get_string("bt.title", out, sizeof out));
    CHECK(strlen(out) == MODESP_MAX_STRING_VALUE_LENGTH - 2);
    const uint32_t lv = st.version();
    REQUIRE(st.set("bt.title", title.substr(0, MODESP_MAX_STRING_VALUE_LENGTH - 2).c_str()));
    CHECK(st.version() == lv);
}

TEST_CASE("a long key cuts on a letter too, in the side table and in the inline prefix") {
    SharedState st;
    const std::string title = cyrillic(200, 1);  // 401 bytes, over the long width as well
    REQUIRE(st.set("player.title", title.c_str()));

    char out[MODESP_MAX_LONG_STRING_LENGTH + 1] = "";
    REQUIRE(st.get_string("player.title", out, sizeof out));
    CHECK(strlen(out) == MODESP_MAX_LONG_STRING_LENGTH - 1);
    CHECK(whole_letters_of(out, title));

    // The map's prefix is what a reader that does not know the side table gets.
    auto v = st.get("player.title");
    REQUIRE(v.has_value());
    const auto* sp = etl::get_if<StringValue>(&v.value());
    REQUIRE(sp != nullptr);
    CHECK(sp->size() == MODESP_MAX_STRING_VALUE_LENGTH - 1);
    CHECK(whole_letters_of(sp->c_str(), title));

    // A title that fits is kept whole, every byte of it.
    const std::string fits = cyrillic(100);      // 200 bytes
    REQUIRE(st.set("player.title", fits.c_str()));
    REQUIRE(st.get_string("player.title", out, sizeof out));
    CHECK(std::string(out) == fits);
}

TEST_CASE("get_string into a narrower buffer hands over whole letters") {
    // The screen reads the 256-byte title keys into 96-byte fields and radio.now_playing into
    // 128. A byte-count copy there was the same half letter, one step later.
    SharedState st;
    const std::string title = cyrillic(100);     // 200 bytes, letters at even offsets
    REQUIRE(st.set("dlna.title", title.c_str()));

    char field[96];                              // room for 95: inside the 48th letter
    REQUIRE(st.get_string("dlna.title", field, sizeof field));
    CHECK(strlen(field) == 94);
    CHECK(whole_letters_of(field, title));

    char two[2];                                 // room for one byte: no letter fits
    REQUIRE(st.get_string("dlna.title", two, sizeof two));
    CHECK(two[0] == '\0');
}
