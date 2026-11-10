/**
 * @file audio_output_ring.cpp
 * @brief upstream's audio_output.h, on the player's PCM ring instead of an I2S port.
 *
 * Upstream's receiver has one consumer: a playback task that pulls up to 352 frames at a
 * time from audio_receiver_read() — the timing engine decides what is due — and hands them
 * to i2s_channel_write, which blocks, so the DMA paces it. The player's audio core owns the
 * only I2S port, so here the frames go into audio_core::pcm_ring() as float, under
 * PcmRing::OWNER_AIRPLAY, and the ring's fill paces the task instead: the audio task drains
 * the ring at the output clock, and holding it at a fixed depth holds the playback task to
 * exactly that rate.
 *
 * The depth is shallow on purpose, and a floor rather than a guess. The timing engine
 * schedules every frame against the sender's clock and asks, before each read, how long
 * until what it writes now is heard (audio_output_get_pipeline_us): the ring's fill plus
 * what the consumer holds downstream of it, its block and the I2S DMA. The ring can hold two
 * seconds, and every one of them would be latency the phone waits out on every pause and
 * seek. The floor is the consumer's priming cushion: after a flush it plays nothing until
 * that much is queued, and a producer that stopped short of it would wait forever. Both
 * numbers come from the consumer through the ring (PcmRing::set_consumer), none is copied.
 *
 * Whether the ring listens is the module's decision (airplay2_set_output): audio.source
 * says who owns the output. When it is not AirPlay, the task keeps reading at the output
 * rate and drops what it reads — what a speaker switched to another input does. The
 * session, its clock and its position stay live, and switching back resumes in step with
 * the phone.
 *
 * The rate is asked for and the answer waited for, exactly as the radio does, before
 * anything is written: the consumer retunes only once the ring is empty, and a producer
 * that kept it full while the question was open would have its 44.1 kHz frames played at
 * 48, 8.8 % sharp, for as long as it kept writing. Refused, the frames are converted with
 * the player's own RateConverter to whatever the clock settled on. And the answer is
 * followed, not only waited for: one that arrives after the wait has given up — the clock
 * still moving out of another source — or that changes later, is the clock the frames are
 * converted for from then on (follow_clock(), audio_core::clock_moved()).
 */

#include <atomic>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "audio_pipe.h"
#include "pcm_ring.h"

extern "C" {
#include "airplay2.h"
#include "airplay2_port.h"
#include "audio_output.h"
#include "audio_receiver.h"
}

