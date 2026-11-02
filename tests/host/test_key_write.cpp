/**
 * @file test_key_write.cpp
 * @brief HOST tests for the one rule for a key written from outside (modesp/net/key_write.h):
 *        POST /api/settings, POST /api/restore and an MQTT command turn a text into the key's
 *        manifest type the same way. MQTT used to read any bool but "true"/"1" as false.
 */

#include "doctest.h"

#include "mocks/esp_log_mock.h"
#include "modesp/net/key_write.h"

#include <cstring>
#include <string>

using namespace modesp;
using namespace modesp::net;

namespace {

KeyWrite w(const char* key, std::string& text, ValueFrom from) {
    const gen::StateMeta* meta = gen::find_state_meta(key);
    REQUIRE(meta);
    return key_write(*meta, key, &text[0], from, "test");
}

KeyWrite w(const char* key, const char* text, ValueFrom from) {
    std::string t(text);
    return w(key, t, from);
}

}  // namespace

TEST_CASE("key_write: a number of the key's type, held to its range") {
    KeyWrite r = w("audio.volume", "40", ValueFrom::JSON_PRIMITIVE);
    CHECK(r.kind == KeyWrite::INT);
    CHECK(r.i == 40);
    CHECK(w("audio.volume", "5.7", ValueFrom::JSON_PRIMITIVE).i == 5);        // toward zero
    CHECK(w("audio.volume", "250", ValueFrom::MQTT_TEXT).i == 100);           // clamped
    CHECK(w("audio.volume", "-1e30", ValueFrom::JSON_PRIMITIVE).i == 0);
    for (const char* bad : {"null", "true", "abc", "-nan", "-inf", ""})
        CHECK(w("audio.volume", bad, ValueFrom::JSON_PRIMITIVE).kind == KeyWrite::REFUSED);
    CHECK(w("audio.volume", "50", ValueFrom::JSON_STRING).kind == KeyWrite::REFUSED);
}

TEST_CASE("key_write: a bool is true or false; over MQTT 1 or 0 too, nothing else") {
    CHECK(w("audio.mute", "true", ValueFrom::JSON_PRIMITIVE).b);
    CHECK(w("audio.mute", "false", ValueFrom::JSON_PRIMITIVE).kind == KeyWrite::BOOL);
    CHECK(w("audio.mute", "1", ValueFrom::JSON_PRIMITIVE).kind == KeyWrite::REFUSED);
    CHECK(w("audio.mute", "1", ValueFrom::MQTT_TEXT).b);
    KeyWrite off = w("audio.mute", "0", ValueFrom::MQTT_TEXT);
    CHECK((off.kind == KeyWrite::BOOL && !off.b));
    // an "on" used to switch it off
    CHECK(w("audio.mute", "on", ValueFrom::MQTT_TEXT).kind == KeyWrite::REFUSED);
}

TEST_CASE("key_write: a string only as a JSON string, unescaped in place") {
    std::string t = R"(Radio \"Rock\")";
    KeyWrite r = w("radio.web_name", t, ValueFrom::JSON_STRING);
    REQUIRE(r.kind == KeyWrite::STRING);
    CHECK(std::string(r.s) == "Radio \"Rock\"");
    CHECK(w("radio.web_name", "Radio", ValueFrom::MQTT_TEXT).kind == KeyWrite::REFUSED);
    CHECK(w("radio.web_name", "5", ValueFrom::JSON_PRIMITIVE).kind == KeyWrite::REFUSED);
    CHECK(w("radio.web_name", R"(\u0000)", ValueFrom::JSON_STRING).kind == KeyWrite::REFUSED);
}
