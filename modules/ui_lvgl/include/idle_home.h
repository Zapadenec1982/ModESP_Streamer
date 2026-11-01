/**
 * @file idle_home.h
 * @brief Back to the Player after a minute alone: when, and how often. No LVGL.
 *
 * Left for AFTER_MS with no finger on the glass and no turn or press of the knob, the panel goes
 * back to the bare Player by itself: another page gives way to it, and the menu, the «Звук» sheet
 * and the pick list close, over the Player too — the sheet only ever opens there. It is a stereo,
 * and the Player is what it shows.
 *
 * Not from Service, which is watched for minutes on end, nor from the component test page. Not
 * while something holds the page — a keyboard or a dialog waiting for an answer, a test tone, a
 * measurement: the panel goes once that is over, if it is still alone. Never in standby, which
 * comes later (ui.standby_min) and wakes on the page it fell asleep on.
 *
 * Once per idle period: the first pass after AFTER_MS decides, so a page brought up later in the
 * same period without a touch (the bench's /api/screenshot?screen=) stays.
 *
 * The idle time is the standby's own (UiLvglModule's idle_ms_): a touch or the knob restarts both.
 * Split out of the shell so the rules are pinned on the host (tests/host/test_idle_home.cpp).
 */

#pragma once

#include <cstdint>
#include <cstring>

namespace idle_home {

/// ms alone before the panel goes back to the Player
constexpr uint32_t AFTER_MS = 60000;

/// A page that is looked at rather than touched, and stays: Service and the component test page.
/// `page` is the id ui.screen publishes.
inline bool watched(const char* page) {
    return strcmp(page, "svc") == 0 || strcmp(page, "m3test") == 0;
}

/// One idle period: the panel goes back at most once in it.
class Period {
public:
    /// One refresh pass. `idle_ms`: since the last touch or turn of the knob; `standby`: the face
    /// is up; `page`: the id of the page on the glass; `over`: the menu, the «Звук» sheet or the
    /// pick list is open. `held()` says something holds the page; it is asked only when the panel
    /// would go, because on the glass it walks the object tree. True on the pass the panel goes.
    template <class Held>
    bool pass(uint32_t idle_ms, bool standby, const char* page, bool over, Held&& held) {
        if (idle_ms < AFTER_MS) {           // touched since: a new period
            spent_ = false;
            return false;
        }
        if (spent_ || standby) return false;
        if (watched(page) || (strcmp(page, "now") == 0 && !over)) {
            spent_ = true;                  // nothing to do in this one
            return false;
        }
        if (held()) return false;           // waits, and goes once it lets go
        spent_ = true;
        return true;
    }

private:
    bool spent_ = false;
};

}  // namespace idle_home
