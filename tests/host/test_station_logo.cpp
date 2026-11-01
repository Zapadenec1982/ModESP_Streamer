/**
 * @file test_station_logo.cpp
 * @brief The presets' logos kept on the panel: names, files, which stations want a fetch, the
 *        shelf, when a file is written, and the lookup that finds several stations' favicons at
 *        once.
 *
 * components/radio_catalog/include/station_logo.h and the byuuid half of radio_catalog.h. The
 * uuid is checked because it becomes a file name; a file is one this firmware wrote or nothing;
 * a preset set read again keeps what it knew about each station, so a dead favicon waits out its
 * backoff instead of being asked for every time the list moves. A logo waiting for its file is
 * not fetched again, and the file waits for silence. The favicon cases run on the recorded
 * top_ua_24.json, whose values were read out by a separate script.
 */

#include "doctest.h"
#include "radio_catalog.h"
#include "radio_catalog_ui.h"
#include "station_logo.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace station_logo;

namespace {

const char* const A = "961949f3-0601-11e8-ae97-52543be04c81";   // Hit FM
const char* const B = "1f900b9a-ea3b-4eac-9505-1df5bef175be";   // Lux FM
const char* const C = "d813e74b-48dd-4ef5-b842-b6bf11c759cf";
const char* const D = "9a4c6adc-d939-4086-9913-88739513caf2";
const char* const E = "55275efc-1ee2-4a52-9aef-7fb632c7f3e8";
const char* const F = "4748a2a9-2f27-4e4d-a15f-a91448a75830";
const char* const G = "02aa69ab-0485-4ff1-a0f1-a6c4be29a8bb";

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

struct Scratch {
    std::vector<long long> mem;
    explicit Scratch(const std::string& json)
        : mem((radio_catalog::scratch_bytes(json.data(), json.size()) + sizeof(long long) - 1) /
              sizeof(long long)) {}
    void*  data() { return mem.data(); }
    size_t size() const { return mem.size() * sizeof(long long); }
};

std::vector<uint16_t> pixels(uint16_t seed) {
    std::vector<uint16_t> px(PIXELS);
    for (int i = 0; i < PIXELS; i++) px[i] = (uint16_t)(seed + i * 7);
    return px;
}

}  // namespace

TEST_CASE("station logo: only a Radio Browser uuid names a file") {
    CHECK(valid_uuid(A));
    CHECK_FALSE(valid_uuid(nullptr));
    CHECK_FALSE(valid_uuid(""));
    CHECK_FALSE(valid_uuid("961949F3-0601-11E8-AE97-52543BE04C81"));    // not as the service writes it
    CHECK_FALSE(valid_uuid("961949f3-0601-11e8-ae97-52543be04c8"));     // one short
    CHECK_FALSE(valid_uuid("961949f3-0601-11e8-ae97-52543be04c811"));   // one long
    CHECK_FALSE(valid_uuid("961949f30-601-11e8-ae97-52543be04c81"));    // a dash out of place
    CHECK_FALSE(valid_uuid("../../../../userdata/stations.json00000"));
    CHECK_FALSE(valid_uuid("961949f3-0601-11e8-ae97-52543be0/c81"));
}

TEST_CASE("station logo: a file's name is its uuid and .565, and nothing else reads as one") {
    char name[64], uuid[UUID_LEN];
    REQUIRE(file_name(A, name, sizeof(name)));
    CHECK(std::string(name) == std::string(A) + ".565");
    REQUIRE(uuid_of(name, uuid, sizeof(uuid)));
    CHECK(std::string(uuid) == A);

    CHECK_FALSE(file_name("not-a-uuid", name, sizeof(name)));
    CHECK_FALSE(file_name(A, name, 40));                              // no room for the terminator
    CHECK_FALSE(uuid_of((std::string(A) + ".565.new").c_str(), uuid, sizeof(uuid)));   // safe_file's candidate
    CHECK_FALSE(uuid_of((std::string(A) + ".png").c_str(), uuid, sizeof(uuid)));
    CHECK_FALSE(uuid_of("961949F3-0601-11E8-AE97-52543BE04C81.565", uuid, sizeof(uuid)));
    CHECK_FALSE(uuid_of(".565", uuid, sizeof(uuid)));
    CHECK_FALSE(uuid_of(name, uuid, 10));
}

