/**
 * @file test_pcm_ring.cpp
 * @brief The SPSC ring between decoder and audio task.
 *
 * The cases that matter are the ones that bite in the field: wrap-around, a partial write
 * when the ring is nearly full, a partial read when it is nearly empty, and the distinction
 * between "finished" and "starved" — which look identical from an empty ring and are the
 * difference between advancing to the next track and clicking.
 */

#include "doctest.h"
#include "pcm_ring.h"
#include <chrono>
#include <thread>
#include <vector>

namespace audio_core {
struct PcmRingTestAccess {
    static void copy_hook(PcmRing& ring, void (*hook)(void*), void* ctx) {
        ring.copy_hook_ = hook;
        ring.copy_hook_ctx_ = ctx;
    }
};
}

using namespace audio_core;

namespace {

/// Frame i carries (i, -i) so a misplaced or duplicated frame cannot hide.
void fill_seq(float* st, size_t frames, int start) {
    for (size_t i = 0; i < frames; i++) {
        st[2 * i]     = (float)(start + (int)i);
        st[2 * i + 1] = -(float)(start + (int)i);
    }
}

bool check_seq(const float* st, size_t frames, int start) {
    for (size_t i = 0; i < frames; i++) {
        if (st[2 * i] != (float)(start + (int)i)) return false;
        if (st[2 * i + 1] != -(float)(start + (int)i)) return false;
    }
    return true;
}

/// Wait for `pred` without starving whoever has to make it true: a handful of yields, then
/// short sleeps. A pure yield spin waits a whole scheduler quantum per handoff on a loaded
/// machine, which is what turned the flush test's 50 000 rounds into forty minutes; the
/// first yields keep the wait tight enough to still catch a store published out of order.
template <class Pred> void wait_until(Pred pred) {
    for (int spins = 0; !pred(); ++spins) {
        if (spins < 64) std::this_thread::yield();
        else std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
}

} // namespace

namespace {
struct PausedCopy {
    std::atomic<bool> entered{false}, resume{false};
    static void hook(void* ctx) {
        auto& p = *static_cast<PausedCopy*>(ctx);
        p.entered.store(true, std::memory_order_release);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!p.resume.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    bool wait() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!entered.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        return entered.load(std::memory_order_acquire);
    }
};
}

TEST_CASE("ring: handoff during a real read waits for quiescence without blocking the caller") {
    PcmRing r;
    REQUIRE(r.init(1024));
    r.claim(PcmRing::OWNER_FILE);
    float in[400]{}, out[400]{};
    REQUIRE(r.write(PcmRing::OWNER_FILE, in, 200) == 200);
    REQUIRE(r.read(out, 100) == 100);
    PausedCopy copy;
    PcmRingTestAccess::copy_hook(r, &PausedCopy::hook, &copy);
    size_t read = 999;
    std::thread consumer([&] { read = r.read(out, 100); });
    const bool entered = copy.wait();
    CHECK(entered);
    const uint32_t old = r.generation();
    r.claim(PcmRing::OWNER_RADIO);
    CHECK(r.generation() != old);
    CHECK(r.reset_pending());
    const auto accepted = r.generation();
    CHECK(r.reset(accepted));              // consumer sees pending flush while old copy exits
    CHECK(r.generation() == accepted);     // coalesce it; new producer keeps its session token
    CHECK(r.available() == 0);
    CHECK(r.write(PcmRing::OWNER_RADIO, in, 20) == 0);
    r.set_eos(true);                       // a new producer may finish before old read exits
    copy.resume.store(true, std::memory_order_release);
    consumer.join();
    PcmRingTestAccess::copy_hook(r, nullptr, nullptr);
    CHECK(read == 0);                      // old-session samples must not reach the DSP
    CHECK_FALSE(r.reset_pending());
    CHECK(r.eos());                        // deferred reset must not erase this new-session EOS
    CHECK(r.available() == 0);             // the old tail cannot resurrect 825 frames
    CHECK(r.write(PcmRing::OWNER_RADIO, in, 20) == 20);
    CHECK(r.read(out, 20) == 20);
    r.deinit();
}

TEST_CASE("ring: reset and multiple handoffs cannot race an active producer copy") {
    PcmRing r;
    REQUIRE(r.init(32));
    r.claim(PcmRing::OWNER_FILE);
    const uint32_t old = r.generation();
    float in[40], out[40]{};
    fill_seq(in, 20, 42);
    PausedCopy copy;
    PcmRingTestAccess::copy_hook(r, &PausedCopy::hook, &copy);
    size_t written = 999;
    std::thread producer([&] { written = r.write(PcmRing::OWNER_FILE, old, in, 20); });
    CHECK(copy.wait());
    r.request_flush();
    r.reset();
    r.claim(PcmRing::OWNER_RADIO);
    r.claim(PcmRing::OWNER_FILE);           // same owner again, but another session
    CHECK(r.flush_pending());
    CHECK(r.read(out, 20) == 0);           // consumer does not wait on that paused producer
    copy.resume.store(true, std::memory_order_release);
    producer.join();
    PcmRingTestAccess::copy_hook(r, nullptr, nullptr);
    CHECK(written == 0);
    CHECK_FALSE(r.flush_pending());
    CHECK(r.available() == 0);
    CHECK(r.write(PcmRing::OWNER_FILE, old, in, 20) == 0);
    CHECK(r.write(PcmRing::OWNER_FILE, r.generation(), in, 20) == 20);
    REQUIRE(r.read(out, 20) == 20);
    CHECK(check_seq(out, 20, 42));
    r.deinit();
}

TEST_CASE("ring: capacity, emptiness and fullness are consistent") {
    PcmRing r;
    REQUIRE(r.init(16));
    CHECK(r.available() == 0);
    CHECK(r.space() == 16);
    CHECK(r.fill_pct() == 0);

    float in[2 * 16];
    fill_seq(in, 16, 0);
    CHECK(r.write(in, 16) == 16);
    CHECK(r.available() == 16);
    CHECK(r.space() == 0);
    CHECK(r.fill_pct() == 100);

    // A full ring takes nothing more, and refuses rather than overwriting.
    CHECK(r.write(in, 4) == 0);
    CHECK(r.available() == 16);
    r.deinit();
}

TEST_CASE("ring: a short write reports what it took, not what it was asked") {
    PcmRing r;
    REQUIRE(r.init(10));
    float in[2 * 32];
    fill_seq(in, 32, 100);
    CHECK(r.write(in, 32) == 10);          // only what fits

    float out[2 * 32];
    CHECK(r.read(out, 32) == 10);          // only what is there
    CHECK(check_seq(out, 10, 100));
    CHECK(r.available() == 0);
    r.deinit();
}

TEST_CASE("ring: data survives wrap-around unshuffled") {
    PcmRing r;
    REQUIRE(r.init(8));
    float in[2 * 8], out[2 * 8];

    // Walk the indices right around the buffer several times in uneven steps, so every
    // wrap lands at a different offset.
    int seq = 0;
    for (int round = 0; round < 20; round++) {
        const size_t n = (size_t)(1 + (round % 5));
        fill_seq(in, n, seq);
        REQUIRE(r.write(in, n) == n);
        REQUIRE(r.read(out, n) == n);
        CHECK(check_seq(out, n, seq));
        seq += (int)n;
    }
    r.deinit();
}

TEST_CASE("ring: a write that straddles the end arrives in one piece") {
    PcmRing r;
    REQUIRE(r.init(8));
    float in[2 * 8], out[2 * 8];

    fill_seq(in, 6, 0);
    REQUIRE(r.write(in, 6) == 6);
    REQUIRE(r.read(out, 5) == 5);          // tail now at 5, head at 6

    fill_seq(in, 6, 50);                   // this one wraps
    REQUIRE(r.write(in, 6) == 6);
    REQUIRE(r.read(out, 1) == 1);          // the leftover frame 5
    REQUIRE(r.read(out, 6) == 6);
    CHECK(check_seq(out, 6, 50));
    r.deinit();
}

TEST_CASE("ring: eos separates 'track finished' from 'decoder is behind'") {
    PcmRing r;
    REQUIRE(r.init(8));
    CHECK_FALSE(r.eos());
    CHECK(r.available() == 0);             // empty and not finished = starved

    r.set_eos(true);
    CHECK(r.eos());
    CHECK(r.available() == 0);             // empty and finished = play the next one

    r.reset();
    CHECK_FALSE(r.eos());                  // reset clears it for the next track
    r.deinit();
}

TEST_CASE("ring: reset drops everything") {
    PcmRing r;
    REQUIRE(r.init(16));
    float in[2 * 16];
    fill_seq(in, 12, 7);
    REQUIRE(r.write(in, 12) == 12);
    r.reset();
    CHECK(r.available() == 0);
    CHECK(r.space() == 16);
    r.deinit();
}

TEST_CASE("ring: a producer and a consumer on two threads lose nothing and invent nothing") {
    // The whole point of the structure. Without a lock, a mistake here shows up as a click
    // once an hour, which is exactly the kind of bug a test has to catch instead.
    PcmRing r;
    REQUIRE(r.init(64));
    constexpr int TOTAL = 20000;

    std::thread producer([&] {
        float buf[2 * 13];
        int seq = 0;
        while (seq < TOTAL) {
            // Clamp the last chunk: uneven sizes are the point, overshooting TOTAL is not.
            size_t want = (size_t)(1 + (seq % 13));
            if (seq + (int)want > TOTAL) want = (size_t)(TOTAL - seq);
            fill_seq(buf, want, seq);
            size_t done = 0;
            while (done < want) done += r.write(buf + 2 * done, want - done);
            seq += (int)want;
        }
        r.set_eos(true);
    });

    std::vector<float> got;
    got.reserve(2 * TOTAL);
    float buf[2 * 17];
    int have = 0;
    while (have < TOTAL) {
        size_t want = 17;
        if (have + (int)want > TOTAL) want = (size_t)(TOTAL - have);
        const size_t n = r.read(buf, want);
        for (size_t i = 0; i < 2 * n; i++) got.push_back(buf[i]);
        have += (int)n;
    }
    producer.join();

    REQUIRE((int)got.size() == 2 * TOTAL);
    CHECK(check_seq(got.data(), TOTAL, 0));
    r.deinit();
}

TEST_CASE("ring: init(0) and init(1) are refused rather than producing a useless ring") {
    PcmRing r;
    CHECK_FALSE(r.init(0));
    CHECK_FALSE(r.init(1));
    CHECK(r.available() == 0);
    CHECK(r.space() == 0);
    float f[2] = {0, 0};
    CHECK(r.write(f, 1) == 0);
    CHECK(r.read(f, 1) == 0);
}

TEST_CASE("ring: flush acknowledgement publishes reset before the producer resumes") {
    // reset() used to clear flush_ BEFORE resetting the indices, so a producer that resumed the
    // instant the flag dropped could write frames the reset then erased. The producer here does
    // exactly that — its wait on the flag is the handoff under test and it writes straight
    // after — and every frame it writes must come out the other side.
    //
    // Bounded: 2000 rounds or two seconds, whichever comes first (the 50 000 rounds this ran
    // took forty minutes on a loaded machine), and the waits back off to short sleeps rather
    // than spinning, so the two threads cannot starve each other.
    PcmRing r;
    REQUIRE(r.init(16));
    constexpr unsigned ROUNDS = 2000;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    std::atomic<unsigned> published{0}, consumed{0}, failures{0};
    std::atomic<bool> stop{false};
    std::thread consumer([&] {
        for (unsigned i = 1; i <= ROUNDS; ++i) {
            wait_until([&] { return r.flush_pending() || stop.load(std::memory_order_acquire); });
            if (stop.load(std::memory_order_acquire)) break;
            r.reset();
            wait_until([&] { return published.load(std::memory_order_acquire) == i; });
            float out[6]{};
            if (r.read(out, 3) != 3 || !check_seq(out, 3, (int)i)) ++failures;
            consumed.store(i, std::memory_order_release);
        }
    });
    unsigned done = 0;
    for (unsigned i = 1; i <= ROUNDS && std::chrono::steady_clock::now() < deadline; ++i) {
        float old[12]{}, next[6];
        r.write(old, 6);
        r.request_flush();
        wait_until([&] { return !r.flush_pending(); });
        fill_seq(next, 3, (int)i);
        if (r.write(next, 3) != 3) ++failures;
        published.store(i, std::memory_order_release);
        wait_until([&] { return consumed.load(std::memory_order_acquire) == i; });
        done = i;
    }
    stop.store(true, std::memory_order_release);   // the consumer is between rounds here
    consumer.join();
    CHECK(failures.load() == 0);
    CHECK(done >= 1);
    // Fewer than this means the machine was too loaded for the rounds to prove much (each
    // backed-off wait costs a timer tick), and the rounds that DID run still had to be
    // clean. Reported rather than failed: load itself failing the gate is exactly what
    // the time budget exists to prevent.
    WARN_MESSAGE(done >= 200, "only " << done << " rounds fitted in the two-second budget");
}

TEST_CASE("ring: a pending rate change is due only once the old frames are gone") {
    PcmRing r;
    REQUIRE(r.init(16));
    CHECK_FALSE(r.rate_change_due(0));           // nothing pending is never due
    CHECK(r.rate_change_due(48000));             // pending on an empty ring: now
    float in[2 * 4];
    fill_seq(in, 4, 0);
    REQUIRE(r.write(in, 4) == 4);
    CHECK_FALSE(r.rate_change_due(48000));       // frames at the old rate are still queued
    r.request_flush();
    CHECK(r.rate_change_due(48000));             // ...unless they are about to be dropped
    r.reset();
    CHECK(r.rate_change_due(48000));
    r.deinit();
}

TEST_CASE("ring: the rate handshake plays the previous track's tail before the clock moves") {
    // F1. Track A (44.1 kHz) ends and its last frames sit in the ring unheard while track B
    // (48 kHz) is opened. Both sides follow the protocol the modules use: the producer marks
    // eos, waits for the ring to empty, asks, clears eos; the consumer keeps playing whatever
    // is queued at the old clock and answers only from an empty ring. Every frame must come
    // out at the clock it was decoded for, and the answer must come exactly once, after the
    // whole tail has been heard.
    PcmRing r;
    REQUIRE(r.init(64));
    constexpr int TAIL = 500, NEXT = 300;
    r.ask_rate(44100);
    r.set_out_rate(44100);                       // track A was answered long ago

    std::atomic<bool> no_answer{false};
    std::thread producer([&] {
        float buf[2 * 13];
        auto put = [&](int total, int base) {
            int done = 0;
            while (done < total) {
                size_t want = (size_t)(1 + ((done + base) % 13));
                if (done + (int)want > total) want = (size_t)(total - done);
                fill_seq(buf, want, base + done);
                size_t w = 0;
                while (w < want) w += r.write(buf + 2 * w, want - w);
                done += (int)want;
            }
        };
        put(TAIL, 0);                            // track A's tail, at 44.1 kHz
        // Track B decoded its first frame, at 48 kHz: drain, ask, clear eos, wait.
        r.set_eos(true);
        wait_until([&] { return r.available() == 0; });
        r.ask_rate(48000);
        r.set_eos(false);
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        wait_until([&] { return r.out_rate() == 48000 || std::chrono::steady_clock::now() > until; });
        if (r.out_rate() != 48000) { no_answer = true; return; }
        put(NEXT, 1000);                         // track B, at 48 kHz
    });

    uint32_t out = 44100, pending = 0;
    int tail_seen = 0, next_seen = 0, wrong_clock = 0, answers = 0, torn = 0;
    int tail_seen_at_answer = -1, idle = 0;
    float buf[2 * 7];
    const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (tail_seen + next_seen < TAIL + NEXT && !no_answer &&
           std::chrono::steady_clock::now() < give_up) {
        const uint32_t asked = r.rate();
        pending = (asked == out) ? 0 : asked;
        if (r.rate_change_due(pending)) {        // the consumer's rule, as the audio task applies it
            out = pending;
            pending = 0;
            r.set_out_rate(out);
            answers++;
            tail_seen_at_answer = tail_seen;
        }
        const size_t n = r.read(buf, 7);
        for (size_t i = 0; i < n; i++) {
            const int v = (int)buf[2 * i];
            if (buf[2 * i + 1] != -(float)v) torn++;
            if (v < 1000) { tail_seen++; if (out != 44100) wrong_clock++; }
            else          { next_seen++; if (out != 48000) wrong_clock++; }
        }
        if (n) idle = 0;
        else if (++idle < 64) std::this_thread::yield();
        else std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    producer.join();
    CHECK_FALSE(no_answer.load());
    CHECK(tail_seen == TAIL);
    CHECK(next_seen == NEXT);
    CHECK(torn == 0);
    CHECK(wrong_clock == 0);
    CHECK(answers == 1);
    CHECK(tail_seen_at_answer == TAIL);          // answered only after the whole tail played
    r.deinit();
}

TEST_CASE("ring: an answer that comes after the producer stopped waiting is still followed") {
    // An AirPlay session started while the output was still moving out of another source. The
    // producer's bounded wait ran out, it started on the fallback, and the consumer's answer —
    // 44.1 kHz — arrived after it had stopped looking. Read once, the handshake left 44.1 kHz
    // frames converted to 48 playing at 44.1 for the whole session: 8 % slow, and a drop every
    // 0.6 s to keep up.
    PcmRing r;
    REQUIRE(r.init(16));
    r.ask_rate(44100);
    REQUIRE(r.out_rate() == 0);                      // the question is open...
    uint32_t clock = r.out_rate() ? r.out_rate() : PIPE_OUT_RATE;   // ...and the wait ran out
    CHECK(clock == 48000);
    CHECK_FALSE(clock_moved(clock, r.out_rate()));   // an open question says nothing

    r.set_out_rate(44100);                           // the consumer answers, late
    REQUIRE(clock_moved(clock, r.out_rate()));
    clock = r.out_rate();
    CHECK(clock == 44100);
    CHECK_FALSE(clock_moved(clock, r.out_rate()));   // the same answer read again: settled

    r.ask_rate(48000);                               // the next track asks: open, not news
    CHECK_FALSE(clock_moved(clock, r.out_rate()));
    r.set_out_rate(44100);                           // the session's clock is locked: same one
    CHECK_FALSE(clock_moved(clock, r.out_rate()));
    r.set_out_rate(96000);                           // and if it ever does move, that is news
    CHECK(clock_moved(clock, r.out_rate()));

    CHECK_FALSE(clock_moved(0, 44100));              // nothing settled yet: nothing to move
    r.deinit();
}

TEST_CASE("ring: a new owner is a new rate question, and the same owner keeps its own") {
    // The session's clock is chosen by its first track's question, but
    // the consumer read whatever rate the ring held — the boot default, or the previous
    // producer's — before the new one had asked, settled the session on it, and a 44.1 kHz
    // album after boot played converted to 48 kHz for its whole length.
    PcmRing r;
    REQUIRE(r.init(16));
    CHECK(r.rate() == 0);                            // nobody has asked yet
    REQUIRE(r.claim(PcmRing::OWNER_RADIO));
    r.ask_rate(48000);
    REQUIRE(r.rate() == 48000);
    REQUIRE(r.claim(PcmRing::OWNER_RADIO));          // a reconnect, a track boundary: still its own
    CHECK(r.rate() == 48000);

    r.release(PcmRing::OWNER_RADIO);
    REQUIRE(r.claim(PcmRing::OWNER_FILE));           // another producer: not its question
    CHECK(r.rate() == 0);
    r.ask_rate(44100);
    CHECK(r.rate() == 44100);
    REQUIRE(r.claim(PcmRing::OWNER_AIRPLAY));        // taken straight from an owner, too
    CHECK(r.rate() == 0);
    r.deinit();
}

TEST_CASE("ring: every owner is read back as itself, and its frames still flow") {
    // USB audio: OWNER_USB is 4, and with the old two-bit owner field its claim set READING —
    // owner() answered NONE and read() refused for ever. Each owner, the new one included, must
    // survive a claim whole and leave the consumer able to read.
    const PcmRing::Owner owners[] = {PcmRing::OWNER_FILE, PcmRing::OWNER_RADIO,
                                     PcmRing::OWNER_AIRPLAY, PcmRing::OWNER_USB};
    PcmRing r;
    REQUIRE(r.init(64));
    int start = 0;
    for (PcmRing::Owner who : owners) {
        REQUIRE(r.claim(who));
        CHECK(r.owner() == who);
        float in[2 * 8], out[2 * 8];
        fill_seq(in, 8, start);
        CHECK(r.write(who, r.generation(), in, 8) == 8);
        CHECK(r.available() == 8);
        CHECK(r.read(out, 8) == 8);
        CHECK(check_seq(out, 8, start));
        CHECK(r.set_eos(who, r.generation(), true));
        CHECK(r.eos());
        CHECK(r.owner() == who);                     // EOS sits next to the owner bits
        start += 8;
    }
    r.release(PcmRing::OWNER_USB);
    CHECK(r.owner() == PcmRing::OWNER_NONE);
    r.deinit();
}

TEST_CASE("ring: a new session is the owner's to ask for, and is taken once") {
    PcmRing r;
    REQUIRE(r.init(16));
    REQUIRE(r.claim(PcmRing::OWNER_USB));
    CHECK_FALSE(r.request_new_session(PcmRing::OWNER_RADIO));   // not its ring
    CHECK_FALSE(r.take_new_session());

    CHECK(r.request_new_session(PcmRing::OWNER_USB));
    CHECK(r.take_new_session());                     // the consumer acts on it once
    CHECK_FALSE(r.take_new_session());

    // Asked, then the ring changed hands before the consumer looked: the request belonged to
    // a producer that is no longer heard, and must not unlock the next one's session.
    CHECK(r.request_new_session(PcmRing::OWNER_USB));
    REQUIRE(r.claim(PcmRing::OWNER_RADIO));
    CHECK_FALSE(r.take_new_session());
    r.deinit();
}

TEST_CASE("fallback_rate: a rate refused inside the range falls back to its family's base") {
    // What this board clocks: 44.1 kHz is the driver's floor, 192 the audio PLL's ceiling.
    constexpr uint32_t LO = 44100, HI = 192000;
    // The port can refuse a rate it could make in principle — while the input port holds
    // the PLL, or when a drain does not finish. The fallback must then be a rate that is
    // always makeable and cheap to convert to, not the top of the family: retune() tries it
    // exactly once, and a refused fallback leaves the clock where it is.
    CHECK(fallback_rate(96000,  LO, HI) == 48000);
    CHECK(fallback_rate(88200,  LO, HI) == 44100);
    CHECK(fallback_rate(176400, LO, HI) == 44100);
    CHECK(fallback_rate(192000, LO, HI) == 48000);
    // The base rates answer themselves: retune() sees dst == hz and does not try again.
    CHECK(fallback_rate(44100, LO, HI) == 44100);
    CHECK(fallback_rate(48000, LO, HI) == 48000);
    // A floor above the base still lands on the lowest member the clock can make.
    CHECK(fallback_rate(48000, 96000, HI) == 96000);
    CHECK(fallback_rate(44100, 96000, HI) == 176400);
}

TEST_CASE("the session's clock lock survives a reset between tracks") {
    // The output clock is settled once, by the first track a source plays, and the
    // producer reads this to know it need not drain the previous track's tail before asking
    // for a rate. reset() runs BETWEEN tracks of the same session — a flush, a seek, a new
    // file — and clearing the lock there would quietly let the next track retune the clock,
    // which is the click the whole stage exists to remove. It is the consumer that clears it,
    // and only when the source changes.
    PcmRing ring;
    REQUIRE(ring.init(1024));

    CHECK_FALSE(ring.clock_locked());        // nothing has settled a clock yet
    ring.set_clock_locked(true);
    CHECK(ring.clock_locked());

    ring.reset();
    CHECK(ring.clock_locked());

    ring.request_flush();
    ring.reset();
    CHECK(ring.clock_locked());

    // And it is plain state, not a latch: the consumer takes it back on a source change.
    ring.set_clock_locked(false);
    CHECK_FALSE(ring.clock_locked());
    ring.deinit();
}
