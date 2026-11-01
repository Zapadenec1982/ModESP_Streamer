/**
 * @file screen_svc.cpp
 * @brief "СЕРВІС" — the numbers a 24-hour soak is judged on, readable without a laptop.
 *
 * It exists because the panel had no CPU indicator at all. `audio.cpu_pct` was mirrored into
 * the snapshot on every tick and no screen ever read it; the same was true of the loop
 * period, the stack margin, every heap figure and the satellite link's counters. All of them
 * were rendered only by the generated Web pages, because those are built from the manifests
 * and these screens are hand-written and know nothing about them. Someone standing at the
 * device could not answer "is it healthy" without opening a browser.
 *
 * Layout, top to bottom:
 *
 *   Three hero figures, in the big digits-only face — audio CPU, the longest main-loop turn,
 *   and free internal SRAM. These are the three that decide a soak, and they are the three a
 *   person should be able to read from across the room without hunting.
 *
 *   Three columns of detail underneath: what the audio core and the scheduler are doing, the
 *   whole heap picture, and the state of the link to the Bluetooth satellite.
 *
 * Every number is graded, and the grade is the colour: normal, accent for "watch this", red
 * for "this is the result". The thresholds and the reasoning behind each one live in
 * service_readout.h, next to the measurements they came from, and are tested on the host —
 * a threshold is a claim about the hardware and belongs somewhere it can be checked.
 *
 * COST. This screen displays a CPU figure, so it must not be visible in it. Everything it
 * shows is republished at 0.2 Hz (SystemMonitor's 5 s report) or 1 Hz (uptime), while the
 * LVGL refresh runs at 25 Hz — so a row is formatted and set only when its value or its
 * verdict changed (svc::Field), and in the steady state that is a handful of small label
 * invalidations per second instead of twenty-two per tick. The same argument that turned the
 * spectrum row from 28 objects into one; here it costs 8 bytes of change-detection state per
 * row rather than the 40-byte text cache the Settings screen keeps.
 *
 * Reached from the pop-out menu like every other screen, and it is not modal: the hamburger,
 * the left-edge strip and a long press on the encoder all still work, so there is no way to
 * arrive here and be stuck.
 */

#include "ui_common.h"
#include "service_readout.h"
#include "i18n.h"
#include <cstdio>
#include <cstring>

