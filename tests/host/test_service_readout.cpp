/**
 * @file test_service_readout.cpp
 * @brief The half of the service screen that can be checked without a panel.
 *
 * The screen itself is LVGL geometry and only the bench can say whether it looks right. What
 * CAN be pinned here is everything the bench would be slowest to catch: a byte count that
 * rounds a 900-byte stack margin away, an uptime that loses the day after twenty-four hours,
 * a threshold on the wrong side of the number it was derived from, and a row that repaints
 * twenty-five times a second because "has it changed" was written the easy way.
 *
 * The thresholds are the 24-hour soak's pass/fail criteria (service_readout.h), so each one
 * is checked AT its boundary in both directions. A test that only proved 10 % is fine and
 * 90 % is not would pass with the line anywhere between them.
 */

#include "doctest.h"
#include "../../modules/ui_lvgl/include/service_readout.h"

#include <cstring>
#include <string>

namespace {

std::string bytes_of(int32_t b) {
    char buf[24];
    svc::bytes(b, buf, sizeof(buf));
    return buf;
}

std::string uptime_of(int32_t s) {
    char buf[24];
    svc::uptime(s, buf, sizeof(buf));
    return buf;
}

std::string gr_of(int32_t tenths) {
    char buf[24];
    svc::gain_reduction(tenths, buf, sizeof(buf));
    return buf;
}

} // namespace

// ═══════════════════════════════════════════════════════════════
// Formatting
// ═══════════════════════════════════════════════════════════════

TEST_CASE("the readouts in the panel's language: unit, decimal separator, reset reason") {
    // Ukrainian is the panel's default and the cases above; the rest follow ui.lang
    // (modules/ui_lvgl/include/i18n.h), with the separator the web's Intl.NumberFormat uses.
    struct Guard { ~Guard() { i18n::set_lang(i18n::Lang::UK); } } guard;
    i18n::set_lang(i18n::Lang::EN);
    CHECK(bytes_of(103 * 1024 + 512) == "103.5 KB");
    CHECK(bytes_of(400) == "400 B");
    CHECK(gr_of(15) == "−1.5 dB");
    CHECK(uptime_of(90061) == "1d 01:01");
    CHECK(std::string(svc::boot_reason("brownout")) == "Brownout");
    i18n::set_lang(i18n::Lang::DE);
    CHECK(bytes_of(31 * 1024 * 1024 + 524288) == "31,50 MB");
    CHECK(uptime_of(90061) == "1T 01:01");
    CHECK(std::string(svc::boot_reason("power_on")) == "Einschalten");
    i18n::set_lang(i18n::Lang::PL);
    CHECK(gr_of(60) == "−6,0 dB");
    CHECK(std::string(svc::boot_reason("unknown")) == "Nieznany");
}

TEST_CASE("byte counts keep the digits that matter at the scale they matter") {
    // Below 10 KB the exact count survives. This is the band `system.main_stack_min` lives
    // in, and the difference between 400 B and 900 B of stack margin is the whole question;
    // rounded to one decimal of a kibibyte both would read "0.4"/"0.9 КБ" and a reader would
    // have to do the multiplication in their head to know it was bytes.
    CHECK(bytes_of(0)    == "0 Б");
    CHECK(bytes_of(400)  == "400 Б");
    CHECK(bytes_of(900)  == "900 Б");
    CHECK(bytes_of(2048) == "2048 Б");

    // The boundary, both sides.
    CHECK(bytes_of(10 * 1024 - 1) == "10239 Б");
    CHECK(bytes_of(10 * 1024)     == "10,0 КБ");

    // Kibibytes with one decimal, truncated rather than rounded — an over-reported free heap
    // is the direction that hurts.
    CHECK(bytes_of(80 * 1024)      == "80,0 КБ");
    CHECK(bytes_of(103 * 1024)     == "103,0 КБ");
    CHECK(bytes_of(103 * 1024 + 512) == "103,5 КБ");
    CHECK(bytes_of(1024 * 1024 - 1)  == "1023,9 КБ");

    // Mebibytes with two, which is where PSRAM lives: 32 MB moving by 10 KB should still be
    // visible as movement.
    CHECK(bytes_of(1024 * 1024)            == "1,00 МБ");
    CHECK(bytes_of(31 * 1024 * 1024)       == "31,00 МБ");
    CHECK(bytes_of(31 * 1024 * 1024 + 524288) == "31,50 МБ");

    // A key that was never published reads as int32_t's default, and "—" is the honest
    // rendering of "no answer" — 0 would claim the heap is empty.
    CHECK(bytes_of(-1) == "—");
}