TEST_CASE("station logo: a file is a header and the pixels, and only such a file decodes") {
    const auto px = pixels(0x1234);
    std::vector<uint8_t> file(FILE_BYTES);
    REQUIRE(encode(px.data(), file.data(), file.size()) == FILE_BYTES);
    CHECK(FILE_BYTES == 3208);
    CHECK(std::string((const char*)file.data(), 4) == "SLG1");
    CHECK((file[4] == 40 && file[5] == 0 && file[6] == 40 && file[7] == 0));
    CHECK((file[8] == 0x34 && file[9] == 0x12));                     // little-endian, as RGB565 is drawn

    std::vector<uint16_t> back(PIXELS);
    REQUIRE(decode(file.data(), file.size(), back.data()));
    CHECK(back == px);

    CHECK(encode(px.data(), file.data(), FILE_BYTES - 1) == 0);
    CHECK_FALSE(decode(file.data(), FILE_BYTES - 1, back.data()));   // cut short
    std::vector<uint8_t> longer(file);
    longer.push_back(0);
    CHECK_FALSE(decode(longer.data(), longer.size(), back.data()));
    std::vector<uint8_t> other(file);
    other[3] = '2';
    CHECK_FALSE(decode(other.data(), other.size(), back.data()));    // another format
    other = file;
    other[4] = 48;
    CHECK_FALSE(decode(other.data(), other.size(), back.data()));    // another size
}

TEST_CASE("station logo: the presets' uuids become the wanted set, each once") {
    Plan p;
    const char* slots[6] = {A, "", nullptr, "typed-in", A, B};
    CHECK(p.want(slots, 6));
    REQUIRE(p.count() == 2);
    CHECK(std::string(p.at(0).uuid) == A);
    CHECK(std::string(p.at(1).uuid) == B);
    CHECK(p.at(0).state == State::NEW);
    CHECK(p.find(B) == 1);
    CHECK(p.find(C) == -1);
    CHECK(p.find(nullptr) == -1);

    // The same set in another order is no change, and each keeps what it was.
    p.set(0, State::HAVE);
    p.wait(1, 100, 50);
    const char* again[6] = {B, A, "", "", "", ""};
    CHECK_FALSE(p.want(again, 6));
    CHECK(p.at(p.find(A)).state == State::HAVE);
    CHECK(p.at(p.find(B)).state == State::WAIT);
    CHECK(p.at(p.find(B)).retry_us == 150);

    // One gone, one new: a change; the one that stayed keeps its wait.
    const char* moved[6] = {B, C, "", "", "", ""};
    CHECK(p.want(moved, 6));
    REQUIRE(p.count() == 2);
    CHECK(p.find(A) == -1);
    CHECK(p.at(p.find(B)).state == State::WAIT);
    CHECK(p.at(p.find(C)).state == State::NEW);

    // Never more than one per preset.
    const char* many[7] = {A, B, C, D, E, F, G};
    p.want(many, 7);
    CHECK(p.count() == WANT);
    CHECK(p.find(G) == -1);

    const char* none[6] = {"", "", "", "", "", ""};
    CHECK(p.want(none, 6));
    CHECK(p.count() == 0);
    CHECK_FALSE(p.want(none, 6));
}

TEST_CASE("station logo: what is due a fetch, and when a failed one is due again") {
    Plan p;
    const char* slots[5] = {A, B, C, D, E};
    p.want(slots, 5);
    p.set(0, State::NEED);
    p.set(1, State::HAVE);
    p.set(2, State::BUSY);
    p.wait(3, 1000, RETRY_SOON_US);
    p.wait(4, 1000, RETRY_LATE_US);
    int idx[WANT];
    REQUIRE(p.due(1000, idx, WANT) == 1);                  // NEW, HAVE, BUSY and the waiting are not
    CHECK(idx[0] == 0);
    CHECK(p.due(1000 + RETRY_SOON_US - 1, idx, WANT) == 1);
    REQUIRE(p.due(1000 + RETRY_SOON_US, idx, WANT) == 2);  // ten minutes on: the soon one
    CHECK(idx[1] == 3);
    CHECK(p.due(1000 + RETRY_LATE_US, idx, WANT) == 3);    // six hours on: the late one too
    CHECK(p.due(1000 + RETRY_LATE_US, idx, 1) == 1);       // never more than asked for
    CHECK(RETRY_SOON_US == 600LL * 1000000);
    CHECK(RETRY_LATE_US == 6LL * 3600 * 1000000);
}

