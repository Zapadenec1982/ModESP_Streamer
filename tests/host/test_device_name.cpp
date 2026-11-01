/**
 * @file test_device_name.cpp
 * @brief The panel's one name (modules/bt: device_name) — only a name the satellite takes is
 *        kept in NVS, and one it would refuse never reaches the services that announce it.
 *
 * A name the satellite refuses (its bt_name_ok) is answered with its old one, and the panel gave
 * its own back on every BT_CONFIG that disagreed: with such a name in NVS the two frames bounced
 * off each other at wire speed, and a reboot changed nothing.
 */

#include "doctest.h"
#include "device_name.h"
#include "modesp/services/nvs_helper.h"
#include "nvs.h"

#include <cstring>
#include <string>

namespace {

std::string name_now() {
    char out[device_name::CAP];
    device_name::get(out, sizeof out);
    return out;
}

bool ok(const char* s) { return device_name::valid(s, strlen(s)); }

}  // namespace

TEST_CASE("device name: valid() is the satellite's rule, 1..32 bytes of well-formed UTF-8") {
    CHECK(ok("P4 Player"));
    CHECK(ok("\xC3\xA9"));                         // é
    CHECK(ok("\xE2\x82\xAC"));                     // €
    CHECK(ok("\xF0\x9F\x8E\xB5"));                 // a musical note
    CHECK(ok("\xD0\x9A\xD1\x83\xD1\x85\xD0\xBD\xD1\x8F"));   // Кухня
    CHECK(ok("\xEF\xBF\xBF"));                     // U+FFFF: a noncharacter, still UTF-8
    CHECK(ok("\xF4\x8F\xBF\xBF"));                 // U+10FFFF, the last there is
    CHECK(ok("12345678901234567890123456789012"));
    CHECK_FALSE(ok("123456789012345678901234567890123"));
    CHECK_FALSE(ok(""));
    CHECK_FALSE(device_name::valid(nullptr, 3));
    CHECK_FALSE(device_name::valid("a\0b", 3));    // a NUL inside
    CHECK_FALSE(ok("\xFF"));
    CHECK_FALSE(ok("\x80"));                       // a stray continuation
    CHECK_FALSE(ok("\xC3"));                       // cut short
    CHECK_FALSE(ok("\xE2\x82"));
    CHECK_FALSE(ok("\xF0\x9F\x8E"));
    CHECK_FALSE(ok("\xC3" "a"));                   // a lead byte with no continuation after it
    CHECK_FALSE(ok("\xC0\x80"));                   // overlong NUL
    CHECK_FALSE(ok("\xC1\xBF"));
    CHECK_FALSE(ok("\xE0\x80\x80"));               // overlong
    CHECK_FALSE(ok("\xF0\x8F\xBF\xBF"));
    CHECK_FALSE(ok("\xED\xA0\x80"));               // a surrogate
    CHECK_FALSE(ok("\xF4\x90\x80\x80"));           // past U+10FFFF
    CHECK_FALSE(ok("\xF5\x80\x80\x80"));
}

TEST_CASE("device name: keep() refuses what the satellite would, and NVS keeps the last good one") {
    host_nvs::reset();
    CHECK_FALSE(device_name::kept());
    CHECK(name_now() == device_name::FALLBACK);

    device_name::keep("\xFF");
    CHECK_FALSE(device_name::kept());
    CHECK(host_nvs::writes() == 0);

    device_name::keep("Кухня");
    CHECK(device_name::kept());
    CHECK(name_now() == "Кухня");

    const long writes = host_nvs::writes();
    device_name::keep("\xD0");                     // half a letter
    device_name::keep("");
    device_name::keep(nullptr);
    device_name::keep("123456789012345678901234567890123");
    device_name::keep("Кухня");                    // the same: nothing written
    CHECK(host_nvs::writes() == writes);
    CHECK(name_now() == "Кухня");
}

TEST_CASE("device name: a stored name the satellite would refuse reads as none") {
    host_nvs::reset();
    REQUIRE(modesp::nvs_helper::write_str("devname", "name", "\xFF"));
    CHECK_FALSE(device_name::kept());              // so the satellite's name is taken over it
    CHECK(name_now() == device_name::FALLBACK);    // and AirPlay, DLNA and USB never announce it
    device_name::keep("Спальня");
    CHECK(device_name::kept());
    CHECK(name_now() == "Спальня");
}