namespace {

using i18n::S;
using i18n::SU;
using i18n::tr;

// ── geometry, all relative to the 752x384 content column ──
constexpr int32_t HERO_W  = 240;
constexpr int32_t HERO_H  = 92;
constexpr int32_t HERO_GAP = (uig::CONT_W - 3 * HERO_W) / 2;    // 16
constexpr int32_t GRID_Y  = HERO_H + 20;                        // 112
constexpr int32_t COL_W   = HERO_W;                             // the columns line up with
constexpr int32_t COL_PITCH = HERO_W + HERO_GAP;                // the hero cards above them
constexpr int32_t CAP_H   = 22;
constexpr int32_t ROW_H   = 28;
constexpr int32_t VAL_W   = 140;    ///< right-aligned value field; the label gets the rest
constexpr int32_t KEY_GAP = 8;      ///< ... or, a label longer than that, the field starts after it
constexpr int32_t GROUP_GAP = 16;

enum HeroId { H_CPU, H_LOOP, H_RAM, H_COUNT };

enum RowId {
    // column 1 — the audio core and the scheduler
    R_UNDER, R_LIM, R_STACK, R_STALL,
    // column 1, second group — the run itself
    R_UPTIME, R_BOOT, R_SAFE,
    // column 2 — memory
    R_INT, R_DMA, R_BIG, R_PSRAM, R_HFREE, R_INTMIN,
    // column 3 — the satellite link
    R_LINK, R_PEER, R_RTT, R_OVF, R_DROPS, R_GAPS,
    R_COUNT
};

struct Hero {
    lv_obj_t* num  = nullptr;
    lv_obj_t* unit = nullptr;
};

struct Svc {
    Hero      hero[H_COUNT];
    lv_obj_t* value[R_COUNT] = {};
    // Change detection. Eight bytes a row, against the forty a text cache would take: the
    // comparison is on the NUMBER, before it has been turned into characters, which also
    // saves the snprintf on every unchanged tick rather than only the label invalidation.
    svc::Field     hf[H_COUNT];
    svc::Field     f[R_COUNT];
    // Two rows are text rather than a number, so they compare what they last drew.
    svc::TextField<12> f_boot;
    svc::TextField<16> f_stall;
};

Svc g;

/// The verdict as a colour. `ok` differs between the hero figures and the detail rows —
/// a hero reads at full contrast when all is well, a detail row sits back — so it is the
/// caller's to choose; Warn and Bad are the same everywhere, which is the point of them.
uint32_t verdict(const UiTheme& t, svc::Grade v, uint32_t ok) {
    switch (v) {
    case svc::Grade::Bad:  return t.red;
    case svc::Grade::Warn: return t.acc;
    default:               return ok;
    }
}

// ── build ────────────────────────────────────────────────────────────────────────────────

/// One hero: a card with a small caption, a big number and its unit beside it.
///
/// The number is drawn in UiTheme::f_num, which carries digits and a handful of symbols and
/// no Cyrillic at all (tools/fonts/build_fonts.py, DIGITS) — so "мс" and "КБ" have to be a
/// second label in a text face, and "%" gets one too rather than have one card built
/// differently from the other two.
void make_hero(lv_obj_t* parent, HeroId id, int32_t x, const char* cap, const char* unit) {
    const UiTheme& t = ui_theme();
    lv_obj_t* c = uih::card(parent, x, 0, HERO_W, HERO_H);
    lv_obj_t* l = uih::label(c, t.f_cap, t.faint, cap, 5);
    lv_obj_set_pos(l, 16, 12);
    // "0" and not "—": f_num carries digits and a few symbols, and the em dash is not one of
    // them, so the placeholder would be a missing glyph. It is on screen for one frame — the
    // first refresh always paints, because a Field has drawn nothing yet.
    g.hero[id].num = uih::label(c, t.f_num, t.text, "0", 0);
    lv_obj_set_pos(g.hero[id].num, 16, 34);
    g.hero[id].unit = uih::label(c, t.f_body, t.dim, unit, 0);
    lv_obj_align_to(g.hero[id].unit, g.hero[id].num, LV_ALIGN_OUT_RIGHT_BOTTOM, 8, -8);
}

/// One detail row: hairline on top, label left, value right — the Settings screen's row,
/// at the tighter pitch this screen needs to fit nineteen of them.
///
/// The labels were sized in Ukrainian; German and Polish run past the 100 px left of the value
/// ("Największy blok"). The field then starts after the label instead of under it, and a value
/// too long for what is left is cut with "…" where the overflow check sees it.
void make_row(lv_obj_t* parent, RowId id, int32_t x, int32_t y, const char* label) {
    const UiTheme& t = ui_theme();
    uih::rect(parent, x, y, COL_W, 1, t.hair);
    lv_obj_t* l = uih::label(parent, t.f_body, t.faint, label, 0);
    lv_obj_set_pos(l, x, y + 7);
    const int32_t key_w = lv_text_get_width(label, (uint32_t)strlen(label), t.f_body, 0);
    const int32_t val_w = LV_MIN(VAL_W, COL_W - key_w - KEY_GAP);
    lv_obj_t* v = uih::label(parent, t.f_body, t.dim, "—", 0);
    lv_obj_set_width(v, val_w);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(v, x + COL_W - val_w, y + 7);
    g.value[id] = v;
}

int32_t group(lv_obj_t* parent, int32_t x, int32_t y, const char* title) {
    const UiTheme& t = ui_theme();
    lv_obj_t* l = uih::label(parent, t.f_cap, t.faint, title, 5);
    lv_obj_set_pos(l, x, y + 4);
    return y + CAP_H;
}

lv_obj_t* build(lv_obj_t* parent, UiCtx&) {
    g = Svc{};
    lv_obj_t* root = uih::box(parent, 0, 0, uig::CONT_W, uig::CONT_H);

    // "ПІК ЦИКЛУ" and not "ЦИКЛ": `system.main_loop_max_ms` is the WORST turn since the last
    // SystemMonitor report, and the counter is cleared after each one, so it is a peak over a
    // 5 s window and not the period the loop is currently running at. The Web page says the
    // same thing in more words ("Найдовший цикл main", tools/system_pages.json).
    make_hero(root, H_CPU,  0,             tr(SU::svc_cpu),  "%");
    make_hero(root, H_LOOP, COL_PITCH,     tr(SU::svc_loop), tr(S::unit_ms));
    make_hero(root, H_RAM,  2 * COL_PITCH, tr(SU::svc_ram),  tr(S::unit_kb));

    // ── column 1: what the audio core and the scheduler are doing, then the run itself ──
    int32_t y = group(root, 0, GRID_Y, tr(SU::svc_audio));
    make_row(root, R_UNDER, 0, y, tr(S::svc_underruns)); y += ROW_H;
    make_row(root, R_LIM,   0, y, tr(S::svc_limiter));   y += ROW_H;
    make_row(root, R_STACK, 0, y, tr(S::svc_stack));     y += ROW_H;
    make_row(root, R_STALL, 0, y, tr(S::svc_stalled));   y += ROW_H;

    y = group(root, 0, y + GROUP_GAP, tr(SU::svc_run));
    make_row(root, R_UPTIME, 0, y, tr(S::svc_uptime));     y += ROW_H;
    make_row(root, R_BOOT,   0, y, tr(S::svc_boot));       y += ROW_H;
    make_row(root, R_SAFE,   0, y, tr(S::svc_safe_short)); y += ROW_H;

    // ── column 2: the whole heap picture ──
    const int32_t x2 = COL_PITCH;
    int32_t y2 = group(root, x2, GRID_Y, tr(SU::svc_memory));
    make_row(root, R_INT,   x2, y2, tr(S::svc_internal)); y2 += ROW_H;
    make_row(root, R_DMA,   x2, y2, "DMA");        y2 += ROW_H;
    make_row(root, R_BIG,   x2, y2, tr(S::svc_largest)); y2 += ROW_H;
    make_row(root, R_PSRAM, x2, y2, "PSRAM");      y2 += ROW_H;
    make_row(root, R_HFREE, x2, y2, tr(S::svc_free)); y2 += ROW_H;
    make_row(root, R_INTMIN, x2, y2, tr(S::svc_internal_min)); y2 += ROW_H;

    // ── column 3: the link to the Bluetooth satellite ──
    const int32_t x3 = 2 * COL_PITCH;
    int32_t y3 = group(root, x3, GRID_Y, tr(SU::svc_sat));
    make_row(root, R_LINK,  x3, y3, tr(S::svc_link)); y3 += ROW_H;
    make_row(root, R_PEER,  x3, y3, tr(S::svc_peer)); y3 += ROW_H;
    make_row(root, R_RTT,   x3, y3, "RTT");       y3 += ROW_H;
    make_row(root, R_OVF,   x3, y3, tr(S::svc_uart_ovf));  y3 += ROW_H;
    make_row(root, R_DROPS, x3, y3, tr(S::svc_drops)); y3 += ROW_H;
    make_row(root, R_GAPS,  x3, y3, tr(S::svc_gaps));  y3 += ROW_H;

    return root;
}

// ── refresh ──────────────────────────────────────────────────────────────────────────────

/// Set a hero figure, and only when it moved. The unit is re-anchored with it because the
/// number's width changes with its digits; doing it here rather than on every tick is why
/// that costs nothing.
void hero(const UiTheme& t, HeroId id, int32_t v, svc::Grade grade) {
    if (!g.hf[id].changed(v, grade)) return;
    char buf[16];
    snprintf(buf, sizeof(buf), "%ld", (long)v);
    lv_label_set_text(g.hero[id].num, buf);
    const uint32_t col = verdict(t, grade, t.text);
    lv_obj_set_style_text_color(g.hero[id].num, ui_col(col), 0);
    lv_obj_set_style_text_color(g.hero[id].unit, ui_col(col == t.text ? t.dim : col), 0);
    lv_obj_align_to(g.hero[id].unit, g.hero[id].num, LV_ALIGN_OUT_RIGHT_BOTTOM, 8, -8);
}

/// Write a detail value and dress it in its verdict.
///
/// Two channels, not one. In the Amber palette the accent is #FFB03A and red is #FF5B4A —
/// close enough in hue that at arm's length, on a dimmed panel, or to a colour-blind reader
/// they are the same warm orange. So Bad is also the semibold face: a second cue that costs
/// one style write on a change that was going to repaint the label anyway.
void paint(const UiTheme& t, lv_obj_t* lbl, svc::Grade grade, const char* text) {
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, ui_col(verdict(t, grade, t.dim)), 0);
    lv_obj_set_style_text_font(lbl, grade == svc::Grade::Bad ? t.f_body_b : t.f_body, 0);
}

