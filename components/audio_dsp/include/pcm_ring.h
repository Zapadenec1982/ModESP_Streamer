/**
 * @file pcm_ring.h
 * @brief Single-producer / single-consumer ring of interleaved stereo float frames.
 *
 * The seam between a decoder and the audio core. The producer is whoever is filling it — the
 * file player today, a network stream later — and the consumer is always the audio task,
 * which must never block: a task that waits on a decoder produces a click, not a delay.
 * So neither side takes a lock, and both report how much they actually moved rather than
 * insisting on a whole request.
 *
 * Buffer lives in PSRAM. Two seconds of stereo float is 768 KB, which is nothing here and is
 * what lets a card read stall for a moment without the output noticing.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace audio_core {

/// The rate every converter in this firmware lands on.
///
/// The output clock follows the source now, so this is no longer "the rate" — it is the
/// rate to fall back to. A source asking for something the clock cannot make has to
/// convert, every converter here is built to reach exactly this rate, and so the clock has
/// to be at it before the converted samples arrive.
constexpr uint32_t PIPE_OUT_RATE = 48000;

/// Where a rate the clock cannot make should be converted TO.
///
/// Not simply PIPE_OUT_RATE, which is what this used to do and what made 22.05 kHz arrive at
/// 48 kHz through a ratio of 2.1769. Every sample rate in use belongs to one of two families:
/// 44.1 kHz and its multiples, and 48 kHz and its multiples. Staying inside the source's own
/// family turns awkward ratios into exact ones — 22.05 to 44.1 is exactly two — and an exact
/// ratio is not merely tidier. This resampler reduces its filter bank to the denominator of
/// the ratio in lowest terms, so 22.05 to 48 needs 320 filters and six hundred milliseconds
/// to build them, while 22.05 to 44.1 needs TWO and builds in single milliseconds. That
/// build is what emptied the ring at the start of such a track.
///
/// `min_hz` and `max_hz` are what the output clock can actually be moved to, which the port
/// reports. Returns 0 when the family has no member in range at all, which cannot happen for
/// any rate this firmware decodes but is the honest answer rather than a guess.
///
/// A rate INSIDE the range that the port refused anyway — the input port holding the PLL, a
/// multiple the PLL cannot reach, a drain that did not finish — lands on the family's base
/// rate, the lowest member the clock makes. It used to land on the highest, which sent a
/// refused 44.1 kHz to 176.4 and a four-times conversion with it, for nothing: no reason
/// that refuses a rate accepts a higher one.
inline uint32_t fallback_rate(uint32_t want, uint32_t min_hz, uint32_t max_hz) {
    if (!want || !min_hz || max_hz < min_hz) return 0;
    // 11025 divides every member of the 44.1 family and no member of the 48 family.
    uint32_t r = (want % 11025 == 0) ? 44100 : 48000;
    if (want > max_hz) {
        while (r * 2 <= max_hz) r *= 2;             // the highest member the clock can make
        return r >= min_hz ? r : 0;
    }
    while (r < min_hz) r *= 2;                      // the lowest member the clock can make
    return r <= max_hz ? r : 0;
}

class PcmRing {
public:
    enum Owner : uint8_t {
        OWNER_NONE = 0, OWNER_FILE = 1, OWNER_RADIO = 2, OWNER_AIRPLAY = 3, OWNER_USB = 4
    };
    /// Allocate `frames` of capacity (PSRAM preferred).
    ///
    /// Safe to call again, and a second call for the SAME size does nothing: both
    /// producers ask for the ring at startup and the consumer is already reading it by
    /// then, so reallocating underneath it is not an option.
    bool init(size_t frames);
    void deinit();

    /// Request a reset without waiting for an in-flight copy. New reads/writes are gated;
    /// the last active operation resets the indices before opening the next generation.
    void reset();
    bool reset(uint32_t expected_generation);
    bool reset(Owner who, uint32_t expected_generation);

    /// Bumps on every reset().
    uint32_t generation() const { return control_.load(std::memory_order_acquire) >> EPOCH_SHIFT; }
    bool reset_pending() const { return (control_.load(std::memory_order_acquire) & RESET) != 0; }

    /// Stop and start WITHOUT losing what is queued.
    ///
    /// Pausing by stopping the decoder does not pause anything: the queue still holds two
    /// seconds and they play out before the silence arrives, so the button appears to work
    /// only once the buffer empties — and then the music stops mid-waveform, which is a step
    /// and therefore a click. Pausing the CONSUMER instead is immediate, keeps the queue for
    /// an instant resume, and gives the fade something real to fade.
    void set_paused(bool on) { paused_.store(on, std::memory_order_release); }
    bool paused() const      { return paused_.load(std::memory_order_acquire); }

    /// Ask the consumer to fade out what is queued and then drop it, rather than dropping it
    /// underneath. The producer cannot do this itself: it does not own the samples already
    /// written, and it refills fast enough that the consumer never sees a gap to smooth.
    /// Cleared by reset(), which the consumer calls once the fade has finished.
    void request_flush() { control_.fetch_or(FLUSH, std::memory_order_acq_rel); }
    bool request_flush(Owner who, uint32_t expected_generation);
    bool flush_pending() const { return (control_.load(std::memory_order_acquire) & (FLUSH | RESET)) != 0; }

    /// Who is allowed to write, since there is more than one thing that can.
    ///
    /// The card player, the radio streamer, the AirPlay receiver and the USB audio device all
    /// write into this ring, and the selected source decides which of them is producing. Two producers
    /// interleaving their frames would not be a glitch, it would be noise, so a write from
    /// anyone but the owner is dropped instead. Claiming always succeeds and empties the
    /// ring — see claim().
    bool claim(Owner who);
    void release(Owner who);
    Owner owner() const { return (Owner)(control_.load(std::memory_order_acquire) & OWNER_MASK); }

    /// Returns frames actually written, which is fewer than asked when the ring is full,
    /// and 0 when the caller does not own the ring.
    size_t write(const float* stereo, size_t frames);
    size_t write(Owner who, const float* stereo, size_t frames);
    /// Reject a decoded block belonging to a previous session, even if the same owner
    /// has since reclaimed the ring (FILE -> RADIO -> FILE).
    size_t write(Owner who, uint32_t generation, const float* stereo, size_t frames);
    /// Returns frames actually read; the caller fills the remainder with silence.
    size_t read(float* stereo, size_t frames);

    size_t available() const;   ///< frames the consumer can read now
    size_t space() const;       ///< frames the producer can write now
    size_t capacity() const { return cap_; }
    /// 0..100, for a buffer meter. Cheap enough to call per block.
    int    fill_pct() const;

    /// The rate the frames in here are at.
    ///
    /// The output clock follows the source now, so the consumer has to be told what it is
    /// looking at. Set by the producer when it learns the format, before it writes anything
    /// at that rate; read by the audio task, which retunes I2S when it changes.
    ///
    /// 0: the owner has not asked yet. A claim by a new owner clears it, because what was
    /// left here is the previous producer's question, and a session's clock is chosen by its
    /// own first track. A claim by the owner it already has keeps it.
    void set_rate(uint32_t hz) { rate_.store(hz, std::memory_order_release); }
    uint32_t rate() const { return rate_.load(std::memory_order_acquire); }

    /// Ask for a rate and mark the answer as pending.
    ///
    /// The producer cannot tell a stale answer from a fresh one by reading a rate, so it
    /// clears the answer before asking. Zero means the question is still open.
    void ask_rate(uint32_t hz) {
        out_rate_.store(0, std::memory_order_relaxed);
        rate_.store(hz, std::memory_order_release);
    }

    /// What the output is ACTUALLY clocked at, written by the consumer.
    ///
    /// Asking is not the same as getting: not every rate can be made from the audio PLL,
    /// and a producer that assumes its own rate was accepted stops converting for an
    /// output that never moved. So the consumer answers here, and the producer reads the
    /// answer before it decides whether it has work to do — and keeps reading it after
    /// that, because the answer can come later than the producer was willing to wait for
    /// it (see clock_moved()).
    void set_out_rate(uint32_t hz) {
        if (hz) clock_rate_.store(hz, std::memory_order_release);
        out_rate_.store(hz, std::memory_order_release);
    }
    /// The rate the output was last known to run at; asking does not clear it. What a producer
    /// converts for when its question is not answered in time: a guess at a constant plays at
    /// the wrong speed whenever the clock was left somewhere else, and the answer that would
    /// correct it needs an empty ring, which the guessed frames keep full.
    uint32_t clock_rate() const { return clock_rate_.load(std::memory_order_acquire); }
    uint32_t out_rate() const { return out_rate_.load(std::memory_order_acquire); }

    /// Whether the output clock is settled for this listening session and will not move.
    ///
    /// The clock is chosen once, by the first track a source plays, and left alone until the
    /// source changes — because moving it is heard as a click at every track boundary of a
    /// mixed-rate playlist. The mechanism is the DAC's: a PCM5102A takes its internal PLL from
    /// the bit clock, retuning stops that clock, and the PLL unlocking and relocking is an
    /// event in the analogue part of the converter. Muting the data path around it does not
    /// help and was measured not to.
    ///
    /// A producer that knows the clock is settled can skip the one thing a moving clock
    /// forced on it: draining the previous track's tail before asking for a rate, so that the
    /// tail could not be replayed at a new clock. With the clock fixed there is nothing to
    /// replay it at, the tail simply plays, and a 44.1/48 playlist is as gapless as an album.
    ///
    /// Written by the consumer, which is the only thing that can retune; read by producers.
    void set_clock_locked(bool on) { clock_locked_.store(on, std::memory_order_release); }
    bool clock_locked() const { return clock_locked_.load(std::memory_order_acquire); }

    /// Ask for a new listening session: the consumer forgets the clock it settled on, so the
    /// owner's next ask_rate() can move it.
    ///
    /// Every other producer converts a second rate into the session's clock, which is right
    /// for a playlist. A computer playing over USB is different: it chooses the rate itself,
    /// the panel's clock is what paces it (asynchronous feedback), and it can switch from
    /// 44.1 to 96 kHz between two tracks. Following it costs the clock's relock at the switch
    /// and keeps every sample as sent.
    ///
    /// Only the owner can ask, and a new owner's claim drops a request it did not make.
    /// The consumer takes it once (take_new_session) and treats it as a source change does.
    ///
    /// The owner asks for the session's rate after this, never before: the release store of
    /// ask_rate() then carries this request with it, and a consumer that reads the new rate can
    /// count on finding the request, even one made after its own take_new_session() came back
    /// empty.
    bool request_new_session(Owner who) {
        if (owner() != who) return false;
        session_req_.store((uint8_t)who, std::memory_order_release);
        return true;
    }
    /// Consumer side: true once per request, and only for the ring's current owner.
    bool take_new_session() {
        const uint8_t who = session_req_.exchange(0, std::memory_order_acq_rel);
        return who != OWNER_NONE && who == owner();
    }

    /// What the consumer holds on either side of the ring, for a producer that paces itself
    /// by the ring's fill rather than by its own input — the AirPlay receiver, whose timing
    /// engine has to know how late what it writes now will be heard.
    ///
    /// `prime` is the cushion the consumer waits for after a flush before it plays anything:
    /// a producer that keeps the ring shallower than that waits for it forever. `downstream`
    /// is how many frames sit between the ring and the pins — the consumer's block, the
    /// limiter's look-ahead and the port's DMA. Written by the consumer at startup and again
    /// whenever the output rate moves; zero until then.
    void set_consumer(uint32_t prime, uint32_t downstream) {
        prime_.store(prime, std::memory_order_relaxed);
        downstream_.store(downstream, std::memory_order_release);
    }
    uint32_t prime_frames() const      { return prime_.load(std::memory_order_relaxed); }
    uint32_t downstream_frames() const { return downstream_.load(std::memory_order_acquire); }

    /// Whether what this ring holds is being heard right now: the consumer reads it for the
    /// selected source and its gates are not shut. Written by the consumer every block; read by a
    /// producer about to claim the ring from someone else (claim_after_fade below).
    void set_consumer_live(bool on) { live_.store(on, std::memory_order_release); }
    bool consumer_live() const      { return live_.load(std::memory_order_acquire); }

    /// Whether nothing at all is heard, whatever the source: the consumer's output gate is shut
    /// and the silence behind it has filled the port's TX queue, so the last of what played has
    /// left the pins — what the amplifier waits for before it goes off. True until the consumer
    /// first says otherwise. Written by the consumer every block; read by a module about to do
    /// what a listener would hear, such as a flash write, which stops both cores' caches.
    void set_output_quiet(bool on) { quiet_.store(on, std::memory_order_release); }
    bool output_quiet() const      { return quiet_.load(std::memory_order_acquire); }

    /// The producer has no more data. The consumer uses it to tell "track finished" from
    /// "decoder is behind", which look identical from an empty ring.
    void set_eos(bool on) {
        if (on) control_.fetch_or(EOS, std::memory_order_acq_rel);
        else control_.fetch_and(~EOS, std::memory_order_acq_rel);
    }
    bool set_eos(Owner who, uint32_t generation, bool on) {
        uint32_t c = control_.load(std::memory_order_acquire);
        for (;;) {
            if ((c & OWNER_MASK) != (uint32_t)who || (c >> EPOCH_SHIFT) != generation)
                return false;
            const uint32_t next = on ? (c | EOS) : (c & ~EOS);
            if (control_.compare_exchange_weak(c, next, std::memory_order_acq_rel)) return true;
        }
    }
    bool eos() const { return (control_.load(std::memory_order_acquire) & EOS) != 0; }

    /// Whether a pending rate request may be acted on NOW — the consumer's side of a rate
    /// change. `pending` is the rate asked for, 0 when nothing is.
    ///
    /// The producer asks for the next track's rate on its first decoded frame, and at that
    /// moment the ring still holds the tail of the previous one — up to half a second that
    /// nobody has heard. Retuning underneath it plays that tail at the new clock: 44.1 kHz
    /// material at 48 is 8.8 % sharp, a semitone and a half, on every mixed-rate playlist.
    /// So a request waits until the old frames are gone. A pending flush is the exception:
    /// those frames are about to be dropped anyway, and the fade that precedes the drop is
    /// the silence the retune wants.
    bool rate_change_due(uint32_t pending) const {
        return pending != 0 && (available() == 0 || flush_pending());
    }

private:
    // Three bits of owner: OWNER_USB is 4. With the old two, its claim would have set READING.
    static constexpr uint32_t OWNER_MASK = 7;
    static constexpr uint32_t READING = 1u << 3, WRITING = 1u << 4;
    static constexpr uint32_t RESET = 1u << 5, RESETTING = 1u << 6, FLUSH = 1u << 7;
    static constexpr uint32_t EOS = 1u << 8;
    static constexpr unsigned EPOCH_SHIFT = 9;
    static constexpr uint32_t EPOCH_STEP = 1u << EPOCH_SHIFT;
    static constexpr uint32_t ANY_GENERATION = UINT32_MAX;
    static constexpr uint32_t ANY_OWNER = UINT32_MAX;
    // Active bits protect copies, not a critical section: the opposite side continues
    // normally and reset/claim never waits for either task. Only a pending reset gates it.
    std::atomic<uint32_t> control_{0};
    bool enter(uint32_t operation, uint32_t who, uint32_t generation, uint32_t& entered);
    void leave(uint32_t operation);
    void finish_reset();
    bool reset_impl(uint32_t who, uint32_t generation);
    size_t write_impl(uint32_t who, uint32_t generation, const float* stereo, size_t frames);
#if defined(HOST_BUILD)
    friend struct PcmRingTestAccess;
    // Deterministic concurrency tests stop the real copy at its entry point. Absent on MCU.
    void (*copy_hook_)(void*) = nullptr;
    void* copy_hook_ctx_ = nullptr;
#endif
    float*  buf_ = nullptr;
    size_t  cap_ = 0;                       ///< in frames
    std::atomic<uint32_t> head_{0};         ///< producer writes here
    std::atomic<uint32_t> tail_{0};         ///< consumer reads here
    std::atomic<bool>     paused_{false};
    std::atomic<uint32_t> rate_{0};
    std::atomic<uint32_t> out_rate_{48000};
    std::atomic<uint32_t> clock_rate_{48000};
    std::atomic<bool>     clock_locked_{false};
    std::atomic<uint8_t>  session_req_{0};      ///< the Owner that asked for a new session
    std::atomic<uint32_t> prime_{0};
    std::atomic<uint32_t> downstream_{0};
    std::atomic<bool>     live_{false};
    std::atomic<bool>     quiet_{true};
};

/// The rest of the rate handshake, for a producer that is already playing.
///
/// A producer asks for its rate, waits a bounded time for out_rate(), and when the wait runs
/// out starts on PIPE_OUT_RATE: it cannot wait for ever. The consumer answers all the same,
/// only later. It retunes once the old frames have played out and its envelope has closed,
/// and behind a source change that can outlast any wait a producer can afford. A producer
/// that never looks again converts for a clock the output is not at, for as long as it plays.
/// An AirPlay session that started while the panel was booting fed 44.1 kHz frames converted to
/// 48 into an output that had settled at 44.1: 8 % slow, and the receiver's timing engine threw
/// away 48 ms every 0.6 s to keep up — a click each time, until the source was switched away
/// and back.
///
/// So the wait is only where a producer starts. While it plays it keeps reading out_rate(),
/// and this says when what it reads means the clock it converts for is wrong: an answer other
/// than `clock`. Zero is a question still open — the producer's own, for its next track —
/// and says nothing about the one playing; a producer with no clock yet has nothing to move.
inline bool clock_moved(uint32_t clock, uint32_t answer) {
    return clock != 0 && answer != 0 && answer != clock;
}

/// How many 5 ms naps a producer waits for the consumer to acknowledge a flush.
///
/// The consumer acknowledges once its envelope has shut, and the envelope closes in
/// FadeGate::CLOSE_S, 100 ms, plus the block it notices the request in. Twenty naps were enough for
/// the old 10 ms fade. Against a 100 ms one they ran out first, and the producer's fallback, a
/// reset of its own, dropped the queue under a fade still in progress: the cut the fade was there
/// to prevent. Sixty naps leave room for a busy consumer.
constexpr int FLUSH_WAIT_NAPS = 60;

/// Take the ring for `who` without cutting off what another producer queued.
///
/// claim() empties the ring on the spot. If that producer's music is playing, the consumer is left
/// mid-waveform with nothing to fade, and the result is a step. This was the second road to the
/// click on a source change: switching from the card to
/// a station, or AirPlay arriving over the radio. So while the consumer says the ring is being
/// heard, the queue goes back to it first, to fade and drop. That is the same flush a producer asks
/// for at its own track change. The claim waits for it, but only so long: `nap` sleeps about 5 ms,
/// and after FLUSH_WAIT_NAPS of them the claim goes ahead anyway, as it always did. Claiming a ring
/// the caller already owns, or one nobody is hearing, does not wait at all.
template <class Nap>
bool claim_after_fade(PcmRing& ring, PcmRing::Owner who, Nap nap) {
    const PcmRing::Owner was = ring.owner();
    if (was != who && ring.consumer_live() && ring.available() > 0) {
        const uint32_t generation = ring.generation();
        if (ring.request_flush(was, generation)) {
            for (int i = 0; i < FLUSH_WAIT_NAPS && ring.flush_pending() &&
                            ring.generation() == generation && ring.consumer_live(); i++) {
                nap();
            }
        }
    }
    return ring.claim(who);
}

/// The one ring between the player and the audio core.
PcmRing& pcm_ring();

} // namespace audio_core
