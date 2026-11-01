/**
 * @file test_eq_config.cpp
 * @brief eq.json parse and serialise — the file the EQ screen writes must be the file the
 *        audio core reads, so the round trip is what gets tested, not either half alone.
 */

#include "doctest.h"
#include "eq_config.h"
#include "eq_web.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

using namespace audio_core;

namespace {

const char* SAMPLE = R"({
  "_comment": "ignored",
  "version": 2,
  "mode": "2.1",
  "crossover": { "freq_hz": 90, "slope": "LR24" },
  "outputs": [
    {
      "id": "dac0", "label": "MAIN",
      "gain_db": -1.5, "delay_ms": 0.0, "invert": false,
      "bands": [
        {"type": "highpass",  "f": 90,   "gain": 0.0,  "q": 0.71, "on": true},
        {"type": "peak",      "f": 3150, "gain": -2.5, "q": 1.40, "on": true}
      ]
    },
    {
      "id": "dac1", "label": "SUB",
      "gain_db": 3.0, "delay_ms": 4.25, "invert": true,
      "bands": [
        {"type": "lowpass", "f": 90, "gain": 0.0, "q": 0.71, "on": true}
      ]
    }
  ]
})";

} // namespace

TEST_CASE("eq_parse: reads both sections, not just the first") {
    EqConfig c;
    REQUIRE(eq_parse(SAMPLE, strlen(SAMPLE), c));
    CHECK(std::string(c.mode) == "2.1");
    CHECK(c.crossover_hz == doctest::Approx(90.0f));
    REQUIRE(c.sections == 2);

    CHECK(std::string(c.sec[0].id) == "dac0");
    CHECK(c.sec[0].gain_db == doctest::Approx(-1.5f));
    CHECK(c.sec[0].invert == false);
    REQUIRE(c.sec[0].count == 2);
    CHECK(c.sec[0].bands[0].type == BandType::HIGH_PASS);
    CHECK(c.sec[0].bands[1].freq_hz == doctest::Approx(3150.0f));
    CHECK(c.sec[0].bands[1].gain_db == doctest::Approx(-2.5f));

    CHECK(std::string(c.sec[1].label) == "SUB");
    CHECK(c.sec[1].delay_ms == doctest::Approx(4.25f));
    CHECK(c.sec[1].invert == true);
    REQUIRE(c.sec[1].count == 1);
    CHECK(c.sec[1].bands[0].type == BandType::LOW_PASS);
}

TEST_CASE("round trip: serialise then parse gives back the same configuration") {
    EqConfig a;
    REQUIRE(eq_parse(SAMPLE, strlen(SAMPLE), a));

    char buf[4096];
    const size_t n = eq_serialize(a, buf, sizeof(buf));
    REQUIRE(n > 0);

    EqConfig b;
    REQUIRE(eq_parse(buf, n, b));

    CHECK(std::string(b.mode) == std::string(a.mode));
    CHECK(b.crossover_hz == doctest::Approx(a.crossover_hz));
    REQUIRE(b.sections == a.sections);
    for (int s = 0; s < a.sections; s++) {
        CHECK(std::string(b.sec[s].id) == std::string(a.sec[s].id));
        CHECK(std::string(b.sec[s].label) == std::string(a.sec[s].label));
        CHECK(b.sec[s].gain_db  == doctest::Approx(a.sec[s].gain_db));
        CHECK(b.sec[s].delay_ms == doctest::Approx(a.sec[s].delay_ms));
        CHECK(b.sec[s].invert   == a.sec[s].invert);
        REQUIRE(b.sec[s].count  == a.sec[s].count);
        for (int i = 0; i < a.sec[s].count; i++) {
            CHECK(b.sec[s].bands[i].type    == a.sec[s].bands[i].type);
            CHECK(b.sec[s].bands[i].freq_hz == doctest::Approx(a.sec[s].bands[i].freq_hz));
            CHECK(b.sec[s].bands[i].gain_db == doctest::Approx(a.sec[s].bands[i].gain_db));
            CHECK(b.sec[s].bands[i].q       == doctest::Approx(a.sec[s].bands[i].q).epsilon(0.01));
            CHECK(b.sec[s].bands[i].enabled == a.sec[s].bands[i].enabled);
        }
    }
}