TEST_CASE("station logo: a kept logo waits for its file, and is not fetched again meanwhile") {
    Plan p;
    const char* slots[3] = {A, B, C};
    p.want(slots, 3);
    p.set(0, State::HAVE);                                 // read out of its file: nothing to write
    p.set(1, State::NEED);
    p.set(2, State::NEED);
    CHECK(p.to_save() == -1);
    CHECK_FALSE(p.at(0).unsaved);

    p.kept(1);                                             // B's logo arrived
    CHECK(p.at(1).state == State::HAVE);
    CHECK(p.at(1).unsaved);
    CHECK(p.to_save() == 1);
    int idx[WANT];
    REQUIRE(p.due(0, idx, WANT) == 1);                     // only C: B's is on the shelf, unwritten
    CHECK(idx[0] == 2);

    // The same set in another order: B's file is still to be written.
    const char* again[3] = {C, B, A};
    CHECK_FALSE(p.want(again, 3));
    REQUIRE(p.to_save() == p.find(B));
    p.saved(p.find(B));
    CHECK(p.to_save() == -1);
    CHECK(p.at(p.find(B)).state == State::HAVE);

    // Several wait, each once; written in turn.
    p.kept(p.find(A));
    p.kept(p.find(C));
    const int first = p.to_save();
    REQUIRE(first >= 0);
    p.saved(first);
    const int second = p.to_save();
    REQUIRE(second >= 0);
    CHECK(second != first);
    p.saved(second);
    CHECK(p.to_save() == -1);

    // A station that leaves the presets before its file is written is not written; back again,
    // it is new — its file looked for, and fetched when there is none, as after a restart.
    p.kept(p.find(A));
    const char* fewer[2] = {B, C};
    CHECK(p.want(fewer, 2));
    CHECK(p.to_save() == -1);
    p.want(slots, 3);
    CHECK(p.at(p.find(A)).state == State::NEW);
    CHECK_FALSE(p.at(p.find(A)).unsaved);

    p.kept(-1);                                            // out of range: nothing
    p.saved(WANT);
    CHECK(p.to_save() == -1);
}

TEST_CASE("station logo: files wait until nothing has been heard for four seconds") {
    CHECK(QUIET_MS == 4000);
    Quiet q;
    for (int i = 0; i < 7; i++) CHECK_FALSE(q.tick(500, true));   // 3.5 s
    CHECK(q.tick(500, true));                                       // 4 s
    CHECK(q.tick(500, true));                                       // and on
    CHECK_FALSE(q.tick(500, false));                                // a sound: from the start
    CHECK_FALSE(q.tick(3999, true));
    CHECK(q.tick(1, true));
    CHECK(q.tick(UINT32_MAX, true));                                // held, not wrapped
    CHECK(q.tick(500, true));
    CHECK_FALSE(q.tick(0, false));
    CHECK(q.tick(UINT32_MAX, true));                                // one long pass is enough
}

TEST_CASE("station logo: the shelf holds one logo per wanted station") {
    // Heap, not stack: six logos are 19 KB, as on the device where the shelf is in PSRAM.
    std::vector<Shelf> mem(1);
    Shelf& s = mem[0];
    const auto pa = pixels(1), pb = pixels(2), pc = pixels(3);
    CHECK(s.find(A) == nullptr);
    CHECK(s.put(A, pa.data()));
    CHECK(s.put(B, pb.data()));
    CHECK(s.count == 2);
    REQUIRE(s.find(B) != nullptr);
    CHECK(s.find(B)->px[5] == pb[5]);
    CHECK(s.put(A, pc.data()));                             // a new logo in the same place
    CHECK(s.count == 2);
    CHECK(s.find(A)->px[5] == pc[5]);
    CHECK_FALSE(s.put("typed-in", pa.data()));
    CHECK(s.find("") == nullptr);

    const char* more[4] = {C, D, E, F};
    for (const char* u : more) CHECK(s.put(u, pa.data()));
    CHECK(s.count == WANT);
    CHECK_FALSE(s.put(G, pa.data()));                     // full

    Plan p;
    const char* kept[6] = {B, D, "", "", "", ""};
    p.want(kept, 6);
    s.keep(p);
    CHECK(s.count == 2);
    REQUIRE(s.find(B) != nullptr);
    CHECK(s.find(B)->px[5] == pb[5]);
    CHECK(s.find(D) != nullptr);
    CHECK(s.find(A) == nullptr);
    CHECK(s.item[2].uuid[0] == '\0');                      // what went is cleared, not left behind
}

