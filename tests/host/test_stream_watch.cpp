/**
 * @file test_stream_watch.cpp
 * @brief When the radio leaves a server that cannot keep up, and where it goes
 *        (modules/radio/include/stream_watch.h).
 *
 * The numbers are the bench's: nightride at 320 kbit/s needs 40 000 B/s and got
 * 28 742-29 604 B/s from its far server, the buffer at 0 %; from a near one 40 000 B/s with the
 * buffer at 96-99 %.
 */

#include "doctest.h"
#include "../../modules/radio/include/stream_watch.h"

using radio_core::StarveWatch;
using radio_core::pick_address;

TEST_CASE("a server that cannot keep up is left after two reports in a row") {
    StarveWatch w;
    CHECK_FALSE(w.update(28742, 40000, 0, 100));
    CHECK(w.update(29604, 40000, 0, 105));
}

TEST_CASE("a dip with the buffer still full, an unknown need, or a recovery is not starving") {
    StarveWatch w;
    // slow, but the buffer is full: the next reports will tell
    CHECK_FALSE(w.update(30000, 40000, 96, 0));
    CHECK_FALSE(w.update(30000, 40000, 50, 5));
    // FLAC and other streams that do not say their rate
    CHECK_FALSE(w.update(0, 0, 0, 10));
    CHECK_FALSE(w.update(0, 0, 0, 15));
    // one bad report, then a good one: the count starts over
    CHECK_FALSE(w.update(20000, 40000, 0, 20));
    CHECK_FALSE(w.update(40000, 40000, 5, 25));
    CHECK_FALSE(w.update(20000, 40000, 0, 30));
    CHECK(w.update(20000, 40000, 0, 35));
}

TEST_CASE("within 5 % of what the stream needs is keeping up") {
    StarveWatch w;
    CHECK_FALSE(w.update(38000, 40000, 0, 0));   // exactly 95 %
    CHECK_FALSE(w.update(38000, 40000, 0, 5));
    CHECK_FALSE(w.update(37999, 40000, 0, 10));
    CHECK(w.update(37999, 40000, 0, 15));
}

TEST_CASE("not torn down again before the gap is over, but again after it") {
    StarveWatch w;
    w.update(10000, 40000, 0, 0);
    REQUIRE(w.update(10000, 40000, 0, 5));
    const uint32_t again = 5 + StarveWatch::GAP_S;
    for (uint32_t t = 10; t < again; t += 5) CHECK_FALSE(w.update(10000, 40000, 0, t));
    CHECK(w.update(10000, 40000, 0, again));
}

TEST_CASE("the gap outlasts a 30 s DNS record, so the next try can land on another server") {
    CHECK(StarveWatch::GAP_S > 30);
}

TEST_CASE("a new station starts counting afresh") {
    StarveWatch w;
    w.update(10000, 40000, 0, 0);
    REQUIRE(w.update(10000, 40000, 0, 5));
    w.reset();
    CHECK_FALSE(w.update(10000, 40000, 0, 10));
    CHECK(w.update(10000, 40000, 0, 15));        // no minute to wait out on another station
}

TEST_CASE("another address than the one that starved, if the host has one") {
    const uint32_t far = 0xDABB5133, near1 = 0xB7165933, near2 = 0xC1452040;
    const uint32_t three[] = {far, near1, near2};
    CHECK(pick_address(three, 3, far) == 1);
    CHECK(pick_address(three, 3, near1) == 0);
    CHECK(pick_address(three, 3, 0) == 0);        // nothing to avoid: the first
    const uint32_t one[] = {far};
    CHECK(pick_address(one, 1, far) == -1);       // a single address: nowhere else to go
    const uint32_t gaps[] = {0, far, 0, near2};
    CHECK(pick_address(gaps, 4, far) == 3);
    CHECK(pick_address(gaps, 0, far) == -1);
}
