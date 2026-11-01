/**
 * @file service_readout.h
 * @brief What the service screen SAYS about a number — formatting, verdicts, and the test
 *        for whether a row needs repainting at all. No LVGL, no ESP-IDF, no globals.
 *
 * Split out of screen_svc.cpp for two reasons.
 *
 * The first is that this half can be tested on a host and the layout half cannot. A wrong
 * threshold or a byte count that reads "0.0 КБ" at 900 bytes is a silent lie on a screen
 * nobody is checking against a second source at four in the morning, which is exactly when
 * a 24-hour soak is running. tests/host/test_service_readout.cpp pins all of it.
 *
 * The second is that the thresholds below ARE the soak's pass/fail criteria, and they came
 * from this repository's own measurements rather than from taste. Each one carries the
 * number it came from. When a criterion moves, it moves here, once.
 *
 * Nothing here allocates and nothing here holds a buffer: every formatter writes into
 * storage the caller owns, because internal RAM is the scarce resource on this board and a
 * screen's static scratch space is the easiest place to spend it by accident.
 */

#pragma once

#include "i18n.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace svc {

// ═══════════════════════════════════════════════════════════════
// Verdicts
// ═══════════════════════════════════════════════════════════════

/// How worried to be about one number. The screen turns this into a colour; keeping the
/// judgement separate from the palette is what lets it be tested, and what stops a
/// threshold being written twice in two files with two different numbers.
enum class Grade : uint8_t {
    Ok   = 0,   ///< within the budget this project measured
    Warn = 1,   ///< inside the margin — worth watching, not yet a failure
    Bad  = 2,   ///< past the line; on a soak this is the result
};

// ═══════════════════════════════════════════════════════════════
// Formatting
// ═══════════════════════════════════════════════════════════════

/// A byte count, at the scale a person reads it from a metre away.
///
/// Integer arithmetic throughout: the fractions are one and two decimals of a division by
/// 1024, which is exact in integers and costs nothing, where %f drags in the whole float
/// formatter for a digit that was never in doubt.
///
/// Below 10 KB the exact byte count is printed instead of a rounded "9.8 КБ", because the
/// one number that lives down there is `system.main_stack_min` — a stack margin of 1200 B
/// and one of 1900 B are different situations and both would round to "1.2"/"1.9 КБ"
/// anyway, but 400 B and 900 B would not survive the rounding at all.
///
/// The web page (tools/system_pages.json) prints these keys with unit "B". This is the same
/// quantity at a readable scale, not a different one.
inline void bytes(int32_t b, char* out, size_t n) {
    if (!out || n == 0) return;
    if (b < 0) { snprintf(out, n, "—"); return; }
    using i18n::S;
    if (b < 10 * 1024) { snprintf(out, n, "%ld %s", (long)b, i18n::tr(S::unit_b)); return; }
    if (b < 1024 * 1024) {
        const long kb = b / 1024;
        const long f  = ((long)(b % 1024) * 10) / 1024;
        snprintf(out, n, "%ld%c%ld %s", kb, i18n::decimal_sep(), f, i18n::tr(S::unit_kb));
        return;
    }
    const long mb = b / (1024 * 1024);
    const long f  = ((long)(b % (1024 * 1024)) * 100) / (1024 * 1024);
    snprintf(out, n, "%ld%c%02ld %s", mb, i18n::decimal_sep(), f, i18n::tr(S::unit_mb));
}

/// Whole kibibytes, for the one place a bare number is wanted: the hero figures are drawn in
/// a digits-only face (UiTheme::f_num), so their unit is a separate label and the number
/// itself must contain no letters and no separator.
inline int32_t kib(int32_t b) { return b < 0 ? 0 : b / 1024; }

/// Uptime. Days appear only once there is one, so a fresh boot reads as a clock and a soak
/// run reads as "1д 04:12" — which is the question actually being asked of this field after
/// a night: did it stay up, and if not, how long ago did it come back.
inline void uptime(int32_t sec, char* out, size_t n) {
    if (!out || n == 0) return;
    if (sec < 0) { snprintf(out, n, "—"); return; }
    // The day count is clamped, and the clamp is not decoration. int32_t seconds reach 24 855
    // days, and the compiler has to assume a `long` conversion can print eleven characters —
    // which is how a caller's sensible-looking buffer becomes a truncation error under
    // -Werror=format-truncation. Clamping states the real range so the bound is provable, and
    // 9999 days is 27 years: a device that reports it has a clock fault, not an uptime.
    long d = sec / 86400;
    if (d > 9999) d = 9999;
    const long h = (sec % 86400) / 3600;
    const long m = (sec % 3600) / 60;
    const long s = sec % 60;
    if (d > 0) snprintf(out, n, "%ld%s %02ld:%02ld", d, i18n::tr(i18n::S::unit_day), h, m);
    else       snprintf(out, n, "%02ld:%02ld:%02ld", h, m, s);
}

