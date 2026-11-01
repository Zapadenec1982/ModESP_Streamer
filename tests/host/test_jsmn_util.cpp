/**
 * @file test_jsmn_util.cpp
 * @brief components/jsmn_util: the helpers the parsers share — a key compared, a value
 *        stepped over whole, a string's escapes undone, the bounded writer.
 *
 * The parsers built on these have tests of their own (stations, the catalog, eq.json, /api/eq,
 * /api/bt, the user backup); these hold the helpers to the rules those parsers rely on.
 */

#include "doctest.h"

#define JSMN_STATIC
#include "jsmn.h"
#include "jsmn_util.h"

#include <cstring>
#include <string>
#include <vector>

namespace {

struct Parsed {
    std::string json;
    std::vector<jsmntok_t> tok;
    int n = 0;
};

Parsed parse(const std::string& json, size_t max_tokens = 64) {
    Parsed p{json, std::vector<jsmntok_t>(max_tokens), 0};
    jsmn_parser jp;
    jsmn_init(&jp);
    p.n = jsmn_parse(&jp, p.json.c_str(), p.json.size(), p.tok.data(), (unsigned)max_tokens);
    return p;
}

bool unescape(const std::string& s, jsmn_util::Controls controls, std::string& out, size_t cap = 64) {
    std::vector<char> buf(cap + 1, 'x');
    size_t len = 0;
    if (!jsmn_util::unescape(s.data(), s.size(), buf.data(), cap, len, controls)) return false;
    out.assign(buf.data(), len);
    return buf[len] == '\0';
}

}  // namespace

TEST_CASE("jsmn_util: key_is takes the whole string token, not a prefix and not a primitive") {
    const Parsed p = parse(R"({"name":"name","n":1,"true":true})");
    REQUIRE(p.n == 7);
    CHECK(jsmn_util::key_is(p.json.c_str(), p.tok[1], "name"));
    CHECK(jsmn_util::key_is(p.json.c_str(), p.tok[2], "name"));    // a value, compared as one
    CHECK_FALSE(jsmn_util::key_is(p.json.c_str(), p.tok[1], "nam"));
    CHECK_FALSE(jsmn_util::key_is(p.json.c_str(), p.tok[3], "name"));
    CHECK(jsmn_util::key_is(p.json.c_str(), p.tok[5], "true"));
    CHECK_FALSE(jsmn_util::key_is(p.json.c_str(), p.tok[6], "true"));   // the primitive true
}

TEST_CASE("jsmn_util: span counts a value with everything inside it, and stops at the count") {
    const Parsed p = parse(R"({"a":[1,{"b":[2,3]},4],"c":5})");
    REQUIRE(p.n == 12);
    CHECK(jsmn_util::span(p.tok.data(), p.n, 0) == 12);             // the whole document
    CHECK(jsmn_util::span(p.tok.data(), p.n, 1) == 9);              // "a" and its value
    CHECK(jsmn_util::span(p.tok.data(), p.n, 2) == 8);              // [1,{"b":[2,3]},4]
    CHECK(jsmn_util::span(p.tok.data(), p.n, 4) == 5);              // {"b":[2,3]}
    CHECK(jsmn_util::span(p.tok.data(), p.n, 3) == 1);              // 1
    CHECK(jsmn_util::span(p.tok.data(), p.n, 11) == 1);             // 5
    CHECK(jsmn_util::span(p.tok.data(), 6, 2) == 4);                // cut at a count of 6
    CHECK(jsmn_util::span(p.tok.data(), p.n, p.n) == 1);            // past the end: one, never more
}

TEST_CASE("jsmn_util: span walks a value nested thousands deep") {
    const int depth = 5000;
    const Parsed p = parse(std::string(depth, '[') + std::string(depth, ']'), depth);
    REQUIRE(p.n == depth);
    CHECK(jsmn_util::span(p.tok.data(), p.n, 0) == depth);
    CHECK(jsmn_util::span(p.tok.data(), p.n, depth / 2) == depth / 2);
}

TEST_CASE("jsmn_util: unescape undoes every JSON escape and keeps raw UTF-8") {
    std::string out;
    REQUIRE(unescape(R"(a\"b\\c\/d é€ 🎵 Ж)", jsmn_util::Controls::NONE, out));
    CHECK(out == "a\"b\\c/d \xc3\xa9\xe2\x82\xac \xf0\x9f\x8e\xb5 \xd0\x96");
}

