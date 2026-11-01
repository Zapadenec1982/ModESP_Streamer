/**
 * @file test_safe_file.cpp
 * @brief The replace sequence, against the three ways a runtime write has cost a file.
 *
 * These tests write real files, because that is the whole subject: what is on disk after a
 * write that did not finish, after a write whose content is wrong, and after a write that
 * was interrupted between the candidate and the swap. MODESP_TEST_TMP is a directory under
 * the host build tree (tests/host/CMakeLists.txt).
 */

#include "doctest.h"

#include "safe_file.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

const std::string kTmp = MODESP_TEST_TMP;

std::string path_of(const char* name) { return kTmp + "/" + name; }

void put(const std::string& path, const std::string& text) {
    FILE* f = fopen(path.c_str(), "wb");
    REQUIRE(f != nullptr);
    if (!text.empty()) fwrite(text.data(), 1, text.size(), f);
    fclose(f);
}

/// Contents of `path`, or "\x01absent" for a file that is not there — a sentinel no test
/// writes, so "missing" and "empty" never read the same.
std::string get(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return "\x01" "absent";   // split: "\x01ab..." would read as one hex escape
    char buf[4096];
    const size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    return std::string(buf, n);
}

bool there(const std::string& path) { return get(path) != "\x01" "absent"; }

/// A validator that refuses everything, to stand for a parser rejecting the candidate.
bool reject_all(const char*, void*) { return false; }

/// A validator that accepts only a file whose first byte is '{' — enough to tell "ran over
/// the candidate" from "ran over the live file".
bool must_look_like_json(const char* path, void*) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    const int c = fgetc(f);
    fclose(f);
    return c == '{';
}

struct Scratch {
    std::string live;
    explicit Scratch(const char* name) : live(path_of(name)) {
        remove(live.c_str());
        remove((live + safe_file::SUFFIX).c_str());
    }
    ~Scratch() {
        remove(live.c_str());
        remove((live + safe_file::SUFFIX).c_str());
    }
    std::string candidate() const { return live + safe_file::SUFFIX; }
};

} // namespace

TEST_CASE("a save creates the file with exactly the bytes handed over") {
    Scratch s("sf_create");
    const std::string text = "{\"version\":2}";
    REQUIRE(safe_file::save(s.live.c_str(), text.data(), text.size()));
    CHECK(get(s.live) == text);
    // No leftovers: a directory listing after a good save looks like nothing happened.
    CHECK_FALSE(there(s.candidate()));
}

TEST_CASE("a save replaces an existing file and never removes it first") {
    Scratch s("sf_replace");
    put(s.live, "old contents");
    const std::string text = "new contents, longer than the old ones";
    REQUIRE(safe_file::save(s.live.c_str(), text.data(), text.size()));
    CHECK(get(s.live) == text);
    CHECK_FALSE(there(s.candidate()));
}

TEST_CASE("a rejected candidate leaves the live file exactly as it was") {
    Scratch s("sf_reject");
    put(s.live, "the configuration someone is using");
    const std::string text = "something the parser will not take";

    CHECK_FALSE(safe_file::save(s.live.c_str(), text.data(), text.size(), reject_all));

    // This is the failure that cost a flashed panel: a file the boot parser would refuse
    // reached flash because nothing ran the parser before the swap.
    CHECK(get(s.live) == "the configuration someone is using");
    CHECK_FALSE(there(s.candidate()));
}

TEST_CASE("the validator sees the candidate, not the live file") {
    Scratch s("sf_validator_sees");
    put(s.live, "not json at all");
    const std::string good = "{\"ok\":true}";
    CHECK(safe_file::save(s.live.c_str(), good.data(), good.size(), must_look_like_json));
    CHECK(get(s.live) == good);

    const std::string bad = "plain text";
    CHECK_FALSE(safe_file::save(s.live.c_str(), bad.data(), bad.size(), must_look_like_json));
    CHECK(get(s.live) == good);
}

