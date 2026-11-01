/**
 * @file test_bt_web.cpp
 * @brief GET and POST /api/bt bodies (modules/bt: bt_web) — what the web is shown of the
 *        satellite's Bluetooth settings and a pairing, and what it may ask for.
 */

#include "doctest.h"
#include "bt_web.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace bt_web;

namespace {

bool parse_s(const char* s, Edit& e) { return parse(s, strlen(s), e); }

bt_settings::View sample_view() {
    bt_settings::View v;
    v.linked = v.settings = v.known = true;
    v.radio = true;
    v.discoverable = false;
    v.auto_reconnect = true;
    v.codecs_supported = 0x1F;
    v.codecs_enabled = 0x0B;
    snprintf(v.name, sizeof(v.name), "P4 \"Player\"");
    v.total = 2;
    v.count = 2;
    const uint8_t a0[6] = {0xA4, 0x50, 0x46, 0x01, 0x02, 0x0F};
    memcpy(v.bonds[0].addr, a0, 6);
    v.bonds[0].flags = 3;                       // connected, last
    snprintf(v.bonds[0].name, sizeof(v.bonds[0].name), "Pixel 8");
    v.bonds[1].addr[5] = 1;
    snprintf(v.bonds[1].name, sizeof(v.bonds[1].name), "Телефон");
    return v;
}

}  // namespace

TEST_CASE("bt web: an address both ways, as the panel prints it") {
    const uint8_t a[6] = {0xA4, 0x50, 0x46, 0x01, 0x02, 0x0F};
    char s[18];
    addr_text(a, s, sizeof(s));
    CHECK(std::string(s) == "A4:50:46:01:02:0F");
    uint8_t b[6] = {};
    REQUIRE(parse_addr(s, 17, b));
    CHECK(memcmp(a, b, 6) == 0);
    CHECK(parse_addr("a4:50:46:01:02:0f", 17, b));           // either case
    CHECK_FALSE(parse_addr("A4-50-46-01-02-0F", 17, b));
    CHECK_FALSE(parse_addr("A4:50:46:01:02:0", 16, b));
    CHECK_FALSE(parse_addr("A4:50:46:01:02:0G", 17, b));
}

TEST_CASE("bt web: GET carries the view, the phones and the pairing, names escaped") {
    const bt_settings::View v = sample_view();
    bt_settings::Pairing p;
    p.seq = 7;
    p.active = true;
    p.kind = 0;
    p.passkey = 123456;
    snprintf(p.name, sizeof(p.name), "Pixel 8");
    p.by = bt_settings::Source::WEB;
    p.accepted = true;
    char buf[4096];
    const size_t n = json(v, p, true, buf, sizeof(buf));
    REQUIRE(n > 0);
    const std::string s(buf, n);
    CHECK(s.find("\"linked\":true,\"settings\":true,\"known\":true,") != std::string::npos);
    CHECK(s.find("\"radio\":true,\"discoverable\":false,\"auto_reconnect\":true,") != std::string::npos);
    CHECK(s.find("\"codecs_supported\":31,\"codecs_enabled\":11,") != std::string::npos);
    CHECK(s.find("\"name\":\"P4 \\\"Player\\\"\"") != std::string::npos);
    CHECK(s.find("{\"addr\":\"A4:50:46:01:02:0F\",\"name\":\"Pixel 8\",\"connected\":true,\"last\":true}") != std::string::npos);
    CHECK(s.find("\"name\":\"Телефон\",\"connected\":false,\"last\":false}") != std::string::npos);
    CHECK(s.find("\"pairing\":{\"seq\":7,\"active\":true,\"kind\":0,\"name\":\"Pixel 8\",\"code\":123456,"
                 "\"result\":255,\"by\":2,\"accepted\":true}") != std::string::npos);
    CHECK(s.rfind("\"confirmer\":true}") == n - 17);
    CHECK(json(v, p, true, buf, 100) == 0);                   // never half a body

    p.active = false;                                         // over: the code goes with it
    const size_t m = json(v, p, false, buf, sizeof(buf));
    CHECK(std::string(buf, m).find("\"code\":0,") != std::string::npos);
}

TEST_CASE("bt web: the switches, alone or together; what is not given stays") {
    Edit e;
    REQUIRE(parse_s(R"({"discoverable":true})", e));
    CHECK(e.discoverable == 1);
    CHECK(e.radio == -1);
    CHECK(e.auto_reconnect == -1);
    CHECK(e.codecs == -1);
    CHECK(e.any_switch());
    CHECK(e.needs_settings());

    REQUIRE(parse_s(R"({"radio":false,"auto_reconnect":true,"codecs":11})", e));
    CHECK(e.radio == 0);
    CHECK(e.auto_reconnect == 1);
    CHECK(e.codecs == 11);

    CHECK_FALSE(parse_s(R"({"radio":1})", e));              // a bool is a bool
    CHECK_FALSE(parse_s(R"({"codecs":256})", e));
    CHECK_FALSE(parse_s(R"({"codecs":-1})", e));
    CHECK_FALSE(parse_s(R"({})", e));                       // nothing asked
    CHECK_FALSE(parse_s(R"({"radio":{"on":true}})", e));
    CHECK_FALSE(parse_s(R"([true])", e));
}