TEST_CASE("jsmn_util: unescape lets control characters through as the caller says") {
    std::string out;
    REQUIRE(unescape(R"(a\nb\tc\r\b\f\u0001\u001f)", jsmn_util::Controls::ESCAPED, out));
    CHECK(out == "a\nb\tc\r\b\f\x01\x1f");
    for (const char* s : {R"(a\nb)", R"(\t)", R"(\u0007)", R"(\u001F)"}) {
        INFO(s);
        CHECK_FALSE(unescape(s, jsmn_util::Controls::NONE, out));
    }
    for (jsmn_util::Controls c : {jsmn_util::Controls::ESCAPED, jsmn_util::Controls::NONE}) {
        CHECK_FALSE(unescape(R"(a\u0000b)", c, out));               // a NUL would cut the text short
        CHECK_FALSE(unescape("a\tb", c, out));                      // raw: JSON allows none
        CHECK_FALSE(unescape(std::string("a\0b", 3), c, out));
    }
}

TEST_CASE("jsmn_util: unescape refuses what is not a JSON string") {
    std::string out;
    for (const char* s : {R"(\x41)", R"(\u12)", R"(\u12G4)", R"(\ud83c)", R"(\ud83cx)", R"(\ud83cA)",
                          R"(\udfb5)", R"(abc\)", R"(\)", R"(\U00e9)"}) {
        INFO(s);
        CHECK_FALSE(unescape(s, jsmn_util::Controls::ESCAPED, out));
    }
}

TEST_CASE("jsmn_util: unescape needs room for the text and its NUL") {
    std::string out;
    CHECK(unescape("abcde", jsmn_util::Controls::NONE, out, 6));
    CHECK_FALSE(unescape("abcdef", jsmn_util::Controls::NONE, out, 6));
    CHECK(unescape(R"(€)", jsmn_util::Controls::NONE, out, 4));      // three bytes and the NUL
    CHECK_FALSE(unescape(R"(€)", jsmn_util::Controls::NONE, out, 3));
}

TEST_CASE("jsmn_util: put_utf8 writes a whole code point or nothing") {
    char buf[8] = "zzzzzzz";
    size_t o = 0;
    REQUIRE(jsmn_util::put_utf8(0x1F3B5, buf, o, sizeof(buf)));
    CHECK(o == 4);
    CHECK(std::string(buf, o) == "\xf0\x9f\x8e\xb5");
    CHECK_FALSE(jsmn_util::put_utf8(0x1F3B5, buf, o, sizeof(buf)));      // 4 + 4 leaves no NUL
    CHECK(o == 4);
    CHECK(buf[4] == 'z');
}

TEST_CASE("jsmn_util: hex digits") {
    CHECK(jsmn_util::hex4("00e9") == 0xe9);
    CHECK(jsmn_util::hex4("FFFF") == 0xffff);
    CHECK(jsmn_util::hex4("12g4") == -1);
    CHECK(jsmn_util::hex_digit('a') == 10);
    CHECK(jsmn_util::hex_digit(':') == -1);
}

TEST_CASE("jsmn_util: Appender writes piece after piece, and a piece that does not fit spoils the whole") {
    char buf[18];                                                   // the 17 characters and the NUL
    jsmn_util::Appender add(buf, sizeof(buf));
    add("{\"n\":%d", 42);
    add(",\"s\":\"%s\"", "ok");
    add("%s", "}");
    CHECK(add.size() == 17);
    CHECK(std::string(buf) == "{\"n\":42,\"s\":\"ok\"}");

    jsmn_util::Appender tight(buf, sizeof(buf) - 1);                // a byte short
    tight("{\"n\":%d", 42);
    tight(",\"s\":\"%s\"", "ok");
    CHECK(tight.size() == 16);
    tight("%s", "}");
    CHECK(tight.size() == 0);
    tight("%s", "");                                                // nothing after it counts
    CHECK(tight.size() == 0);

    jsmn_util::Appender none(buf, 0);
    none("%s", "");
    CHECK(none.size() == 0);
}
