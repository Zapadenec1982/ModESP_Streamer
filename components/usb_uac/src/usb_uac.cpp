/**
 * @file usb_uac.cpp
 * @brief The producer and the measurement (usb_uac.h); the USB side is uac_usb.c.
 *
 * Two tasks on core 1 — the core that keeps the controller's interrupt on time
 * (docs/CONCEPT.md, "USB"):
 * TinyUSB's device task, which also owns the interrupt (allocated where uac_usb_init() runs),
 * and the producer. Every millisecond the producer empties TinyUSB's receive FIFO, converts to
 * float and, when USB is the output, writes into the player's ring. The ring is the elastic
 * buffer; the feedback endpoint keeps it at its target.
 */

#include "usb_uac.h"
#include "uac_tone.h"
#include "uac_usb.h"

#include "pcm_ring.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

static const char* TAG = "usb_uac";

namespace usb_uac {
namespace {

using audio_core::PcmRing;

portMUX_TYPE      s_mux = portMUX_INITIALIZER_UNLOCKED;
Stats             s_last;
bool              s_started = false;
int               s_core = -1;
std::atomic<bool> s_output{false};
std::atomic<bool> s_signal{false};
SemaphoreHandle_t s_init_done = nullptr;   ///< given by device_task once uac_usb_init() has answered
bool              s_init_ok   = false;

/// How long start() waits for the PHY and TinyUSB, which come up in milliseconds.
constexpr TickType_t INIT_WAIT = pdMS_TO_TICKS(1000);

/// Larger than 1 ms of the widest format (97 frames × 8 B); a late wake-up reads in pieces.
constexpr uint32_t RAW_BYTES  = 4096;
constexpr uint32_t MAX_FRAMES = RAW_BYTES / 4;   // 16-bit stereo is the most frames per byte

/// Feedback: the relative correction is (target − fill) / GAIN_FRAMES_PER_UNIT, i.e. one
/// second of frames per 100 %. A first-order loop with a one-second time constant — slow next
/// to the consumer's block ripple, which the fill's average smooths further, and fast next to
/// any clock drift. Clamped to ±0.5 %, far beyond two crystals' disagreement.
constexpr float FB_CLAMP = 0.005f;
/// Fill average: per 1 ms wake-up, a 64 ms time constant.
constexpr float FILL_ALPHA = 1.0f / 64.0f;
/// What the computer's sound must exceed to count as playing: −60 dBFS.
constexpr float SIGNAL_PEAK = 0.001f;
constexpr int64_t SIGNAL_HOLD_US = 300000;

void device_task(void*) {
    s_init_ok = uac_usb_init();
    xSemaphoreGive(s_init_done);
    if (!s_init_ok) {
        vTaskDelete(nullptr);
        return;
    }
    ESP_LOGI(TAG, "UAC2 device on the HS port (rhport 1)");
    for (;;) uac_usb_task();
}

State current_state() {
    if (!uac_usb_mounted() || g_uac.suspended) return State::waiting;
    return g_uac.alt ? State::playing : State::ready;
}

void nap() { vTaskDelay(pdMS_TO_TICKS(5)); }

void producer_task(void*) {
    auto* raw = static_cast<uint8_t*>(heap_caps_malloc(RAW_BYTES, MALLOC_CAP_SPIRAM));
    auto* pcm = static_cast<float*>(heap_caps_malloc(MAX_FRAMES * 2 * sizeof(float), MALLOC_CAP_SPIRAM));
    if (!raw || !pcm) {
        ESP_LOGE(TAG, "producer buffers");
        vTaskDelete(nullptr);
        return;
    }
    PcmRing& ring = audio_core::pcm_ring();

    uint32_t gen_seen = ~0u;
    uint8_t  alt = 0, fb = 0;
    uint32_t rate = 0;
    bool     wanted = false;        // USB was the output on the last pass
    Phase    phase = Phase::idle;
    uint32_t gen = 0;               // the ring generation we write under
    uint32_t flush_gen = 0;
    int64_t  phase_t0 = 0;
    bool     need_rate = false;     // the ring must be asked for `rate`
    float    fill_ema = -1.f;
    uint32_t fb_last = 0;
    int64_t  loud_at = 0;

    int64_t  sec_start = esp_timer_get_time();
    uint32_t rx_prev = g_uac.rx_bytes, pkts_prev = g_uac.pkts;
    uint32_t written = 0, dropped = 0, overflows = 0;
    uint32_t fill_min = ~0u, fill_max = 0, target = 0;
    double   fb_sum = 0;
    uint32_t fb_n = 0;
    float    peak[2] = {0, 0};
    uint32_t glitches = 0, streams = 0;
    Tone     tone;

    auto set_fb = [&](uint32_t v) {
        if (v != fb_last) {
            uac_usb_set_feedback(v);
            fb_last = v;
        }
    };

    for (;;) {
        vTaskDelay(1);
        const int64_t now = esp_timer_get_time();

        // ── The stream: alternate setting and clock ───────────────────────────────────────────
        // A suspended bus ends the stream as alt 0 would. On this board it is also what pulling
        // the cable looks like (uac_usb.c): the alternate setting stayed 2, and the ring was left
        // in run, starving — 63 000 underruns in eight minutes with nothing plugged in (24.10.2026).
        // A resume with the setting kept starts the stream again. A bus reset ends it as well:
        // TinyUSB drops its configuration there and calls back nothing, so it is TinyUSB that is
        // asked whether a host has the panel configured.
        const uint8_t host_alt = (uac_usb_mounted() && !g_uac.suspended) ? g_uac.alt : 0;
        if (g_uac.stream_gen != gen_seen || host_alt != alt || (alt && g_uac.rate != rate)) {
            gen_seen = g_uac.stream_gen;
            alt  = host_alt;
            fb   = uac_usb_frame_bytes(alt);
            rate = g_uac.rate;
            tone.restart();
            streams++;
            need_rate = alt != 0;
            ESP_LOGI(TAG, "stream: alt %u, %lu Hz, %u bytes/frame", alt, (unsigned long)rate, fb);
        }

        // ── The ring: ours while USB is the output ────────────────────────────────────────────
        // Taken from whoever holds it when USB becomes the output, and after that only from
        // nobody. s_output falls when usb_audio's on_update sees audio.source move on, later in
        // the main-loop pass than the producers' own modules; a producer that claimed in between
        // — the card, on a tap in the web — had the ring taken back within a millisecond,
        // cancelled its track and stayed idle, silent under its own source.
        const bool want = s_output.load(std::memory_order_acquire) && ring.capacity() > 0;
        const PcmRing::Owner holder = ring.owner();
        if (want && holder != PcmRing::OWNER_USB && (!wanted || holder == PcmRing::OWNER_NONE)) {
            // Fade whatever another producer queued instead of cutting it (pcm_ring.h).
            audio_core::claim_after_fade(ring, PcmRing::OWNER_USB, nap);
            gen = ring.generation();
            ring.set_paused(false);
            ring.set_eos(PcmRing::OWNER_USB, gen, alt == 0);
            need_rate = alt != 0;
            phase = Phase::idle;
        } else if (!want && holder == PcmRing::OWNER_USB) {
            ring.release(PcmRing::OWNER_USB);
        }
        wanted = want;
        const bool mine = want && ring.owner() == PcmRing::OWNER_USB;
        if (!mine) phase = Phase::idle;
        if (mine && ring.generation() != gen && phase != Phase::flush) {
            gen = ring.generation();   // the consumer reset the ring under us (a flush of its own)
        }

        // ── The clock: ask for the computer's rate, a new session when it changed ─────────────
        if (mine && alt && need_rate) {
            need_rate = false;
            ring.set_eos(PcmRing::OWNER_USB, gen, false);
            // Every stream, the first since the claim too: fade out what is left, and let the
            // reset make the consumer prime again. Without it a stream restarted at the same
            // rate met a consumer reading an empty ring with no cushion — six underruns at the
            // start of a 48 kHz run (U1, 24.10.2026) — and a new rate would retune under the
            // old rate's tail. The first stream is no different: a claim made while the
            // computer was silent marks the ring ended, an ended ring stops the consumer's
            // priming, and a first stream at the clock's own rate (nothing to retune, so nothing
            // primes after it) played with no cushion as well.
            flush_gen = gen;
            ring.request_flush(PcmRing::OWNER_USB, gen);
            phase = Phase::flush;
            phase_t0 = now;
        }
        if (mine && phase == Phase::flush &&
            (ring.generation() != flush_gen || now - phase_t0 > 500000)) {
            gen = ring.generation();
            phase = Phase::ask;
            ring.request_new_session(PcmRing::OWNER_USB);
            ring.ask_rate(rate);
            phase_t0 = now;
        }
        if (mine && phase == Phase::ask) {
            const uint32_t answer = ring.out_rate();
            if (answer == rate) {
                phase = Phase::run;
                fill_ema = -1.f;
            } else if (answer != 0) {
                phase = Phase::refused;
                ESP_LOGW(TAG, "the clock would not go to %lu Hz (answered %lu)",
                         (unsigned long)rate, (unsigned long)answer);
            }
        }
        if (mine && phase == Phase::run && audio_core::clock_moved(rate, ring.out_rate())) {
            need_rate = true;   // somebody moved the clock under a running stream: ask again
        }
        if (mine && !alt && phase != Phase::idle) {
            ring.set_eos(PcmRing::OWNER_USB, gen, true);   // the computer stopped: end, not starve
            phase = Phase::idle;
        }

        // ── The data: drain the FIFO, measure, write when running ─────────────────────────────
        if (alt && fb) {
            const bool run = mine && phase == Phase::run;
            for (;;) {
                const uint32_t avail = uac_usb_available();
                const uint32_t chunk = std::min<uint32_t>(avail - avail % fb, RAW_BYTES - RAW_BYTES % fb);
                if (!chunk) break;
                const uint32_t got = uac_usb_read(raw, uint16_t(chunk));
                const uint32_t frames = got / fb;
                const bool mute = g_uac.mute;
                float block_peak = 0;
                for (uint32_t i = 0; i < frames; i++) {
                    float l, r;
                    if (alt == 1) {
                        int16_t s[2];
                        memcpy(s, raw + i * fb, sizeof(s));
                        l = s[0] / 32768.f;
                        r = s[1] / 32768.f;
                    } else {
                        int32_t s[2];   // 24 bits, MSB-aligned in the 4-byte subslot
                        memcpy(s, raw + i * fb, sizeof(s));
                        l = s[0] / 2147483648.f;
                        r = s[1] / 2147483648.f;
                    }
                    peak[0] = std::max(peak[0], std::fabs(l));
                    peak[1] = std::max(peak[1], std::fabs(r));
                    block_peak = std::max(block_peak, std::max(std::fabs(l), std::fabs(r)));
                    tone.push(l);
                    pcm[2 * i]     = mute ? 0.f : l;
                    pcm[2 * i + 1] = mute ? 0.f : r;
                }
                if (block_peak > SIGNAL_PEAK) loud_at = now;
                if (run) {
                    const size_t w = ring.write(PcmRing::OWNER_USB, gen, pcm, frames);
                    written += uint32_t(w);
                    if (w < frames) {
                        overflows++;
                        dropped += uint32_t(frames - w);
                    }
                } else {
                    dropped += frames;
                }
            }
        }
        s_signal.store(alt && now - loud_at < SIGNAL_HOLD_US, std::memory_order_release);

        // ── Feedback: steer the computer by the ring's fill ───────────────────────────────────
        if (mine && alt && phase == Phase::run && fb) {
            const uint32_t prime = ring.prime_frames();
            target = (prime ? prime : rate / 10) + rate / 50;     // the cushion + 20 ms
            const float fill = float(ring.available()) + float(uac_usb_available() / fb);
            fill_ema = fill_ema < 0 ? fill : fill_ema + (fill - fill_ema) * FILL_ALPHA;
            const float c = std::clamp((float(target) - fill_ema) / float(rate), -FB_CLAMP, FB_CLAMP);
            const uint32_t nominal = uac_usb_nominal_feedback(rate);
            set_fb(uint32_t(double(nominal) * (1.0 + c) + 0.5));
            const uint32_t fill_ms = uint32_t(fill * 1000.f / float(rate));
            fill_min = std::min(fill_min, fill_ms);
            fill_max = std::max(fill_max, fill_ms);
            fb_sum += c;
            fb_n++;
        } else if (alt) {
            set_fb(uac_usb_nominal_feedback(rate));
        }

        // ── Once a second: publish ────────────────────────────────────────────────────────────
        if (now - sec_start >= 1000000) {
            // Per second of esp_timer, not per loop: the window closes up to a tick late.
            const uint64_t win = uint64_t(now - sec_start);
            auto per_s = [win](uint32_t n) { return uint32_t((uint64_t(n) * 1000000u + win / 2) / win); };
            sec_start = now;
            const uint32_t rx = g_uac.rx_bytes;
            const uint32_t pk = g_uac.pkts;
            Stats s;
            s.state       = current_state();
            s.phase       = phase;
            s.rate        = g_uac.rate;
            s.bits        = alt == 1 ? 16 : alt == 2 ? 24 : 0;
            s.output      = want;
            s.signal      = s_signal.load(std::memory_order_acquire);
            s.rx_frames   = fb ? per_s((rx - rx_prev) / fb) : 0;
            s.written     = per_s(written);
            s.dropped     = dropped;
            s.overflows   = overflows;
            s.fill_min_ms = fill_min == ~0u ? 0 : fill_min;
            s.fill_max_ms = fill_max;
            s.target_ms   = rate ? uint32_t(uint64_t(target) * 1000u / rate) : 0;
            s.fb_ppm      = fb_n ? int32_t(std::lround(fb_sum / fb_n * 1e6)) : 0;
            for (int c = 0; c < 2; c++) {
                s.peak_db[c] = peak[c] > 0 ? 20.f * std::log10(peak[c]) : -120.f;
            }
            s.tone_hz     = alt ? tone.close(rate, peak[0], glitches) : 0;
            s.glitches    = glitches;
            s.host_volume = g_uac.volume;
            s.host_mute   = g_uac.mute;
            s.streams     = streams;
            s.pkts        = per_s(pk - pkts_prev);
            s.lost        = g_uac.lost;
            s.loss_events = g_uac.loss_events;
            s.same_frame  = g_uac.same_frame;
            s.gap_max_us  = g_uac.gap_max_us;
            g_uac.gap_max_us = 0;
            s.core        = s_core;
            portENTER_CRITICAL(&s_mux);
            s_last = s;
            portEXIT_CRITICAL(&s_mux);

            rx_prev = rx;
            pkts_prev = pk;
            written = 0;
            fill_min = ~0u;
            fill_max = 0;
            fb_sum = 0;
            fb_n = 0;
            peak[0] = peak[1] = 0;
        }
    }
}

}  // namespace

bool start(const Config& cfg) {
    if (s_started) return true;
    uac_usb_set_strings(cfg.product, cfg.serial);

    static StaticSemaphore_t init_storage;
    s_init_done = xSemaphoreCreateBinaryStatic(&init_storage);
    const size_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (xTaskCreatePinnedToCore(device_task, "usb_dev", 4096, nullptr, cfg.priority, nullptr,
                                cfg.core) != pdPASS) {
        ESP_LOGE(TAG, "usb_dev task");
        return false;
    }
    // The PHY and the stack come up on that task, because the controller's interrupt lands on
    // the core that brings it up. started() promises a device — it is what offers source 7 — so
    // the answer is waited for here, not assumed.
    if (xSemaphoreTake(s_init_done, INIT_WAIT) != pdTRUE || !s_init_ok) {
        ESP_LOGE(TAG, "USB PHY or TinyUSB did not start");
        return false;
    }
    // The producer does arithmetic and ring copies only — no flash, no NVS — so its stack can
    // live in PSRAM.
    if (xTaskCreatePinnedToCoreWithCaps(producer_task, "usb_pcm", 4096, nullptr,
                                        cfg.producer_priority, nullptr, cfg.core,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "usb_pcm task");
        return false;
    }
    s_started = true;
    s_core = cfg.core;
    ESP_LOGI(TAG, "started on core %d; internal heap %u -> %u", cfg.core, unsigned(before),
             unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
    return true;
}

bool started() { return s_started; }

void set_output(bool on) { s_output.store(on, std::memory_order_release); }

bool signal() { return s_signal.load(std::memory_order_acquire); }

Stats stats() {
    portENTER_CRITICAL(&s_mux);
    Stats s = s_last;
    portEXIT_CRITICAL(&s_mux);
    if (!s_started) s.state = State::off;
    return s;
}

const char* state_name(State s) {
    switch (s) {
        case State::off:     return "off";
        case State::waiting: return "waiting";
        case State::ready:   return "ready";
        case State::playing: return "playing";
    }
    return "?";
}

const char* phase_name(Phase p) {
    switch (p) {
        case Phase::idle:    return "idle";
        case Phase::flush:   return "flush";
        case Phase::ask:     return "ask";
        case Phase::run:     return "run";
        case Phase::refused: return "refused";
    }
    return "?";
}

size_t loss_log(char* out, size_t n, unsigned max_events) {
    if (!out || n == 0) return 0;
    out[0] = '\0';
    size_t len = 0;
    const uint32_t head = g_uac.ev_head;
    const unsigned count = unsigned(std::min<uint32_t>(head, std::min<unsigned>(max_events, UAC_EVENTS)));
    for (unsigned k = 0; k < count; k++) {
        const uint32_t i = (head - 1 - k) % UAC_EVENTS;
        const uint32_t t = g_uac.ev_t_ms[i];
        const int w = snprintf(out + len, n - len, "%st=%lu.%03lus %u (%lu us) @%08lx<%08lx %s",
                               k ? "; " : "", (unsigned long)(t / 1000), (unsigned long)(t % 1000),
                               unsigned(g_uac.ev_lost[i]), (unsigned long)g_uac.ev_gap_us[i],
                               (unsigned long)g_uac.ev_pc[i], (unsigned long)g_uac.ev_caller[i],
                               (const char*)g_uac.ev_task[i]);
        if (w < 0 || size_t(w) >= n - len) break;
        len += size_t(w);
    }
    return len;
}

}  // namespace usb_uac
