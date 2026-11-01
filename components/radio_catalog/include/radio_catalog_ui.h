/**
 * @file radio_catalog_ui.h
 * @brief What the panel's screens and the radio module share: the catalog screen's asks, and
 *        what it draws besides the page — the request's state, the countries, the genres — and
 *        the presets' logos the Player draws.
 *
 * The screen runs in the LVGL task, which may not write SharedState: the main loop owns it.
 * So the screen leaves what it wants in asks(), the radio module turns each ask into the same
 * states the web page and MQTT write (radio.cat_query, radio.cat_play, …), and what comes
 * back is published in stores the screen reads without a lock. The page itself is
 * CatalogStore (radio_catalog.h).
 *
 * Pure, like the rest of the component: tested on the host.
 */

#pragma once

#include "radio_catalog.h"
#include "station_logo.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace radio_catalog {

/// One writer, any number of readers, no lock: a sequence number that is odd while the value
/// is being written, and a reader that saw it move tries again.
template <typename T>
class Published {
public:
    void set(const T& v) {
        const uint32_t s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        value_ = v;
        seq_.store(s + 2, std::memory_order_release);
    }
    /// False when it kept moving under us; `out` is then not to be used.
    bool get(T& out) const {
        for (int attempt = 0; attempt < 8; attempt++) {
            const uint32_t s0 = seq_.load(std::memory_order_acquire);
            if (s0 & 1u) continue;
            out = value_;
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq_.load(std::memory_order_relaxed) == s0) return true;
        }
        return false;
    }
    /// Moves whenever the value does: the cheap test before a copy.
    uint32_t generation() const { return seq_.load(std::memory_order_acquire); }

private:
    T                     value_{};
    std::atomic<uint32_t> seq_{0};
};

/// What the catalog is doing, as radio.cat_state spells it.
enum class State : uint8_t { IDLE, LOADING, READY, ERROR };

/// The station request the page answers, and how it went. Written by the radio module.
struct Status {
    State    state   = State::IDLE;
    char     spec[64] = "";         ///< radio.cat_query
    Sort     sort    = Sort::POPULAR;
    Quality  quality = Quality::K128;
    int      page    = 0;
    bool     more    = false;
    uint32_t serial  = 0;           ///< the request; the page answers it when its serial matches
    char     error[64] = "";
};
Published<Status>& status();

/// Radio Browser lists 241 countries.
constexpr int MAX_COUNTRIES = 256;

/// The countries the screen offers, in its order. Written by the catalog task.
struct Countries {
    State        state = State::IDLE;
    int          count = 0;
    CountryCount item[MAX_COUNTRIES];
};
Published<Countries>& countries();

/// Puts a parsed /json/countrycodes in the screen's order, in place: only the codes with a
/// name (country_name), a code listed twice counted once, `first` at the top, the rest by station
/// count, most first, and a country with no stations left out. Returns how many are kept.
int order_countries(CountryCount* items, int n, const char* first);

constexpr int MAX_GENRES = 40;

/// data/radio_genres.json as the screen shows it. Written by the radio module, once.
struct Genres {
    int   count = 0;
    Genre item[MAX_GENRES];
};
Published<Genres>& genres();

/// The logos the panel has for the stations on presets (station_logo.h), as the Player's preset
/// buttons draw them. Written by the radio module (modules/radio/src/logos.cpp) whenever a logo
/// arrives or the presets change; the generation is the Player's cue to copy it again.
Published<station_logo::Shelf>& logo_shelf();

enum class AskKind : uint8_t {
    QUERY,          ///< spec: a new request, from its first page
    SORT,           ///< value: a Sort
    QUALITY,        ///< value: a Quality
    NEXT,           ///< the page after this one
    PREV,           ///< the page before
    PLAY,           ///< value: the row to play
    KEEP,           ///< value: the row to keep in the station list; slot: a preset too, or −1
    KEEP_PLAYING,   ///< the catalog station that is playing, kept; slot as for KEEP
    COUNTRIES,      ///< the list of countries, unless the one there is fresh
};

struct Ask {
    AskKind  kind   = AskKind::QUERY;
    int32_t  value  = 0;
    int32_t  slot   = -1;
    uint32_t serial = 0;    ///< PLAY, KEEP: the page the row was read from (Page::serial)
    char     spec[64] = "";
};

/// The screen's asks, in the order made. One writer (the LVGL task) and one reader (the main
/// loop), so two counters are the whole of the synchronisation.
class Asks {
public:
    /// False when the reader has fallen eight behind: the ask is dropped, which at human
    /// rates means the main loop is wedged.
    bool post(const Ask& a);
    bool take(Ask& out);

private:
    static constexpr uint32_t N = 8;
    Ask                   item_[N];
    std::atomic<uint32_t> head_{0};   ///< written by post()
    std::atomic<uint32_t> tail_{0};   ///< written by take()
};
Asks& asks();

}  // namespace radio_catalog