TEST_CASE("round trip survives an edit — which is what the EQ screen does") {
    EqConfig a;
    REQUIRE(eq_parse(SAMPLE, strlen(SAMPLE), a));
    a.sec[0].bands[1].gain_db = 4.5f;
    a.sec[0].bands[1].freq_hz = 1234.0f;
    a.sec[0].bands[1].enabled = false;

    char buf[4096];
    const size_t n = eq_serialize(a, buf, sizeof(buf));
    REQUIRE(n > 0);
    EqConfig b;
    REQUIRE(eq_parse(buf, n, b));
    CHECK(b.sec[0].bands[1].gain_db == doctest::Approx(4.5f));
    CHECK(b.sec[0].bands[1].freq_hz == doctest::Approx(1234.0f));
    CHECK(b.sec[0].bands[1].enabled == false);
    // the untouched second section must come back whole — this is the whole reason the
    // serialiser models the entire file rather than only the section being edited
    REQUIRE(b.sections == 2);
    CHECK(b.sec[1].delay_ms == doctest::Approx(4.25f));
    CHECK(b.sec[1].bands[0].type == BandType::LOW_PASS);
}

TEST_CASE("round trip keeps a preset's bands exactly - 62.5 Hz and Q 0.707 included") {
    // The EQ screen recognises a preset by comparing the loaded bands with the preset's own
    // within 0.01 Hz and 0.001 of Q (eq_presets.cpp, eq_same_section). A fixed-width format that
    // rounds either one turns every preset into the user's own set after a reboot.
    EqConfig a;
    REQUIRE(eq_parse(SAMPLE, strlen(SAMPLE), a));
    a.sec[0].count = 3;
    a.sec[0].bands[0] = EqBand{BandType::LOW_SHELF, 40.0f, 3.9f, 0.707f, true};
    a.sec[0].bands[1] = EqBand{BandType::PEAK, 62.5f, -0.3f, 1.0f, true};
    a.sec[0].bands[2] = EqBand{BandType::HIGH_SHELF, 12000.0f, 2.4f, 0.707f, true};

    char buf[4096];
    const size_t n = eq_serialize(a, buf, sizeof(buf));
    REQUIRE(n > 0);
    EqConfig b;
    REQUIRE(eq_parse(buf, n, b));
    REQUIRE(b.sec[0].count == 3);
    for (int i = 0; i < 3; i++) {
        CHECK(std::fabs(b.sec[0].bands[i].freq_hz - a.sec[0].bands[i].freq_hz) <= 0.01f);
        CHECK(std::fabs(b.sec[0].bands[i].q - a.sec[0].bands[i].q) <= 0.001f);
        CHECK(std::fabs(b.sec[0].bands[i].gain_db - a.sec[0].bands[i].gain_db) <= 0.01f);
    }
}

TEST_CASE("eq_parse: garbage and truncation leave a flat EQ, not a half-read one") {
    EqConfig c;
    CHECK_FALSE(eq_parse("not json", 8, c));
    CHECK(c.sections == 0);

    const std::string truncated(SAMPLE, strlen(SAMPLE) / 2);
    EqConfig d;
    CHECK_FALSE(eq_parse(truncated.c_str(), truncated.size(), d));
    CHECK(d.sections == 0);
}

TEST_CASE("eq_parse: a field it does not know is skipped whole, however it nests") {
    // Skipping counts a value's tokens in a loop rather than by recursion. One token too few
    // or too many and what follows is misread.
    const char* json = R"({"x":[[1,{"a":[2,{}]}],[]],"mode":"2.1",
        "outputs":[{"id":"dac0","extra":{"k":[[3],{"z":null}]},"gain_db":-1.5,
                    "bands":[{"type":"peak","odd":[[[4]]],"f":1000,"gain":2,"q":1,"on":true}]}],
        "y":{"q":[[[5]]],"r":{}}})";
    EqConfig c;
    REQUIRE(eq_parse(json, strlen(json), c));
    CHECK(std::string(c.mode) == "2.1");
    REQUIRE(c.sections == 1);
    CHECK(std::string(c.sec[0].id) == "dac0");
    CHECK(c.sec[0].gain_db == doctest::Approx(-1.5));
    REQUIRE(c.sec[0].count == 1);
    CHECK(c.sec[0].bands[0].freq_hz == doctest::Approx(1000));
    CHECK(c.sec[0].bands[0].gain_db == doctest::Approx(2));

    // A chain of keys nearly as deep as the token array (320) is skipped too: no outputs.
    std::string chain = "{\"x\":[";
    for (int i = 0; i < 300; i++) chain += "\"a\":";
    chain += "\"a\"]}";
    EqConfig d;
    CHECK_FALSE(eq_parse(chain.c_str(), chain.size(), d));
}