TEST_CASE("station logo: the shelf the Player reads moves by two when it is published") {
    auto& pub = radio_catalog::logo_shelf();
    std::vector<Shelf> mem(2);
    const uint32_t g0 = pub.generation();
    const auto px = pixels(9);
    mem[0].put(A, px.data());
    pub.set(mem[0]);
    CHECK(pub.generation() == g0 + 2);
    REQUIRE(pub.get(mem[1]));
    REQUIRE(mem[1].find(A) != nullptr);
    CHECK(mem[1].find(A)->px[100] == px[100]);
}

TEST_CASE("station logo: one lookup asks for several stations") {
    char path[320];
    const char* two[2] = {A, B};
    REQUIRE(radio_catalog::byuuid_path(two, 2, path, sizeof(path)) > 0);
    CHECK(std::string(path) == std::string("/json/stations/byuuid?uuids=") + A + "," + B);
    const char* six[6] = {A, B, C, D, E, F};
    const size_t n = radio_catalog::byuuid_path(six, 6, path, sizeof(path));
    CHECK(n == 28 + 6 * 36 + 5);                          // the request a full set of presets makes
    CHECK(radio_catalog::byuuid_path(six, 6, path, 100) == 0);
    const char* gap[2] = {A, ""};
    CHECK(radio_catalog::byuuid_path(gap, 2, path, sizeof(path)) == 0);
    CHECK(radio_catalog::byuuid_path(two, 0, path, sizeof(path)) == 0);
}

TEST_CASE("station logo: a byuuid answer gives each station's uuid and favicon") {
    using radio_catalog::Favicon;
    const std::string json = slurp("tests/host/fixtures/radio_catalog/top_ua_24.json");
    REQUIRE(!json.empty());
    Scratch s(json);
    std::vector<Favicon> f(24);
    REQUIRE(radio_catalog::parse_favicons(json.data(), json.size(), s.data(), s.size(), f.data(), 24) == 24);
    CHECK(std::string(f[0].uuid) == A);
    CHECK(std::string(f[0].url) == "https://www.hitfm.ua/static/img/fav-icon/apple-icon-120x120.png");
    CHECK(std::string(f[2].uuid) == "bd9c1301-562d-451f-94bf-6a76e3bb898e");
    CHECK(f[2].url[0] == '\0');                          // listed with none
    CHECK(std::string(f[17].url) == "https://abs.twimg.com/favicons/twitter.2.ico");
    int with = 0;
    for (const Favicon& x : f) with += x.url[0] != '\0';
    CHECK(with == 13);
    CHECK(radio_catalog::parse_favicons(json.data(), json.size(), s.data(), s.size(), f.data(), 3) == 3);

    // A station with no uuid is no answer to anything; an address too long for a row is none.
    const std::string odd = "[{\"name\":\"x\",\"favicon\":\"http://a/b.png\"},"
                            "{\"stationuuid\":\"" + std::string(B) + "\",\"favicon\":\"http://h/" +
                            std::string(300, 'a') + ".png\"}]";
    Scratch s2(odd);
    REQUIRE(radio_catalog::parse_favicons(odd.data(), odd.size(), s2.data(), s2.size(), f.data(), 24) == 1);
    CHECK(std::string(f[0].uuid) == B);
    CHECK(f[0].url[0] == '\0');

    const std::string obj = "{\"stationuuid\":\"x\"}";
    Scratch s3(obj);
    CHECK(radio_catalog::parse_favicons(obj.data(), obj.size(), s3.data(), s3.size(), f.data(), 24) == -1);
}