TEST_CASE("the hero figure is whole kibibytes, and never negative") {
    // The big free-RAM number is drawn in a digits-only face, so it carries no unit and no
    // sign — a "-1" arriving from an unpublished key would render as a stray glyph beside a
    // caption reading КБ.
    CHECK(svc::kib(0)            == 0);
    CHECK(svc::kib(1023)         == 0);
    CHECK(svc::kib(1024)         == 1);
    CHECK(svc::kib(103 * 1024)   == 103);
    CHECK(svc::kib(-1)           == 0);
}

TEST_CASE("byte counts never overrun the caller's buffer") {
    // The screen hands these 16 bytes. The longest output is a two-decimal mebibyte figure.
    char buf[16];
    svc::bytes(2047 * 1024 * 1024, buf, sizeof(buf));   // ~2 GB, well past this board
    CHECK(strlen(buf) < sizeof(buf));
    // And a buffer far too small must still terminate rather than write past the end.
    char tiny[4];
    svc::bytes(123456789, tiny, sizeof(tiny));
    CHECK(strlen(tiny) < sizeof(tiny));
}

TEST_CASE("uptime grows a day field exactly when there is a day") {
    CHECK(uptime_of(0)     == "00:00:00");
    CHECK(uptime_of(59)    == "00:00:59");
    CHECK(uptime_of(3600)  == "01:00:00");
    CHECK(uptime_of(3661)  == "01:01:01");
    CHECK(uptime_of(86399) == "23:59:59");
    // The moment the soak is about: one whole day. Seconds go away because at this scale
    // they are noise, and the minutes are what a log line is matched against.
    CHECK(uptime_of(86400) == "1д 00:00");
    CHECK(uptime_of(90061) == "1д 01:01");
    CHECK(uptime_of(9 * 86400 + 3600 * 23 + 60 * 59) == "9д 23:59");
    CHECK(uptime_of(-1) == "—");
}

TEST_CASE("the day count is clamped, so the field's width is a provable bound") {
    // int32_t seconds reach 24 855 days and the compiler has to assume a long conversion may
    // print eleven characters — which is how a sensible-looking buffer becomes a
    // -Werror=format-truncation failure. The clamp states the real range. 9999 days is 27
    // years: a device reporting it has a clock fault, not an uptime.
    CHECK(uptime_of(9999 * 86400) == "9999д 00:00");
    CHECK(uptime_of(20000 * 86400) == "9999д 00:00");
    CHECK(uptime_of(2147483647) == "9999д 03:14");   // int32_t's ceiling still formats
    char buf[16];
    svc::uptime(2147483647, buf, sizeof(buf));
    CHECK(strlen(buf) < sizeof(buf));
}

TEST_CASE("a boot reason is translated, or handed back as itself") {
    CHECK(std::string(svc::boot_reason("power_on")) == "Живлення");
    CHECK(std::string(svc::boot_reason("task_wdt")) == "WDT задачі");
    CHECK(std::string(svc::boot_reason("brownout")) == "Просідання");
    CHECK(std::string(svc::boot_reason("unknown"))  == "Невідомо");
    CHECK(std::string(svc::boot_reason(""))         == "—");
    CHECK(std::string(svc::boot_reason(nullptr))    == "—");
    // A token this build has never heard of — a newer ESP-IDF reset reason — must show up as
    // itself. Falling back to "невідомо" would hide the one detail worth having.
    CHECK(std::string(svc::boot_reason("usb_uart")) == "usb_uart");
}

TEST_CASE("limiter gain reduction is printed with the sign the rest of the UI uses") {
    // audio.lim_gr is published as a positive magnitude; the audio log and the EQ screen both
    // negate it when they print. Zero is printed unsigned — "−0.0 dB" reads as a fault.
    CHECK(gr_of(0)  == "0,0 дБ");
    CHECK(gr_of(15) == "−1,5 дБ");
    CHECK(gr_of(60) == "−6,0 дБ");
    CHECK(gr_of(-5) == "0,0 дБ");   // a limiter cannot add gain

    // Clamped at 99.9 dB. The figure is −20·log10(gain), so a gain that has collapsed to zero
    // sends it to infinity — and a limiter taking 60 dB is already a fault, so everything
    // past the clamp is the same fault with more digits. It is also what makes the field's
    // width provable, which -Werror=format-truncation insists on.
    CHECK(gr_of(999)     == "−99,9 дБ");
    CHECK(gr_of(5000)    == "−99,9 дБ");
    CHECK(gr_of(2000000) == "−99,9 дБ");
    char buf[16];
    svc::gain_reduction(2147483647, buf, sizeof(buf));
    CHECK(strlen(buf) < sizeof(buf));
}

