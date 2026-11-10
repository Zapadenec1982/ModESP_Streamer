/**
 * @file pcm_ring.cpp
 * @brief SPSC ring. One index each, acquire/release ordering, no locks anywhere.
 */

#include "pcm_ring.h"

#include <cstring>
#include <cstdlib>

#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#endif

namespace audio_core {
namespace {

void* ring_alloc(size_t bytes) {
#if defined(ESP_PLATFORM)
    // PSRAM: this is a second of audio, not something to spend internal SRAM on. The audio
    // task reads it in 240-frame blocks, so the access pattern is long and sequential —
    // exactly what the cache handles well.
    void* p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) return p;
    return heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
#else
    return malloc(bytes);
#endif
}

void ring_free(void* p) {
#if defined(ESP_PLATFORM)
    heap_caps_free(p);
#else
    free(p);
#endif
}

} // namespace

bool PcmRing::init(size_t frames) {
    if (frames < 2) return false;
    // Already the right size: leave it alone. Two modules ask for this ring now — the card
    // player and the radio — and whichever initialises second used to FREE the buffer the
    // audio task was already reading from and hand back a different one. A consumer reading
    // released memory is not a thing that fails loudly; it is a thing that crackles.
    // And a ring that exists is never freed here for another size: the audio task and a
    // producer may be inside it. The caller is told no, and deinit() is its own decision.
    if (buf_) return cap_ == frames + 1;
    // One slot stays empty so full and empty are distinguishable without a separate count,
    // which is what keeps this lock-free with a single index per side.
    buf_ = static_cast<float*>(ring_alloc((frames + 1) * 2 * sizeof(float)));
    if (!buf_) return false;
    cap_ = frames + 1;
    head_.store(0, std::memory_order_relaxed);
    tail_.store(0, std::memory_order_relaxed);
    set_eos(false);
    return true;
}

void PcmRing::deinit() {
    if (buf_) ring_free(buf_);
    buf_ = nullptr;
    cap_ = 0;
}

void PcmRing::reset() {
    reset_impl(ANY_OWNER, ANY_GENERATION);
}

bool PcmRing::reset(uint32_t expected_generation) {
    return reset_impl(ANY_OWNER, expected_generation);
}

bool PcmRing::reset(Owner who, uint32_t expected_generation) {
    return reset_impl((uint32_t)who, expected_generation);
}

bool PcmRing::reset_impl(uint32_t who, uint32_t generation) {
    uint32_t c = control_.load(std::memory_order_acquire);
    for (;;) {
        if ((who != ANY_OWNER && (c & OWNER_MASK) != who) ||
            (generation != ANY_GENERATION && (c >> EPOCH_SHIFT) != generation)) return false;
        // The current request already gates all copies. A consumer acknowledging its
        // pending flush must not invalidate the new owner's freshly captured generation.
        if (c & RESET) { finish_reset(); return true; }
        if (control_.compare_exchange_weak(c, ((c + EPOCH_STEP) | RESET) & ~EOS,
                                          std::memory_order_acq_rel)) break;
    }
    finish_reset();
    return true;
}

bool PcmRing::request_flush(Owner who, uint32_t generation) {
    uint32_t c = control_.load(std::memory_order_acquire);
    for (;;) {
        if ((c & OWNER_MASK) != (uint32_t)who || (c >> EPOCH_SHIFT) != generation) return false;
        if (control_.compare_exchange_weak(c, c | FLUSH, std::memory_order_acq_rel)) return true;
    }
}

void PcmRing::finish_reset() {
    uint32_t c = control_.load(std::memory_order_acquire);
    for (;;) {
        if (!(c & RESET) || (c & (READING | WRITING | RESETTING))) return;
        if (control_.compare_exchange_weak(c, c | RESETTING, std::memory_order_acq_rel)) break;
    }
    // We reserved the reset with no active copies. A new entry can briefly set its active
    // bit, but sees RESETTING and leaves without touching indices or samples.
    head_.store(0, std::memory_order_relaxed);
    tail_.store(0, std::memory_order_relaxed);
    // Claims that arrive while resetting may be coalesced: no new frames can have entered.
    // Keep their owner/epoch and acknowledge flush in the SAME publication that opens I/O.
    control_.fetch_and(~(RESET | RESETTING | FLUSH), std::memory_order_release);
}

bool PcmRing::enter(uint32_t operation, uint32_t who, uint32_t generation, uint32_t& entered) {
    const uint32_t before = control_.fetch_or(operation, std::memory_order_acq_rel);
    if (before & operation) return false;   // another caller owns this direction
    entered = before | operation;
    if ((before & (RESET | RESETTING)) ||
        (who != ANY_OWNER && (before & OWNER_MASK) != who) ||
        (generation != ANY_GENERATION && (before >> EPOCH_SHIFT) != generation)) {
        leave(operation);
        return false;
    }
    return true;
}

void PcmRing::leave(uint32_t operation) {
    const uint32_t before = control_.fetch_and(~operation, std::memory_order_acq_rel);
    if (before & RESET) finish_reset();
}

