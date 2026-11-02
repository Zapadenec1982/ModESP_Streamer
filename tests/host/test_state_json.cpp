/**
 * @file test_state_json.cpp
 * @brief HOST tests for the state as JSON (components/modesp_net, state_json.h): the one writer of
 *        GET /api/state and every WebSocket message. A string is escaped and whole, a float that
 *        is not finite is null rather than the `nan` that made a page drop the whole message, and
 *        an object that does not fit fails instead of going out cut.
 */

#include "doctest.h"

#include "modesp/net/state_json.h"

#include <cmath>
#include <cstring>
#include <string>

using namespace modesp;
using namespace modesp::net;

namespace {

std::string one(const char* key, const StateValue& v, const char* text = nullptr, size_t size = 256) {
    std::string buf(size, '\0');
    StateJson j = state_json_begin(&buf[0], size);
    state_json_entry(StateKey(key), v, text, &j);
    if (!state_json_end(j)) return "<failed>";
    return std::string(buf.c_str());
}

}  // namespace

TEST_CASE("state JSON: each type as the page reads it") {
    CHECK(one("audio.volume", StateValue(int32_t(-3))) == R"({"audio.volume":-3})");
    CHECK(one("audio.mute", StateValue(true)) == R"({"audio.mute":true})");
    CHECK(one("audio.vol_eff", StateValue(0.5f)) == R"({"audio.vol_eff":0.50})");
    CHECK(one("radio.name", StateValue(StringValue("a\"b\\c")), "a\"b\\c") ==
          R"({"radio.name":"a\"b\\c"})");
}

TEST_CASE("state JSON: a float that is not finite is null, never nan") {
    CHECK(one("audio.lim_gr", StateValue(std::nanf(""))) == R"({"audio.lim_gr":null})");
    CHECK(one("audio.lim_gr", StateValue(INFINITY)) == R"({"audio.lim_gr":null})");
}

TEST_CASE("state JSON: a long string comes from the text, whole") {
    const std::string path(200, 'x');
    CHECK(one("player.track", StateValue(StringValue("xx")), path.c_str(), 512) ==
          "{\"player.track\":\"" + path + "\"}");
}

TEST_CASE("state JSON: entries are separated, and one that does not fit fails the object") {
    char buf[64];
    StateJson j = state_json_begin(buf, sizeof(buf));
    state_json_entry(StateKey("a"), StateValue(int32_t(1)), nullptr, &j);
    state_json_entry(StateKey("b"), StateValue(false), nullptr, &j);
    REQUIRE(state_json_end(j));
    CHECK(std::string(buf) == R"({"a":1,"b":false})");

    char small[20];
    StateJson k = state_json_begin(small, sizeof(small));
    state_json_entry(StateKey("radio.name"), StateValue(StringValue("abcdefgh")), "abcdefgh", &k);
    CHECK_FALSE(state_json_end(k));
}