// ═══════════════════════════════════════════════════════════════
// Thresholds — checked at the boundary, in both directions
// ═══════════════════════════════════════════════════════════════

TEST_CASE("audio CPU load grades where the header says it does") {
    CHECK(svc::cpu(0)   == svc::Grade::Ok);
    CHECK(svc::cpu(59)  == svc::Grade::Ok);
    CHECK(svc::cpu(60)  == svc::Grade::Warn);
    CHECK(svc::cpu(84)  == svc::Grade::Warn);
    CHECK(svc::cpu(85)  == svc::Grade::Bad);
    CHECK(svc::cpu(100) == svc::Grade::Bad);
}

TEST_CASE("the main loop period grades against its own 10 ms nominal") {
    // MODESP_MAIN_LOOP_HZ = 100, so a healthy board reports 10 and one tick of jitter at
    // FREERTOS_HZ = 1000 is 1 ms. Anything inside 12 is the loop doing its job.
    CHECK(svc::loop_ms(10) == svc::Grade::Ok);
    CHECK(svc::loop_ms(12) == svc::Grade::Ok);
    CHECK(svc::loop_ms(13) == svc::Grade::Warn);
    CHECK(svc::loop_ms(29) == svc::Grade::Warn);
    CHECK(svc::loop_ms(30) == svc::Grade::Bad);
}

TEST_CASE("the main stack margin grades in bytes, not words") {
    // uxTaskGetStackHighWaterMark returns BYTES on ESP-IDF. A test that assumed words would
    // pass with every threshold four times too generous, which is the failure mode worth
    // pinning: 1024 here has to mean a kilobyte.
    CHECK(svc::stack_free(4096) == svc::Grade::Ok);
    CHECK(svc::stack_free(2048) == svc::Grade::Ok);
    CHECK(svc::stack_free(2047) == svc::Grade::Warn);
    CHECK(svc::stack_free(1024) == svc::Grade::Warn);
    CHECK(svc::stack_free(1023) == svc::Grade::Bad);
    CHECK(svc::stack_free(0)    == svc::Grade::Bad);
}

TEST_CASE("free DMA-internal grades against the 80 KB target") {
    // 80 KB is the target the memory work was held to. It began
    // at 28 275 B, which is what the failure looked like, and that number must land in Bad.
    CHECK(svc::dma_free(80 * 1024)     == svc::Grade::Ok);
    CHECK(svc::dma_free(80 * 1024 - 1) == svc::Grade::Warn);
    CHECK(svc::dma_free(40 * 1024)     == svc::Grade::Warn);
    CHECK(svc::dma_free(40 * 1024 - 1) == svc::Grade::Bad);
    CHECK(svc::dma_free(28275)         == svc::Grade::Bad);
}

TEST_CASE("free internal SRAM grades a step above the DMA line") {
    // DMA-capable internal is a subset of internal, so internal must not still read green
    // where DMA has already gone amber — that would be the same fault one allocation earlier,
    // reported as healthy.
    CHECK(svc::internal_free(96 * 1024)     == svc::Grade::Ok);
    CHECK(svc::internal_free(96 * 1024 - 1) == svc::Grade::Warn);
    CHECK(svc::internal_free(48 * 1024)     == svc::Grade::Warn);
    CHECK(svc::internal_free(48 * 1024 - 1) == svc::Grade::Bad);
    // The ordering, stated as a property rather than as two numbers that happen to agree.
    for (int32_t b : {0, 32 * 1024, 64 * 1024, 96 * 1024, 512 * 1024})
        CHECK(svc::internal_free(b) >= svc::dma_free(b));
}

TEST_CASE("satellite round-trip time grades against a link that carries audio") {
    CHECK(svc::rtt_ms(0)   == svc::Grade::Ok);
    CHECK(svc::rtt_ms(50)  == svc::Grade::Ok);
    CHECK(svc::rtt_ms(51)  == svc::Grade::Warn);
    CHECK(svc::rtt_ms(200) == svc::Grade::Warn);
    CHECK(svc::rtt_ms(201) == svc::Grade::Bad);
}