/// Set a detail row from a number, and only when the number or its verdict moved. `text` is
/// what to print for that number — formatted by the caller, which is where the units live.
void row(const UiTheme& t, RowId id, int32_t v, svc::Grade grade, const char* text) {
    if (!g.f[id].changed(v, grade)) return;
    paint(t, g.value[id], grade, text);
}

/// A byte count in a detail row — the common case, so the formatting is here rather than at
/// nine call sites. The buffer is a local of THIS function: it is used and copied into the
/// label before it returns, so it costs stack for a moment rather than .bss for ever.
void row_bytes(const UiTheme& t, RowId id, int32_t v, svc::Grade grade) {
    if (!g.f[id].changed(v, grade)) return;
    char buf[24];
    svc::bytes(v, buf, sizeof(buf));
    paint(t, g.value[id], grade, buf);
}

/// A plain counter.
void row_count(const UiTheme& t, RowId id, int32_t v, svc::Grade grade) {
    if (!g.f[id].changed(v, grade)) return;
    char buf[16];
    snprintf(buf, sizeof(buf), "%ld", (long)v);
    paint(t, g.value[id], grade, buf);
}

void refresh(lv_obj_t*, UiCtx& c) {
    const UiTheme& t = *c.th;
    const auto& s = c.s;

    // ── the three that decide a soak ──
    hero(t, H_CPU,  s.cpu_pct,      svc::cpu(s.cpu_pct));
    hero(t, H_LOOP, s.loop_max_ms,  svc::loop_ms(s.loop_max_ms));
    hero(t, H_RAM,  svc::kib(s.heap_internal), svc::internal_free(s.heap_internal));

    // ── audio core + scheduler ──
    row_count(t, R_UNDER, s.underruns, svc::counter(s.underruns));
    {
        // Tenths of a dB, so the comparison is on an integer: `audio.lim_gr` is a float whose
        // low bits move on their own, and comparing floats for "has it changed" would repaint
        // this row on every tick for ever.
        //
        // Bounded before the conversion, not after. A float outside int32_t's range is
        // undefined behaviour to convert, and a limiter whose gain has collapsed to zero
        // publishes exactly that (−20·log10(0)). NaN fails the first comparison and lands on
        // zero, which is the honest reading of "no number arrived".
        const float   gr     = s.lim_gr_db;
        const int32_t tenths = !(gr > 0.0f) ? 0 : (gr < 99.9f ? (int32_t)(gr * 10.0f + 0.5f) : 999);
        // A working limiter is worth noticing but is not a fault — it is doing its job — so
        // it gets the accent and never the red.
        const svc::Grade lim = tenths > 0 ? svc::Grade::Warn : svc::Grade::Ok;
        if (g.f[R_LIM].changed(tenths, lim)) {
            char buf[24];
            svc::gain_reduction(tenths, buf, sizeof(buf));
            paint(t, g.value[R_LIM], lim, buf);
        }
    }
    row_bytes(t, R_STACK, s.stack_min, svc::stack_free(s.stack_min));
    if (g.f_stall.changed(s.stalled, svc::flag(s.stalled[0] != '\0')))
        paint(t, g.value[R_STALL], g.f_stall.grade(), s.stalled[0] ? s.stalled : tr(S::svc_none));

    // ── the run itself ──
    if (g.f[R_UPTIME].changed(s.uptime_s)) {
        char buf[24];
        svc::uptime(s.uptime_s, buf, sizeof(buf));
        lv_label_set_text(g.value[R_UPTIME], buf);
    }
    if (g.f_boot.changed(s.boot_reason, svc::boot_grade(s.boot_reason)))
        paint(t, g.value[R_BOOT], g.f_boot.grade(), svc::boot_reason(s.boot_reason));
    row(t, R_SAFE, s.safe_mode ? 1 : 0, svc::flag(s.safe_mode),
        tr(s.safe_mode ? S::svc_safe_on : S::svc_safe_off));

    // ── memory ──
    //
    // Internal and DMA are graded because they are the two this board runs out of, and so is
    // the lowest internal since boot (`system.heap_internal_min`), on the same line as internal
    // now: it is the same quantity at its worst moment, and a soak is judged on its worst
    // moment. The other three are not. `system.heap_free` is a whole-heap figure, and with
    // CONFIG_SPIRAM_USE_MALLOC=y (boards/p4_player/sdkconfig.board:27) the whole heap is 32 MB
    // of PSRAM plus a rounding error of SRAM, so a threshold on it would be a green light that
    // can never turn red. It is shown because the Web page shows it and the two screens must
    // not disagree — but without a verdict, because there is no honest one to give. Its
    // minimum, `system.heap_min`, is the same green light, so neither screen shows it; its row
    // is the internal minimum's. `system.heap_largest` is the largest INTERNAL block
    // (SystemMonitor: asking it of the whole heap walked PSRAM with interrupts off for
    // milliseconds); it has no threshold yet because nobody has measured what a dangerous one
    // looks like.
    row_bytes(t, R_INT,    s.heap_internal,     svc::internal_free(s.heap_internal));
    row_bytes(t, R_DMA,    s.heap_dma,          svc::dma_free(s.heap_dma));
    row_bytes(t, R_BIG,    s.heap_largest,      svc::Grade::Ok);
    row_bytes(t, R_PSRAM,  s.heap_psram,        svc::Grade::Ok);
    row_bytes(t, R_HFREE,  s.heap_free,         svc::Grade::Ok);
    row_bytes(t, R_INTMIN, s.heap_internal_min, svc::internal_free(s.heap_internal_min));

    // ── the satellite link ──
    //
    // Without a satellite fitted the bt module still publishes these keys, so the column
    // reads "немає" rather than sitting blank — "not connected" and "no such thing" look the
    // same on a panel, and only one of them is a fault. Which is why the link is only amber
    // once a satellite has said hello: a warning that is on from every boot of a board that
    // has no satellite is a warning that gets ignored on the board that does.
    row(t, R_LINK, (s.bt_link ? 1 : 0) | (s.bt_seen ? 2 : 0),
        (!s.bt_link && s.bt_seen) ? svc::Grade::Warn : svc::Grade::Ok,
        tr(s.bt_link ? S::svc_link_up : s.bt_seen ? S::svc_link_lost : S::svc_none));
    row(t, R_PEER, s.bt_connected ? 1 : 0, svc::Grade::Ok,
        tr(s.bt_connected ? S::svc_peer_on : S::svc_none));
    {
        const svc::Grade rtt = s.bt_link ? svc::rtt_ms(s.bt_rtt_ms) : svc::Grade::Ok;
        if (g.f[R_RTT].changed(s.bt_link ? s.bt_rtt_ms : -1, rtt)) {
            char buf[24];
            // No link, no round trip: printing the last one measured would be a stale number
            // presented as a live one. −1 is the value compared for the same reason — without
            // it, a link that drops while the last RTT happened to be 7 ms would not count as
            // a change and the row would keep showing "7 мс".
            if (s.bt_link) snprintf(buf, sizeof(buf), "%ld %s", (long)s.bt_rtt_ms, tr(S::unit_ms));
            else           snprintf(buf, sizeof(buf), "—");
            paint(t, g.value[R_RTT], rtt, buf);
        }
    }
    row_count(t, R_OVF,   s.bt_uart_ovf, svc::soft_counter(s.bt_uart_ovf));
    row_count(t, R_DROPS, s.bt_rx_drops, svc::soft_counter(s.bt_rx_drops));
    row_count(t, R_GAPS,  s.bt_seq_gaps, svc::soft_counter(s.bt_seq_gaps));
}

}  // namespace

// The heart is "vital signs", not "favourites" — there is no favourites feature in this
// firmware for it to be confused with, and the icon font (tools/fonts/build_fonts.py) has no
// pulse or CPU glyph, which the Web page uses for the same page.
const UiScreenDef UI_SCREEN_SVC = {"svc", UI_SYM_HEART, build, refresh, nullptr, nullptr};
