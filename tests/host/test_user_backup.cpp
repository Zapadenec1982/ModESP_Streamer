/**
 * @file test_user_backup.cpp
 * @brief The user's files as one JSON document (components/user_store/user_backup):
 *        what a restore body may carry, and the text that comes out of it.
 */

#include "doctest.h"
#include "user_backup.h"
#include "user_store.h"
#include "safe_file.h"
#include "modesp/services/config_service.h"

#include <cstring>
#include <string>

using namespace modesp;

namespace {

std::string text_of(const user_backup::Entry& e) {
    char out[512];
    size_t len = 0;
    REQUIRE(user_backup::unescape(e.text, e.len, out, sizeof(out), len));
    return std::string(out, len);
}

}  // namespace

TEST_CASE("a backup body: the known files, in the store's order, unknown names skipped") {
    const char* body = R"({"modesp_userdata":1,"made":"2026-10-11","files":{
        "stations.json":"{\"stations\":[{\"name\":\"Jazz \\\"FM\\\"\",\"url\":\"http://x/y\"}]}",
        "from_a_later_build.json":"{}",
        "last_track":"Albums/Бах/02.flac",
        "eq.json":"{\"version\":2}"}})";
    user_backup::Entry e[8];
    size_t n = 0;
    REQUIRE(user_backup::parse(body, strlen(body), e, 8, n));
    REQUIRE(n == 3);
    CHECK(std::string(e[0].name) == "eq.json");
    CHECK(std::string(e[1].name) == "stations.json");
    CHECK(std::string(e[2].name) == "last_track");
    CHECK(text_of(e[0]) == "{\"version\":2}");
    CHECK(text_of(e[1]) == "{\"stations\":[{\"name\":\"Jazz \\\"FM\\\"\",\"url\":\"http://x/y\"}]}");
    CHECK(text_of(e[2]) == "Albums/Бах/02.flac");
    CHECK(user_backup::is_json_object(text_of(e[1]).c_str(), text_of(e[1]).size()));
}

TEST_CASE("not a backup: refused before anything is written") {
    const char* bad[] = {
        R"([1,2])",
        R"({"modesp_userdata":1})",                                   // no files
        R"({"modesp_userdata":1,"files":{}})",                        // none of ours
        R"({"modesp_userdata":1,"files":{"eq.json":{"version":2}}})", // not carried as text
        R"({"modesp_userdata":2,"files":{"eq.json":"{}"}})",          // a later format
        R"({"modesp_userdata":1,"files":{"eq.json":"{}")",            // cut off
    };
    for (const char* b : bad) {
        INFO(b);
        user_backup::Entry e[8];
        size_t n = 0;
        CHECK_FALSE(user_backup::parse(b, strlen(b), e, 8, n));
    }
}

TEST_CASE("unescape: every JSON escape, surrogate pairs, and what may not pass") {
    char out[64];
    size_t len = 0;
    const char* s = R"(a\"b\\c\/d\n\tЖ🎵)";
    REQUIRE(user_backup::unescape(s, strlen(s), out, sizeof(out), len));
    CHECK(std::string(out, len) == "a\"b\\c/d\n\t\xD0\x96\xF0\x9F\x8E\xB5");

    const char* refused[] = {R"(\x)", R"(\u12)", R"(\ud83c)", R"(\udfb5x)", R"(\u0000)", "a\tb"};
    for (const char* r : refused) {
        INFO(r);
        CHECK_FALSE(user_backup::unescape(r, strlen(r), out, sizeof(out), len));
    }
    CHECK_FALSE(user_backup::unescape("abcdef", 6, out, 6, len));   // no room for the NUL
}

TEST_CASE("which files are JSON, and what counts as one") {
    CHECK(user_backup::is_json_file("eq.json"));
    CHECK(user_backup::is_json_file("devices.json"));
    CHECK_FALSE(user_backup::is_json_file("last_track"));
    CHECK(user_backup::is_json_object("  {\"a\":[1,2]}\n", 15));
    CHECK_FALSE(user_backup::is_json_object("[1]", 3));
    CHECK_FALSE(user_backup::is_json_object("{\"a\":", 5));
    CHECK_FALSE(user_backup::is_json_object("", 0));
}

TEST_CASE("max_bytes: what each file's reader takes whole, and safe_file can put in place") {
    // Copies of the readers' limits (user_backup.cpp names each source). A restore used to
    // stage any size: a 30 KB stations.json went in, was read cut short, and the radio wrote
    // its built-in list over it; past 64 KB a file was never installed.
    CHECK(user_backup::max_bytes("stations.json") == 24 * 1024 - 1);   // STATIONS_JSON_MAX - 1
    CHECK(user_backup::max_bytes("eq.json") == 4096 - 1);              // eq_config MAX_FILE - 1
    CHECK(user_backup::max_bytes("eq_custom.json") == 4096 - 1);
    CHECK(user_backup::max_bytes("devices.json") == ConfigService::MAX_JSON_SIZE - 1);
    CHECK(user_backup::max_bytes("last_track") == 1024);
    CHECK(user_backup::max_bytes("from_a_later_build.json") == 0);
    CHECK(user_backup::max_bytes(nullptr) == 0);
    for (const char* const* name = UserStore::FILES; *name; ++name) {
        INFO(*name);
        CHECK(user_backup::max_bytes(*name) > 0);                      // a new file needs a limit
        CHECK(user_backup::max_bytes(*name) <= safe_file::MAX_COPY_BYTES);
    }
}
