/**
 * @file touch_gesture.h
 * @brief A finger that scrolls does not set anything — the glass's rules. No LVGL.
 *
 * Without these rules scrolling catches the sliders and the equalizer, on the web and on the
 * glass alike. LVGL only decides that a press is a scroll after scroll_limit, and it sends
 * PRESSING to the pressed widget all the while; a slider that took the finger's x on PRESSED and
 * PRESSING had jumped and moved before the page began to move. The rules are the web's
 * (webui/src/lib/gesture.js):
 *
 *   - a slider: only a finger on its knob takes it — GRAB px either side of its centre, an EQ
 *     point's reach (a touch anywhere on the track would move it, the knob jumping to the finger
 *     once it had gone SLOP px). The gesture is the slider's once that finger has gone SLOP px
 *     along, more along than across, and the knob then follows the finger from where it stood:
 *     it never jumps to it. It is the page's if the finger went up or down
 *     first. A press, a tap or a finger off the knob changes nothing — a tap is how a finger
 *     stops a list that is still moving. The glass's rule only: on the web a finger anywhere on
 *     the slider takes it once it has gone SLOP px along (lib/gesture.js);
 *   - an EQ point: only the point takes a finger. A tap picks its band; the point moves once
 *     held HOLD_MS or once the finger went SLOP px from where it landed;
 *   - «Скасувати»: a snackbar for UNDO_MS after a moved sound setting, from where it started.
 *
 * Split out of the widgets so the rules are pinned on the host (tests/host/test_touch_gesture.cpp).
 */

#pragma once

#include <cstdint>
#include <cstdlib>

namespace touch {

/// px a finger goes before a press is a drag or a scroll
constexpr int32_t SLOP = 8;
/// px either side of a slider knob's centre where a finger takes it: an EQ point's 64 px, halved
constexpr int32_t GRAB = 32;
/// ms a still finger holds an EQ point before it is picked up
constexpr uint32_t HOLD_MS = 300;
/// ms «Скасувати» stays up after a moved setting
constexpr uint32_t UNDO_MS = 5000;

enum class Intent : uint8_t { PRESS, DRAG, SCROLL };

/// What a press has become after moving (dx, dy): still a press, a drag along x, or a scroll.
inline Intent intent(int32_t dx, int32_t dy, int32_t slop = SLOP) {
    const int32_t ax = std::abs(dx), ay = std::abs(dy);
    if (ax >= slop && ax > ay) return Intent::DRAG;
    if (ay >= slop) return Intent::SCROLL;
    return Intent::PRESS;
}

/// One press on a slider, from down to end. move() keeps the first answer that is not PRESS.
class Slide {
public:
    enum State : uint8_t { IDLE, PRESS, DRAG, SCROLL, MISS };

    State state() const { return st_; }
    /// A finger down at (x, y) on a slider whose knob is centred at `knob_x` and stands at `value`.
    /// More than `grab` px off the knob it is MISS for the whole press: nothing moves.
    State down(int32_t x, int32_t y, int32_t knob_x, int32_t value, int32_t grab = GRAB) {
        x0_ = x;
        y0_ = y;
        v0_ = value;
        st_ = std::abs(x - knob_x) <= grab ? PRESS : MISS;
        return st_;
    }
    State move(int32_t x, int32_t y) {
        if (st_ != PRESS) return st_;
        const Intent i = intent(x - x0_, y - y0_);
        if (i == Intent::DRAG) st_ = DRAG;
        else if (i == Intent::SCROLL) st_ = SCROLL;
        return st_;
    }
    /// The page took it over (LVGL found a scroll): nothing more is the slider's.
    void scrolled() { if (st_ != IDLE) st_ = SCROLL; }
    /// Where the finger has taken the knob while it drags: from where the knob stood, as far as
    /// the finger went, `span` px being `range`. Not clamped: the slider's limits are the caller's.
    int32_t follow(int32_t x, int32_t range, int32_t span) const {
        if (span <= 0) return v0_;
        const int64_t num = (int64_t)(x - x0_) * range;
        return v0_ + (int32_t)((num >= 0 ? num + span / 2 : num - span / 2) / span);
    }
    /// The press ends: DRAG for one that dragged; a tap (PRESS), a scroll or a miss did nothing.
    State end() {
        const State s = st_;
        st_ = IDLE;
        return s;
    }

private:
    int32_t x0_ = 0, y0_ = 0, v0_ = 0;
    State   st_ = IDLE;
};

/// One press on an EQ point: PRESS while short and still, LIFT once held `hold` ms (picked up;
/// any move then drags it), DRAG once the finger went `slop` px any way. end() says DRAG for a
/// press that moved the point, PRESS (a tap: the band is picked, nothing moves) otherwise.
/// Times are the caller's, so the test needs no clock.
class Point {
public:
    enum State : uint8_t { IDLE, PRESS, LIFT, DRAG };

    State state() const { return st_; }
    State down(int32_t x, int32_t y, uint32_t t) {
        x0_ = x;
        y0_ = y;
        t0_ = t;
        st_ = PRESS;
        return st_;
    }
    State tick(uint32_t t) {
        if (st_ == PRESS && t - t0_ >= HOLD_MS) st_ = LIFT;
        return st_;
    }
    State move(int32_t x, int32_t y, uint32_t t) {
        tick(t);
        const int32_t dx = x - x0_, dy = y - y0_;
        if (st_ == PRESS && dx * dx + dy * dy >= SLOP * SLOP) st_ = DRAG;
        else if (st_ == LIFT && (dx != 0 || dy != 0)) st_ = DRAG;
        return st_;
    }
    State end() {
        const State s = st_;
        st_ = IDLE;
        return s == DRAG ? DRAG : PRESS;
    }

private:
    int32_t  x0_ = 0, y0_ = 0;
    uint32_t t0_ = 0;
    State    st_ = IDLE;
};

/// Where a setting started, for its «Скасувати»: moved again while the snackbar is up (within
/// `ms` of its last move) it keeps the first start, as the web's lib/undo.js does.
template <class T>
struct UndoStart {
    T        from{};
    uint32_t at = 0;       ///< the last move
    bool     live = false;

    /// A move from `was` at `now`: the start the snackbar says.
    const T& note(const T& was, uint32_t now, uint32_t ms = UNDO_MS) {
        if (!live || now - at >= ms) from = was;
        live = true;
        at = now;
        return from;
    }
    void clear() { live = false; }
};

}  // namespace touch