/// `system.boot_reason` as words. The tokens are the ones SystemMonitor::on_init publishes
/// (components/modesp_services/src/system_monitor.cpp); an unrecognised one is handed back
/// unchanged, so a token a newer ESP-IDF adds shows up as itself rather than as "невідомо",
/// which would be a lie about a reset nobody has seen before.
inline const char* boot_reason(const char* token) {
    if (!token || !token[0]) return "—";
    using i18n::S;
    using i18n::tr;
    if (!strcmp(token, "power_on"))   return tr(S::svc_boot_power_on);
    if (!strcmp(token, "software"))   return tr(S::svc_boot_software);
    if (!strcmp(token, "panic"))      return tr(S::svc_boot_panic);
    if (!strcmp(token, "int_wdt"))    return tr(S::svc_boot_int_wdt);
    if (!strcmp(token, "task_wdt"))   return tr(S::svc_boot_task_wdt);
    if (!strcmp(token, "wdt"))        return "WDT";
    if (!strcmp(token, "brownout"))   return tr(S::svc_boot_brownout);
    if (!strcmp(token, "deep_sleep")) return tr(S::svc_boot_deep_sleep);
    if (!strcmp(token, "unknown"))    return tr(S::svc_boot_unknown);
    return token;
}

/// Limiter gain reduction, in the sign the rest of the UI uses: `audio.lim_gr` is published
/// as a positive magnitude and both the audio log and the EQ screen print it negated
/// (audio_module.cpp:438, screen_eq.cpp:566). A service screen that showed "+1.5 dB" where
/// the EQ screen shows "−1.5" would be two answers to one question.
///
/// Takes tenths of a dB so the caller can compare an integer for change detection rather
/// than a float, whose last bit moves on its own.
inline void gain_reduction(int32_t tenths, char* out, size_t n) {
    if (!out || n == 0) return;
    if (tenths < 0) tenths = 0;               // a limiter cannot add gain
    // Clamped at 99.9 dB, for a real reason and a compiler one. The real one: the figure is
    // −20·log10(gain) and a gain that has collapsed to zero sends it to infinity, which is
    // not a reading — a limiter taking 60 dB is already a fault, and anything past that is
    // the same fault with more digits. The compiler one: without a stated range a `long`
    // conversion may print eleven characters, and -Werror=format-truncation is right to
    // refuse a buffer sized for the number this can actually be.
    if (tenths > 999) tenths = 999;
    const long whole = tenths / 10, frac = tenths % 10;
    const char sep = i18n::decimal_sep();
    const char* db = i18n::tr(i18n::S::unit_db);
    if (tenths == 0) snprintf(out, n, "0%c0 %s", sep, db);
    else             snprintf(out, n, "−%ld%c%ld %s", whole, sep, frac, db);
}

// ═══════════════════════════════════════════════════════════════
// Thresholds — the soak's pass/fail criteria, with their provenance
// ═══════════════════════════════════════════════════════════════

/// Audio task load, `audio.cpu_pct`: DSP time over real time for the task pinned to core 1.
/// It is not a budget anyone has measured a failure against, so the lines are the ordinary
/// ones for a hard-real-time task: comfortable below 60 %, no headroom for a codec change
/// or a resampler above 85 %.
inline Grade cpu(int32_t pct) {
    if (pct >= 85) return Grade::Bad;
    if (pct >= 60) return Grade::Warn;
    return Grade::Ok;
}

/// `system.main_loop_max_ms`: the longest turn of the 100 Hz main loop since the last
/// SystemMonitor report, so 10 ms is the nominal value and anything above it is something
/// above `main` not giving the CPU back (MODESP_MAIN_LOOP_HZ = 100, modesp/app.h:22).
///
/// This is the field the FREERTOS_HZ=1000 decision is graded on: at 1000 Hz a tick is 1 ms,
/// so the loop can hold its period exactly and a climb here is real starvation rather than
/// tick quantisation. 12 ms allows one tick of jitter plus rounding; 30 ms means two thirds
/// of the loop's turns were missed, which is where the encoder starts to feel dead.
inline Grade loop_ms(int32_t ms) {
    if (ms >= 30) return Grade::Bad;
    if (ms >= 13) return Grade::Warn;
    return Grade::Ok;
}

/// `system.main_stack_min`, in BYTES on ESP-IDF (uxTaskGetStackHighWaterMark; the kernel
/// documents the unit against plain FreeRTOS, and SystemMonitor publishes it as it comes).
/// 4 KB of it used to disappear into a WebSocket broadcast, so the margin below which the
/// next thing put on this stack overflows it is not hypothetical.
inline Grade stack_free(int32_t b) {
    if (b < 1024) return Grade::Bad;
    if (b < 2048) return Grade::Warn;
    return Grade::Ok;
}