namespace {

const char* TAG = "airplay2_out";

using audio_core::PcmRing;

constexpr size_t   FRAME       = 352;         ///< what upstream reads per call (ALAC's frame)
constexpr size_t   READ_MAX    = FRAME + 1;   ///< upstream asks for one more, and so do we
constexpr uint32_t RATE        = 44100;       ///< what an AirPlay stream is at unless told
/// Frames above the consumer's priming cushion to keep queued: two reads' worth, so the
/// fill never dips under the cushion between one write and the next.
constexpr uint32_t ABOVE_PRIME = 2 * FRAME;
/// Nothing due for this long and the stream is idle — paused, or between sessions — rather
/// than late. Said to the ring as end-of-stream, or the audio core counts every empty block
/// of a paused phone as an underrun.
constexpr int64_t  IDLE_US     = 60 * 1000;
/// How long to wait for the consumer to answer a rate request; the radio waits as long.
constexpr int      ASK_POLLS   = 60;          ///< × 5 ms
/// Output room for the converter. Any size works — push_converted() hands it no more than
/// RateConverter::max_in() says fits — and this one takes a whole read at up to twice the
/// source rate, which is as far as fallback_rate() ever moves a clock within its family.
constexpr size_t   CVT_FRAMES  = 2 * READ_MAX + 16;

struct Out {
    TaskHandle_t          task = nullptr;
    std::atomic<bool>     run{false};
    std::atomic<bool>     want{false};         ///< the module's say: is the ring ours
    std::atomic<bool>     flush{false};
    std::atomic<uint32_t> src_rate{RATE};
    std::atomic<uint32_t> underruns{0};
    // Playback task only:
    bool                  owner  = false;
    uint32_t              generation = 0;
    bool                  idle   = true;
    uint32_t              asked  = 0;          ///< the source rate the clock was asked for
    uint32_t              clock  = RATE;       ///< what the consumer said the output is at
    bool                  direct = true;       ///< clock == source rate: no conversion
    audio_core::RateConverter conv;
};
Out g;

// In PSRAM (audio_output_init): 10 KB of scratch the internal heap cannot spare, touched
// only by the playback task, which never runs with the cache off.
int16_t* s_pcm = nullptr;             ///< READ_MAX stereo frames, as the receiver gives them
float*   s_f32 = nullptr;             ///< the same, as float
float*   s_cvt = nullptr;             ///< CVT_FRAMES stereo frames out of the converter

uint32_t target_frames() {
    return audio_core::pcm_ring().prime_frames() + ABOVE_PRIME;
}

/// Convert `src` frames for an output at `out`, or pass them straight through at their own.
void use_clock(uint32_t src, uint32_t out) {
    g.clock = out;
    g.direct = (out == src);
    if (!g.direct && !g.conv.ensure(src, out)) {
        ESP_LOGE(TAG, "no memory for a %lu -> %lu converter", (unsigned long)src, (unsigned long)out);
        g.direct = true;                       // pitch is wrong, but silence is worse
    }
}

/// Ask the consumer for the source's rate and wait for its answer; convert if refused.
void settle_rate(uint32_t src) {
    auto& ring = audio_core::pcm_ring();
    ring.ask_rate(src);
    uint32_t out = 0;
    for (int i = 0; i < ASK_POLLS && !(out = ring.out_rate()); i++) vTaskDelay(pdMS_TO_TICKS(5));
    if (!out) {
        out = audio_core::pcm_ring().clock_rate();   // no answer yet: the clock it was left at
        ESP_LOGW(TAG, "no answer to %lu Hz; assuming %lu", (unsigned long)src, (unsigned long)out);
    }
    g.asked = src;
    use_clock(src, out);
    ESP_LOGI(TAG, "%lu Hz stream, output at %lu%s", (unsigned long)src, (unsigned long)out,
             g.direct ? "" : " (converted)");
}

/// The answer settle_rate() stopped waiting for, or a later one: the clock the output is
/// really at is the one to convert for. A session runs for hours on one question, so a
/// late answer left unread is not a late start but a wrong pitch and a drop every 0.6 s.
void follow_clock() {
    const uint32_t out = audio_core::pcm_ring().out_rate();
    if (!audio_core::clock_moved(g.clock, out)) return;
    ESP_LOGW(TAG, "output at %lu Hz, not the %lu assumed; converting for it now",
             (unsigned long)out, (unsigned long)g.clock);
    use_clock(g.asked, out);
}

/// Claim or release the ring on the producer side, which is the only side allowed to.
void follow_owner() {
    auto& ring = audio_core::pcm_ring();
    const bool want = g.want.load(std::memory_order_acquire);
    // Lost to nobody (another producer took the ring and let it go while this source stayed
    // selected): take it again, as USB does. Never back from an owner that still holds it.
    if (want && g.owner && ring.owner() == PcmRing::OWNER_NONE) g.owner = false;
    if (want != g.owner) {
        g.owner = want;
        if (want) {
            // Empties it: whatever was there is not ours. Faded first if it is still being heard
            // (pcm_ring.h, claim_after_fade).
            audio_core::claim_after_fade(ring, PcmRing::OWNER_AIRPLAY,
                                         [] { vTaskDelay(pdMS_TO_TICKS(5)); });
            g.generation = ring.generation();
            ring.set_paused(false);
            ring.set_eos(PcmRing::OWNER_AIRPLAY, g.generation, g.idle);
            g.asked = 0;                           // a new owner is a new question
            ESP_LOGI(TAG, "ring claimed");
        } else {
            ring.release(PcmRing::OWNER_AIRPLAY);
            ESP_LOGI(TAG, "ring released");
            return;
        }
    }
    // A consumer-acknowledged flush creates a new generation. Any in-flight
    // converted block was rejected by push(); new decoded data can now resume.
    if (g.owner && ring.owner() == PcmRing::OWNER_AIRPLAY &&
        g.generation != ring.generation()) {
        g.generation = ring.generation();
        g.conv.reset();
        ring.set_eos(PcmRing::OWNER_AIRPLAY, g.generation, g.idle);
    }
    const uint32_t src = g.src_rate.load(std::memory_order_acquire);
    if (g.owner && src != g.asked) settle_rate(src);
    else if (g.owner) follow_clock();
}

/// Real time, when nobody is listening: one read per frame period, so the timing engine sees
/// the output advancing exactly as it would if it were heard.
void pace_unheard(int64_t& next_us, size_t frames) {
    next_us += (int64_t)frames * 1000000 / g.src_rate.load(std::memory_order_relaxed);
    const int64_t wait = next_us - esp_timer_get_time();
    if (wait > 0) vTaskDelay(pdMS_TO_TICKS((wait + 999) / 1000));
    else if (wait < -100000) next_us = esp_timer_get_time();   // fell behind: do not sprint
}

/// Put `n` frames of float stereo into the ring, waiting for room. False: no longer ours.
bool push(const float* f, size_t n) {
    auto& ring = audio_core::pcm_ring();
    size_t done = 0;
    while (done < n && g.run.load(std::memory_order_relaxed)) {
        if (!g.want.load() || g.flush.load()) return false;
        const size_t w = ring.write(PcmRing::OWNER_AIRPLAY, g.generation, f + done * 2, n - done);
        if (w) { done += w; continue; }
        if (ring.owner() != PcmRing::OWNER_AIRPLAY || ring.generation() != g.generation)
            return false;
        vTaskDelay(1);                               // full: let the audio task drain it
    }
    return done == n;
}

/// The same through the converter, in pieces no larger than its output can take: handed more,
/// it fills the buffer, stops, and the rest of the input is lost without a sound.
bool push_converted(const float* f, size_t n) {
    const size_t step = g.conv.max_in(CVT_FRAMES);
    for (size_t off = 0; off < n; off += step) {
        const size_t in = n - off < step ? n - off : step;
        size_t made = 0;
        const float* out = g.conv.process(f + off * 2, in, s_cvt, CVT_FRAMES, made);
        if (!push(out, made)) return false;
    }
    return true;
}

void playback_task(void*) {
    auto& ring = audio_core::pcm_ring();
    int64_t next_us = esp_timer_get_time();
    int64_t last_audio_us = 0;
    bool wrote = false;                              ///< since the last flush or claim
    while (g.run.load(std::memory_order_acquire)) {
        const bool was_owner = g.owner;
        follow_owner();
        if (g.owner != was_owner) wrote = false;
        if (g.flush.exchange(false, std::memory_order_acq_rel)) {
            // The consumer fades what is queued and drops it; a producer may not reset the
            // ring underneath a reader. The converter's history belongs to what was dropped.
            if (g.owner) ring.request_flush(PcmRing::OWNER_AIRPLAY, g.generation);
            g.conv.reset();
            wrote = false;
        }

        const size_t n = audio_receiver_read(s_pcm, READ_MAX);
        const int64_t now = esp_timer_get_time();
        if (n) last_audio_us = now;
        const bool idle = !n && now - last_audio_us > IDLE_US;
        if (idle != g.idle) {
            g.idle = idle;
            if (g.owner) ring.set_eos(PcmRing::OWNER_AIRPLAY, g.generation, idle);
        }

        if (!g.owner) {
            pace_unheard(next_us, n ? n : FRAME);
            continue;
        }
        next_us = now;
        if (!n) {
            // Nothing due yet. Upstream writes a frame of silence here and lets the blocking
            // write be its clock; the ring is a clock only while it is being filled, so wait
            // out one frame period instead.
            vTaskDelay(pdMS_TO_TICKS(FRAME * 1000 / RATE));
            continue;
        }
        // An empty ring while playing means this task came back too late: what the timing
        // engine's own statistics call an output underrun.
        if (wrote && !g.idle && ring.available() == 0) g.underruns.fetch_add(1, std::memory_order_relaxed);

        for (size_t i = 0; i < n * 2; i++) s_f32[i] = (float)s_pcm[i] * (1.0f / 32768.0f);
        if (!(g.direct ? push(s_f32, n) : push_converted(s_f32, n))) {
            // Taken from us. The refused write did not wait, so wait here: a loop that never
            // sleeps starves core 1's idle task into the watchdog.
            if (ring.owner() != PcmRing::OWNER_AIRPLAY) pace_unheard(next_us, n);
            continue;
        }
        wrote = true;

        // The pace: let the audio task take the ring back down to the target, sleeping the
        // excess away in one go rather than a tick at a time.
        const uint32_t target = target_frames();
        size_t avail;
        while ((avail = ring.available()) > target && g.run.load(std::memory_order_relaxed) &&
               g.want.load() && ring.owner() == PcmRing::OWNER_AIRPLAY &&
               ring.generation() == g.generation && !g.flush.load(std::memory_order_relaxed)) {
            const uint32_t ms = (uint32_t)((avail - target) * 1000u / g.clock);
            vTaskDelay(pdMS_TO_TICKS(ms ? ms : 1));
        }
    }
    g.task = nullptr;
    vTaskDelete(nullptr);
}

} // namespace