TEST_CASE("counters separate the ones that fail a run from the ones that cost a frame") {
    // audio.underruns: one is a result.
    CHECK(svc::counter(0) == svc::Grade::Ok);
    CHECK(svc::counter(1) == svc::Grade::Bad);
    // The link's counters: a phone walking out of range produces these legitimately.
    CHECK(svc::soft_counter(0) == svc::Grade::Ok);
    CHECK(svc::soft_counter(1) == svc::Grade::Warn);
    CHECK(svc::soft_counter(9999) == svc::Grade::Warn);
}

TEST_CASE("a raised flag and a bad boot reason are results, an ordinary boot is not") {
    CHECK(svc::flag(false) == svc::Grade::Ok);
    CHECK(svc::flag(true)  == svc::Grade::Bad);

    for (const char* good : {"power_on", "software", "deep_sleep", "unknown", "", "usb_uart"})
        CHECK(svc::boot_grade(good) == svc::Grade::Ok);
    for (const char* bad : {"panic", "brownout", "wdt", "int_wdt", "task_wdt"})
        CHECK(svc::boot_grade(bad) == svc::Grade::Bad);
    CHECK(svc::boot_grade(nullptr) == svc::Grade::Ok);
}

// ═══════════════════════════════════════════════════════════════
// Change detection — the reason this screen is affordable
// ═══════════════════════════════════════════════════════════════

TEST_CASE("a field asks to be drawn once, then only when something moved") {
    svc::Field f;
    CHECK(f.changed(42, svc::Grade::Ok));        // first pass: nothing is on screen yet
    CHECK_FALSE(f.changed(42, svc::Grade::Ok));  // and then silence, for as long as it holds
    for (int i = 0; i < 100; i++) CHECK_FALSE(f.changed(42, svc::Grade::Ok));
    CHECK(f.changed(43, svc::Grade::Ok));
    CHECK_FALSE(f.changed(43, svc::Grade::Ok));
}

TEST_CASE("a field redraws when only the verdict moved") {
    // The case that a value-only comparison gets wrong. Two numbers can print the same and
    // mean different things — a heap figure that has not moved while the THRESHOLD it is
    // compared against was crossed by something else, or, as here, a value whose text is
    // unchanged after a reset. Leaving the colour behind would be wrong in the one direction
    // that matters: a red number quietly staying red after the fault cleared.
    svc::Field f;
    CHECK(f.changed(70, svc::Grade::Warn));
    CHECK_FALSE(f.changed(70, svc::Grade::Warn));
    CHECK(f.changed(70, svc::Grade::Bad));
    CHECK(f.grade() == svc::Grade::Bad);
    CHECK(f.changed(70, svc::Grade::Ok));
    CHECK(f.grade() == svc::Grade::Ok);
}

TEST_CASE("zero is a value, not 'never drawn'") {
    // A sentinel of 0 would skip the first paint of every counter on the screen — which is
    // every counter, since they all start at zero and most stay there. The row would sit
    // showing its placeholder for the whole run.
    svc::Field f;
    CHECK(f.changed(0, svc::Grade::Ok));
    CHECK_FALSE(f.changed(0, svc::Grade::Ok));
}

TEST_CASE("a rebuilt screen repaints every field") {
    // A palette change does lv_obj_clean() + rebuild (ui_lvgl_module.cpp, apply_theme), so
    // every widget is new and empty while the values behind them have not changed. Without
    // the reset the rows would come back blank and stay blank until something moved.
    svc::Field f;
    CHECK(f.changed(7, svc::Grade::Ok));
    CHECK_FALSE(f.changed(7, svc::Grade::Ok));
    f.reset();
    CHECK(f.changed(7, svc::Grade::Ok));
}

TEST_CASE("a text field compares the text it actually drew") {
    svc::TextField<16> t;
    CHECK(t.changed(""));                 // first pass
    CHECK_FALSE(t.changed(""));
    CHECK(t.changed("audio", svc::Grade::Bad));
    CHECK(t.grade() == svc::Grade::Bad);
    CHECK_FALSE(t.changed("audio", svc::Grade::Bad));
    CHECK(t.changed("", svc::Grade::Ok));  // the stall cleared
    CHECK_FALSE(t.changed(nullptr));       // nullptr and "" are the same absence

    // A name longer than the field compares on what was stored, not on the caller's string,
    // so two names sharing a prefix past the buffer must not be called equal by accident —
    // and must not read off the end of the copy either.
    svc::TextField<8> s;
    CHECK(s.changed("abcdefghij"));
    CHECK_FALSE(s.changed("abcdefghij"));
    CHECK_FALSE(s.changed("abcdefgXX"));   // identical within the seven characters kept
    CHECK(s.changed("abcdeZZ"));
}