TEST_CASE("a leftover candidate from an interrupted save is never read and is overwritten") {
    Scratch s("sf_leftover");
    put(s.live, "good");
    // What a power cut during fwrite leaves behind: a partial candidate, live file intact.
    put(s.candidate(), "half-writ");

    // A reader only ever opens the live name, so the leftover is invisible to it...
    CHECK(get(s.live) == "good");

    // ...and the next save reuses that name rather than tripping over it.
    const std::string text = "good again";
    REQUIRE(safe_file::save(s.live.c_str(), text.data(), text.size()));
    CHECK(get(s.live) == text);
    CHECK_FALSE(there(s.candidate()));
}

TEST_CASE("sweep removes a leftover candidate and does not touch the live file") {
    Scratch s("sf_sweep");
    put(s.live, "live");
    put(s.candidate(), "junk");
    safe_file::sweep(s.live.c_str());
    CHECK_FALSE(there(s.candidate()));
    CHECK(get(s.live) == "live");
    safe_file::sweep(s.live.c_str());   // idempotent: nothing to remove is not an error
    CHECK(get(s.live) == "live");
}

TEST_CASE("an empty file is a legitimate result, not a failure") {
    Scratch s("sf_empty");
    put(s.live, "something");
    REQUIRE(safe_file::save(s.live.c_str(), "", 0));
    CHECK(get(s.live) == "");
    CHECK(there(s.live));
}

TEST_CASE("a path too long for the candidate name fails before anything is touched") {
    // 192 bytes is the buffer safe_file uses for "<path>.new"; a path that cannot have the
    // suffix appended must not half-do the job.
    std::string very_long = kTmp + "/";
    very_long.append(300, 'x');
    CHECK_FALSE(safe_file::save(very_long.c_str(), "x", 1));
    CHECK_FALSE(there(very_long));
}

TEST_CASE("copy moves the bytes and leaves the destination alone when the source is gone") {
    Scratch src("sf_copy_src");
    Scratch dst("sf_copy_dst");
    put(src.live, "{\"stations\":[]}");
    put(dst.live, "older destination");

    REQUIRE(safe_file::copy(src.live.c_str(), dst.live.c_str()));
    CHECK(get(dst.live) == "{\"stations\":[]}");
    CHECK_FALSE(there(dst.candidate()));

    remove(src.live.c_str());
    CHECK_FALSE(safe_file::copy(src.live.c_str(), dst.live.c_str()));
    CHECK(get(dst.live) == "{\"stations\":[]}");   // untouched by the failed copy
    CHECK_FALSE(there(dst.candidate()));
}

TEST_CASE("copy carries a file larger than its own buffer without losing bytes") {
    Scratch src("sf_copy_big_src");
    Scratch dst("sf_copy_big_dst");
    std::string big;
    // Not round: a bug that copies whole 512-byte chunks only would still pass on 2048.
    for (int i = 0; i < 1700; i++) big.push_back((char)('a' + (i % 26)));
    put(src.live, big);
    REQUIRE(safe_file::copy(src.live.c_str(), dst.live.c_str()));
    CHECK(get(dst.live).size() == big.size());
    CHECK(get(dst.live) == big);
}

TEST_CASE("a copy the validator refuses leaves the destination as it was") {
    Scratch src("sf_copy_reject_src");
    Scratch dst("sf_copy_reject_dst");
    put(src.live, "not json");
    put(dst.live, "{\"keep\":\"me\"}");
    CHECK_FALSE(safe_file::copy(src.live.c_str(), dst.live.c_str(), must_look_like_json));
    CHECK(get(dst.live) == "{\"keep\":\"me\"}");
    CHECK_FALSE(there(dst.candidate()));
}

TEST_CASE("exists answers about the live name only") {
    Scratch s("sf_exists");
    CHECK_FALSE(safe_file::exists(s.live.c_str()));
    put(s.candidate(), "candidate only");
    CHECK_FALSE(safe_file::exists(s.live.c_str()));
    put(s.live, "live");
    CHECK(safe_file::exists(s.live.c_str()));
}