TEST_CASE("eq_parse: a key with no value is a file it cannot read, at every depth") {
    // jsmn is not strict here and lets {"f":} through. What follows such a key was read as its
    // value: another field's token, or, after the file's last key, one jsmn never wrote.
    const char* bad[] = {
        R"({"outputs":[{"bands":[{"gain":,"f":1000}]}]})",            // "f" taken for the gain
        R"({"outputs":[{"id":,"bands":[{"f":1000}]}]})",              // "bands" taken for the id
        R"({"crossover":{"freq_hz":},"outputs":[{"bands":[{"f":1000}]}]})",
        R"({"outputs":[{"bands":[{"f":1000}]}],"mode":})",            // the file's last key
        R"({"outputs":[{"bands":[{"f":1000}],"label":}]})",           // a section's last key
        R"({"outputs":[{"bands":[{"f":}]}]})",                        // a band's last key
    };
    for (const char* b : bad) {
        INFO(b);
        EqConfig c;
        CHECK_FALSE(eq_parse(b, strlen(b), c));
        CHECK(c.sections == 0);
    }

    // The band's key as the 320th token: jsmn filled the whole array, and the value read for
    // it was the slot past its end.
    std::string full = R"({"outputs":[{"bands":[{"x":[1)";
    for (int k = 1; k < 310; k++) full += ",1";
    full += R"(],"f":}]}]})";
    EqConfig d;
    CHECK_FALSE(eq_parse(full.c_str(), full.size(), d));
    CHECK(d.sections == 0);
}

TEST_CASE("eq_parse: a number no float holds refuses the file, wherever it stands") {
    // jsmn is not strict here: inf and nan pass as numbers, and strtof makes 1e39 an inf. As a
    // band's gain each designed NaN coefficients, and the audio core silenced every block.
    const char* bad[] = {
        R"({"outputs":[{"bands":[{"type":"peak","f":1000,"gain":inf,"q":1,"on":true}]}]})",
        R"({"outputs":[{"bands":[{"type":"peak","f":1000,"gain":-inf,"q":1,"on":true}]}]})",
        R"({"outputs":[{"bands":[{"type":"peak","f":1000,"gain":nan,"q":1,"on":true}]}]})",
        R"({"outputs":[{"bands":[{"type":"peak","f":1000,"gain":1e39,"q":1,"on":true}]}]})",
        R"({"outputs":[{"bands":[{"type":"peak","f":nan,"gain":3,"q":1,"on":true}]}]})",
        R"({"outputs":[{"bands":[{"type":"peak","f":1000,"gain":3,"q":inf,"on":true}]}]})",
        R"({"outputs":[{"gain_db":inf,"bands":[{"f":1000}]}]})",
        R"({"crossover":{"freq_hz":nan},"outputs":[{"bands":[{"f":1000}]}]})",
        R"({"version":nan,"outputs":[{"bands":[{"f":1000}]}]})",
    };
    for (const char* b : bad) {
        INFO(b);
        EqConfig c;
        CHECK_FALSE(eq_parse(b, strlen(b), c));
        CHECK(c.sections == 0);
    }
}

TEST_CASE("eq_parse: a band past what the editors reach is held to it, and plays") {
    // The limits of an edit from the web (eq_web_clamp), which are the panel page's.
    const char* far = R"({"outputs":[{"bands":[
        {"type":"peak","f":5,"gain":600,"q":40,"on":true},
        {"type":"lowshelf","f":90000,"gain":-600,"q":4,"on":true},
        {"type":"notch","f":1000,"gain":0,"q":0.01,"on":true}]}]})";
    EqConfig c;
    REQUIRE(eq_parse(far, strlen(far), c));
    REQUIRE(c.sec[0].count == 3);
    const EqBand* b = c.sec[0].bands;
    CHECK(b[0].freq_hz == EQ_WEB_F_MIN);
    CHECK(b[0].gain_db == EQ_WEB_GAIN_MAX);
    CHECK(b[0].q == EQ_WEB_Q_MAX);
    CHECK(b[1].freq_hz == EQ_WEB_F_MAX);
    CHECK(b[1].gain_db == -EQ_WEB_GAIN_MAX);
    CHECK(b[1].q == SHELF_Q_MAX);
    CHECK(b[2].q == EQ_WEB_Q_MIN);

    // 600 dB as written left the volume no room at all; held to +12 dB, the EQ asks 12 dB of
    // headroom, and a tone at the band plays at its level.
    const char* loud = R"({"outputs":[{"bands":[{"type":"peak","f":1000,"gain":600,"q":1,"on":true}]}]})";
    EqConfig l;
    REQUIRE(eq_parse(loud, strlen(loud), l));
    DspChain chain;
    chain.reset();
    chain.set_bands(l.sec[0].bands, l.sec[0].count);
    chain.set_eq_enabled(true);
    chain.set_volume_percent(100);
    CHECK(chain.headroom_db() == doctest::Approx(EQ_WEB_GAIN_MAX).epsilon(0.01));
    float blk[DspChain::BLOCK * 2];
    float phase = 0.0f, peak = 0.0f;
    for (int n = 0; n < 60; n++) {
        for (size_t i = 0; i < DspChain::BLOCK; i++) {
            blk[2 * i] = blk[2 * i + 1] = 0.25f * std::sin(2.0f * (float)M_PI * phase);
            phase += 1000.0f / FS_HZ_DEFAULT;
            if (phase >= 1.0f) phase -= 1.0f;
        }
        chain.process(blk, DspChain::BLOCK);
        for (float v : blk) REQUIRE(std::isfinite(v));
        if (n >= 40) for (float v : blk) peak = std::max(peak, std::fabs(v));
    }
    CHECK(chain.faults() == 0);
    CHECK(peak == doctest::Approx(0.25f).epsilon(0.05));
}