size_t PcmRing::available() const {
    if (!buf_) return 0;
    const uint32_t before = control_.load(std::memory_order_acquire);
    if (before & (RESET | RESETTING)) return 0;
    const uint32_t h = head_.load(std::memory_order_acquire);
    const uint32_t t = tail_.load(std::memory_order_relaxed);
    const uint32_t after = control_.load(std::memory_order_acquire);
    if ((after & (RESET | RESETTING)) || (before >> EPOCH_SHIFT) != (after >> EPOCH_SHIFT)) return 0;
    return h >= t ? (h - t) : (cap_ - t + h);
}

size_t PcmRing::space() const {
    if (!buf_) return 0;
    if (reset_pending()) return 0;
    return cap_ - 1 - available();
}

int PcmRing::fill_pct() const {
    if (cap_ < 2) return 0;
    return (int)((available() * 100) / (cap_ - 1));
}

bool PcmRing::claim(Owner who) {
    uint32_t c = control_.load(std::memory_order_acquire);
    for (;;) {
        if ((c & OWNER_MASK) == (uint32_t)who) return true;
        // A new owner is a new rate question. Cleared before the owner is published, so the
        // consumer never reads the new owner together with the previous producer's rate —
        // nor with a new-session request the previous producer left behind.
        rate_.store(0, std::memory_order_release);
        session_req_.store(0, std::memory_order_release);
        const uint32_t next = ((c + EPOCH_STEP) & ~(OWNER_MASK | EOS)) | (uint32_t)who | RESET;
        if (control_.compare_exchange_weak(c, next, std::memory_order_acq_rel)) break;
    }
    finish_reset();
    return true;
}

void PcmRing::release(Owner who) {
    uint32_t c = control_.load(std::memory_order_acquire);
    while ((c & OWNER_MASK) == (uint32_t)who &&
           !control_.compare_exchange_weak(c, c & ~OWNER_MASK, std::memory_order_acq_rel)) {}
}

size_t PcmRing::write(Owner who, const float* stereo, size_t frames) {
    return write_impl((uint32_t)who, ANY_GENERATION, stereo, frames);
}

size_t PcmRing::write(Owner who, uint32_t generation, const float* stereo, size_t frames) {
    return write_impl((uint32_t)who, generation, stereo, frames);
}

size_t PcmRing::write(const float* stereo, size_t frames) {
    return write_impl(ANY_OWNER, ANY_GENERATION, stereo, frames);
}

size_t PcmRing::write_impl(uint32_t who, uint32_t generation, const float* stereo, size_t frames) {
    if (!buf_ || !frames) return 0;
    uint32_t entered;
    if (!enter(WRITING, who, generation, entered)) return 0;
    const uint32_t h = head_.load(std::memory_order_relaxed);
    const uint32_t t = tail_.load(std::memory_order_acquire);
    const size_t free_frames = (h >= t ? (cap_ - h + t) : (t - h)) - 1;
    if (frames > free_frames) frames = free_frames;
    if (!frames) { leave(WRITING); return 0; }

    // At most two copies: to the end of the buffer, then from the start.
    const size_t first = (h + frames <= cap_) ? frames : (cap_ - h);
#if defined(HOST_BUILD)
    if (copy_hook_) copy_hook_(copy_hook_ctx_);
#endif
    memcpy(buf_ + 2 * h, stereo, first * 2 * sizeof(float));
    if (frames > first) {
        memcpy(buf_, stereo + first * 2, (frames - first) * 2 * sizeof(float));
    }
    head_.store((uint32_t)((h + frames) % cap_), std::memory_order_release);
    const bool current = generation == ANY_GENERATION
        ? this->generation() == (entered >> EPOCH_SHIFT) : this->generation() == generation;
    leave(WRITING);
    return current ? frames : 0;
}

size_t PcmRing::read(float* stereo, size_t frames) {
    if (!buf_ || !frames) return 0;
    uint32_t entered;
    if (!enter(READING, ANY_OWNER, ANY_GENERATION, entered)) return 0;
    const uint32_t t = tail_.load(std::memory_order_relaxed);
    const uint32_t h = head_.load(std::memory_order_acquire);
    const size_t have = (h >= t) ? (h - t) : (cap_ - t + h);
    if (frames > have) frames = have;
    if (!frames) { leave(READING); return 0; }

    const size_t first = (t + frames <= cap_) ? frames : (cap_ - t);
#if defined(HOST_BUILD)
    if (copy_hook_) copy_hook_(copy_hook_ctx_);
#endif
    memcpy(stereo, buf_ + 2 * t, first * 2 * sizeof(float));
    if (frames > first) {
        memcpy(stereo + first * 2, buf_, (frames - first) * 2 * sizeof(float));
    }
    tail_.store((uint32_t)((t + frames) % cap_), std::memory_order_release);
    const bool current = generation() == (entered >> EPOCH_SHIFT);
    leave(READING);
    return current ? frames : 0;
}

PcmRing& pcm_ring() {
    static PcmRing ring;
    return ring;
}

} // namespace audio_core
