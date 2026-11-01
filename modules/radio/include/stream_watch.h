/**
 * @file stream_watch.h
 * @brief When a live stream's server cannot keep up, and which of its addresses to try next.
 *
 * A station's host often has several addresses, and they are not equally near:
 * stream.nightride.fm hands out three, one of them ~200 ms away and two ~65 ms.
 * Through a TCP window of W bytes a connection carries at most W per round trip, so the far
 * one starved a 320 kbit/s stream at 28.7 KB/s with the network otherwise idle. The radio
 * then leaves that server: it reconnects, and asks for an address other than the one that
 * starved. Which address the resolver hands out first is the DNS server's choice and is
 * cached for the record's TTL, so reconnecting by name alone would mostly land on it again.
 *
 * Pure, so the decisions are tested on the host (tests/host/test_stream_watch.cpp).
 */

#pragma once

#include <cstdint>

namespace radio_core {

/// Fed once per stream report (~5 s): says when to leave the server.
class StarveWatch {
public:
    /// Reports in a row that make a server "cannot keep up": ten seconds of it.
    static constexpr int WINDOWS = 2;
    /// Not again sooner than this after leaving one: a station that starves everywhere is not
    /// helped by being torn down every ten seconds. Just over the 30 s TTL such hosts give
    /// their record, because stream.nightride.fm answers ONE address per query, from Google's
    /// and Cloudflare's resolvers alike: until the record expires, asking again gives the same
    /// server, and only the next attempt can land elsewhere.
    static constexpr uint32_t GAP_S = 35;
    /// Below this fill the buffer is running dry; above it a slow report is only a dip.
    static constexpr unsigned DRY_PCT = 10;

    /// One report: what the network brought (bytes/s), what the stream needs (bytes/s, 0 when
    /// not known), how full the stream buffer is (percent), and the time in seconds. True when
    /// it is time to try another server.
    bool update(uint32_t rate, uint32_t need, unsigned buffer_pct, uint32_t now_s) {
        const bool starving = need > 0 && (uint64_t)rate * 100 < (uint64_t)need * 95 &&
                              buffer_pct < DRY_PCT;
        bad_ = starving ? bad_ + 1 : 0;
        if (bad_ < WINDOWS) return false;
        if (left_ && now_s - left_at_ < GAP_S) return false;
        bad_ = 0;
        left_ = true;
        left_at_ = now_s;
        return true;
    }

    /// A different station, or the stream stopped: start counting afresh.
    void reset() { *this = StarveWatch{}; }

private:
    int      bad_     = 0;
    bool     left_    = false;
    uint32_t left_at_ = 0;
};

/// The first of `n` addresses that is not `avoid` (0 is no address), or −1 when there is none.
inline int pick_address(const uint32_t* list, int n, uint32_t avoid) {
    for (int i = 0; i < n; i++) {
        if (list[i] && list[i] != avoid) return i;
    }
    return -1;
}

}  // namespace radio_core