TEST_CASE("bt web: a name in UTF-8, escapes read, the limits the satellite has") {
    Edit e;
    REQUIRE(parse_s(R"({"name":"Кухня"})", e));
    CHECK(e.rename);
    CHECK(std::string(e.name) == "Кухня");
    REQUIRE(parse_s(R"({"name":"P4 \"A\" \\ é 😀"})", e));
    CHECK(std::string(e.name) == "P4 \"A\" \\ \xc3\xa9 \xf0\x9f\x98\x80");
    REQUIRE(parse_s(R"({"name":"12345678901234567890123456789012"})", e));   // 32 bytes
    CHECK_FALSE(parse_s(R"({"name":"123456789012345678901234567890123"})", e));
    CHECK_FALSE(parse_s(R"({"name":""})", e));
    CHECK_FALSE(parse_s(R"({"name":"a\nb"})", e));             // no control characters
    CHECK_FALSE(parse_s(R"({"name":"a\u0007"})", e));
    CHECK_FALSE(parse_s(R"({"name":"\ud83d"})", e));            // half a pair
    CHECK_FALSE(parse_s(R"({"name":"\ude00"})", e));
    CHECK_FALSE(parse_s(R"({"name":"\x41"})", e));
    CHECK_FALSE(parse_s(R"({"name":7})", e));
}

TEST_CASE("bt web: raw bytes that are not UTF-8 are not a name") {
    // The satellite refuses such a name and answers with its old one, and the panel used to keep
    // it in NVS and give it back on every BT_CONFIG: a ping-pong at wire speed that a reboot kept.
    Edit e;
    CHECK_FALSE(parse_s("{\"name\":\"\xFF\"}", e));
    CHECK_FALSE(parse_s("{\"name\":\"ab\x80\"}", e));                  // a stray continuation
    CHECK_FALSE(parse_s("{\"name\":\"\xD0\"}", e));                    // half a letter
    CHECK_FALSE(parse_s("{\"name\":\"\xE2\x82\"}", e));
    CHECK_FALSE(parse_s("{\"name\":\"\xC0\x80\"}", e));                // an overlong NUL
    CHECK_FALSE(parse_s("{\"name\":\"\xED\xA0\x80\"}", e));            // a surrogate
    CHECK_FALSE(parse_s("{\"name\":\"\xF4\x90\x80\x80\"}", e));        // past U+10FFFF
    CHECK_FALSE(parse_s("{\"name\":\"\xF8\x88\x80\x80\x80\"}", e));
    CHECK_FALSE(e.rename);
    // Raw UTF-8 is a name as much as escaped: 16 Cyrillic letters are the 32 bytes allowed.
    REQUIRE(parse_s("{\"name\":\"\xD0\x9A\xD1\x83\xD1\x85\xD0\xBD\xD1\x8F \xE2\x82\xAC\"}", e));
    CHECK(std::string(e.name) == "Кухня €");
    REQUIRE(parse_s(R"({"name":"ЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖ"})", e));
    CHECK(strlen(e.name) == 32);
    CHECK_FALSE(parse_s(R"({"name":"ЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖa"})", e));
}

TEST_CASE("bt web: forget one phone or all, disconnect, refresh") {
    Edit e;
    REQUIRE(parse_s(R"({"forget":"A4:50:46:01:02:0F"})", e));
    CHECK(e.forget);
    CHECK_FALSE(e.forget_all);
    CHECK(e.forget_addr[0] == 0xA4);
    CHECK(e.forget_addr[5] == 0x0F);
    REQUIRE(parse_s(R"({"forget":"all"})", e));
    CHECK(e.forget_all);
    CHECK_FALSE(parse_s(R"({"forget":"everything"})", e));
    CHECK_FALSE(parse_s(R"({"forget":true})", e));

    REQUIRE(parse_s(R"({"disconnect":true})", e));
    CHECK(e.disconnect);
    CHECK_FALSE(e.needs_settings());                          // works with a 1.7 satellite too
    CHECK_FALSE(parse_s(R"({"disconnect":false})", e));       // asks for nothing
    REQUIRE(parse_s(R"({"refresh":true})", e));
    CHECK(e.needs_settings());
}

TEST_CASE("bt web: a pairing answer names its request") {
    Edit e;
    REQUIRE(parse_s(R"({"pair":"accept","seq":7})", e));
    CHECK(e.pair);
    CHECK(e.accept);
    CHECK(e.seq == 7);
    REQUIRE(parse_s(R"({"seq":8,"pair":"reject"})", e));
    CHECK(e.pair);
    CHECK_FALSE(e.accept);
    CHECK(e.seq == 8);
    CHECK_FALSE(parse_s(R"({"pair":"accept"})", e));          // which request?
    CHECK_FALSE(parse_s(R"({"pair":"accept","seq":0})", e));
    CHECK_FALSE(parse_s(R"({"pair":"maybe","seq":7})", e));
    CHECK_FALSE(parse_s(R"({"pair":true,"seq":7})", e));
}