/// `system.heap_dma` — free DMA-capable internal SRAM, the resource this board actually runs
/// out of (docs/CONCEPT.md, "Board constraints"). 80 KB is the target the memory work was
/// held to; it started from 28 275 B, which is what failure looked like, so half the target
/// is the line below which the same failure is in reach.
inline Grade dma_free(int32_t b) {
    if (b < 40 * 1024) return Grade::Bad;
    if (b < 80 * 1024) return Grade::Warn;
    return Grade::Ok;
}

/// `system.heap_internal` — all free internal 8-bit SRAM, of which the DMA-capable part is a
/// subset. Held a step above the DMA line for that reason: internal running out first is the
/// same fault one allocation earlier.
inline Grade internal_free(int32_t b) {
    if (b < 48 * 1024) return Grade::Bad;
    if (b < 96 * 1024) return Grade::Warn;
    return Grade::Ok;
}

/// `bt.rtt_ms` — PING/PONG round trip over the UART link to the satellite. The link carries
/// audio, so a round trip is normally single-digit milliseconds; 200 ms is the point at
/// which the transport buttons stop feeling connected to anything.
inline Grade rtt_ms(int32_t ms) {
    if (ms > 200) return Grade::Bad;
    if (ms > 50)  return Grade::Warn;
    return Grade::Ok;
}

/// A counter that must stay at zero for the run to pass: `audio.underruns`. One is a result.
inline Grade counter(int32_t n) { return n > 0 ? Grade::Bad : Grade::Ok; }

/// A counter that ticking does not by itself fail the run — the satellite link's
/// `bt.uart_ovf`, `bt.rx_drops`, `bt.seq_gaps`. They cost frames, not the session, and a
/// phone walking out of range produces them legitimately. Worth a colour, not a verdict.
inline Grade soft_counter(int32_t n) { return n > 0 ? Grade::Warn : Grade::Ok; }

/// A flag that should not be raised: `system.safe_mode`, a non-empty `system.stalled`.
inline Grade flag(bool raised) { return raised ? Grade::Bad : Grade::Ok; }

/// How the device came up. A watchdog, a panic or a brownout means the previous run ended
/// badly, which is the whole question a soak asks; an ordinary power-on or a commanded
/// restart is not news.
inline Grade boot_grade(const char* token) {
    if (!token || !token[0]) return Grade::Ok;
    if (!strcmp(token, "panic") || !strcmp(token, "brownout")) return Grade::Bad;
    if (!strcmp(token, "int_wdt") || !strcmp(token, "task_wdt") || !strcmp(token, "wdt"))
        return Grade::Bad;
    return Grade::Ok;
}

// ═══════════════════════════════════════════════════════════════
// Change detection
// ═══════════════════════════════════════════════════════════════

/// One value on screen, and what was last drawn for it.
///
/// The screen refreshes at 25 Hz; almost everything it shows moves at 0.2 Hz, because
/// SystemMonitor reports every 5 s (system_monitor.h:39). Formatting and setting twenty
/// labels twenty-five times a second would be paid for in the very CPU figure this screen
/// exists to display — the same argument that turned the spectrum row from 28 objects into
/// one. So a row is touched only when something about it actually changed.
///
/// The verdict is part of "changed", not a consequence of it: 84 % and 85 % differ by one
/// character but by a whole colour, and a screen that repainted the text and left the colour
/// behind would be wrong in the one direction that matters.
struct Field {
    /// True when this value needs drawing. False means the label on screen is already right.
    bool changed(int32_t v, Grade g = Grade::Ok) {
        if (painted_ && v == value_ && g == grade_) return false;
        painted_ = true;
        value_   = v;
        grade_   = g;
        return true;
    }
    /// The verdict currently on screen. Only meaningful after changed() has returned true.
    Grade grade() const { return grade_; }
    /// Forget what was drawn, so the next changed() repaints whatever the value is. Needed
    /// when something outside the value invalidates the row — a palette change rebuilds
    /// every widget, and a row that "has not changed" would come back blank.
    void reset() { painted_ = false; }

private:
    int32_t value_   = 0;
    Grade   grade_   = Grade::Ok;
    bool    painted_ = false;
};

/// The same, for a value that is text: a boot reason, a stalled task's name. Compares the
/// string rather than a number, and owns the copy it compares against.
template <size_t N>
struct TextField {
    bool changed(const char* s, Grade g = Grade::Ok) {
        if (!s) s = "";
        if (painted_ && g == grade_ && strncmp(seen_, s, N - 1) == 0) return false;
        painted_ = true;
        grade_   = g;
        strncpy(seen_, s, N - 1);
        seen_[N - 1] = '\0';
        return true;
    }
    Grade grade() const { return grade_; }
    void reset() { painted_ = false; seen_[0] = '\0'; }

private:
    char  seen_[N] = "";
    Grade grade_   = Grade::Ok;
    bool  painted_ = false;
};

} // namespace svc