// ── upstream's API ────────────────────────────────────────────────────────────────────────

extern "C" esp_err_t audio_output_init(void) {
    if (audio_core::pcm_ring().capacity() == 0) {
        ESP_LOGE(TAG, "no PCM ring — nothing has made one");
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_pcm) {
        s_pcm = (int16_t*)heap_caps_malloc(READ_MAX * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        s_f32 = (float*)heap_caps_malloc(READ_MAX * 2 * sizeof(float), MALLOC_CAP_SPIRAM);
        s_cvt = (float*)heap_caps_malloc(CVT_FRAMES * 2 * sizeof(float), MALLOC_CAP_SPIRAM);
        if (!s_pcm || !s_f32 || !s_cvt) {
            heap_caps_free(s_pcm); heap_caps_free(s_f32); heap_caps_free(s_cvt);
            s_pcm = nullptr; s_f32 = nullptr; s_cvt = nullptr;
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

extern "C" void audio_output_start(void) {
    if (g.task) return;
    g.run.store(true, std::memory_order_release);
    // At upstream's priority, which is what it has to outrank (the realtime receiver at 8):
    // audio_output.h, AUDIO_PLAYBACK_TASK_PRIORITY. Pinned to core 1 as upstream pins it;
    // the player's audio task there runs at 22 and is not affected.
    if (xTaskCreatePinnedToCore(playback_task, "ap_play", 4096, nullptr,
                                AUDIO_PLAYBACK_TASK_PRIORITY, &g.task, 1) != pdPASS) {
        g.run.store(false);
        ESP_LOGE(TAG, "playback task not created");
    }
}

extern "C" void audio_output_stop(void) {
    g.run.store(false, std::memory_order_release);
    for (int i = 0; i < 100 && g.task; i++) vTaskDelay(pdMS_TO_TICKS(10));
    if (!g.task && g.owner) {
        audio_core::pcm_ring().release(PcmRing::OWNER_AIRPLAY);
        g.owner = false;
    }
}

extern "C" void audio_output_flush(void) {
    g.flush.store(true, std::memory_order_release);
}

extern "C" void audio_output_set_source_rate(int rate) {
    // pace_unheard() steps by this rate's frame period. A rate that rounds it to nothing never
    // lets ap_play sleep, and ap_play outranks IDLE on core 1: the task watchdog restarts the
    // panel. The handlers refuse such a stream (airplay2_port.h); this holds the pace anyway.
    if (rate >= AIRPLAY_RATE_MIN_HZ && rate <= AIRPLAY_RATE_MAX_HZ)
        g.src_rate.store((uint32_t)rate, std::memory_order_release);
}

extern "C" uint32_t audio_output_get_hardware_latency_us(void) {
    // The modelled pipeline: the ring at its target plus what sits downstream of it.
    const uint64_t frames = (uint64_t)target_frames() + audio_core::pcm_ring().downstream_frames();
    return (uint32_t)(frames * 1000000u / g.clock);
}

extern "C" bool audio_output_get_pipeline_us(int64_t* now_us, uint32_t* pipeline_us) {
    if (!now_us || !pipeline_us || !g.owner) return false;   // unheard: the model is honest
    const auto& ring = audio_core::pcm_ring();
    *now_us = esp_timer_get_time();
    const uint64_t frames = (uint64_t)ring.available() + ring.downstream_frames();
    *pipeline_us = (uint32_t)(frames * 1000000u / g.clock);
    return true;
}

extern "C" uint32_t audio_output_get_underruns(void) {
    return g.underruns.load(std::memory_order_relaxed);
}

// ── ours ──────────────────────────────────────────────────────────────────────────────────

extern "C" void airplay2_set_output(bool on) {
    g.want.store(on, std::memory_order_release);
}