TEST_CASE("eq_serialize: a buffer that is too small yields nothing, never a partial file") {
    EqConfig c;
    REQUIRE(eq_parse(SAMPLE, strlen(SAMPLE), c));
    char small[64];
    CHECK(eq_serialize(c, small, sizeof(small)) == 0);
}

TEST_CASE("the shipped data/eq.json parses and drives a flat response") {
    EqConfig c;
    REQUIRE(eq_load(MODESP_ROOT "/data/eq.json", c));
    CHECK(std::string(c.mode) == "2.0");
    REQUIRE(c.sections == 2);
    // Every band ships disabled, so the default sound is unprocessed.
    for (int f : {20, 100, 1000, 10000}) {
        CHECK(eq_magnitude_db(c.sec[0].bands, c.sec[0].count, (float)f)
              == doctest::Approx(0.0f));
    }
}

TEST_CASE("eq_parse: a file from a later schema is refused, not read with today's rules") {
    // The version field has been written since the first release and nothing ever read it.
    // If a later build adds a field — a second section that actually does
    // something, a crossover type — this parser would drop it, and the first slider move
    // would write the file back without it. Refusing gives a flat EQ and a log line; the
    // file itself stays whole.
    const std::string future =
        "{\"version\":99,\"mode\":\"2.0\",\"outputs\":[{\"id\":\"dac0\",\"bands\":["
        "{\"type\":\"peak\",\"f\":1000,\"gain\":6,\"q\":1,\"on\":true}]}]}";
    EqConfig c;
    CHECK_FALSE(eq_parse(future.c_str(), future.size(), c));
    CHECK(c.sections == 0);

    // The current number is fine, and so is a file that predates the field entirely: the
    // absence means "1", and version 1 parses correctly under these rules.
    const std::string current =
        "{\"version\":2,\"mode\":\"2.0\",\"outputs\":[{\"id\":\"dac0\",\"bands\":["
        "{\"type\":\"peak\",\"f\":1000,\"gain\":6,\"q\":1,\"on\":true}]}]}";
    EqConfig d;
    CHECK(eq_parse(current.c_str(), current.size(), d));
    CHECK(d.sections == 1);

    const std::string no_version =
        "{\"mode\":\"2.0\",\"outputs\":[{\"id\":\"dac0\",\"bands\":["
        "{\"type\":\"peak\",\"f\":1000,\"gain\":6,\"q\":1,\"on\":true}]}]}";
    EqConfig e;
    CHECK(eq_parse(no_version.c_str(), no_version.size(), e));
    CHECK(e.sections == 1);
}

TEST_CASE("eq_serialize stamps the schema number the parser checks against") {
    EqConfig c;
    c.sections = 1;
    snprintf(c.sec[0].id, sizeof(c.sec[0].id), "dac0");
    c.sec[0].count = 1;
    char buf[2048];
    const size_t n = eq_serialize(c, buf, sizeof(buf));
    REQUIRE(n > 0);
    char expect[32];
    snprintf(expect, sizeof(expect), "\"version\": %d", EQ_SCHEMA_VERSION);
    CHECK(std::string(buf, n).find(expect) != std::string::npos);
    // And what it writes, it reads back.
    EqConfig back;
    CHECK(eq_parse(buf, n, back));
}
