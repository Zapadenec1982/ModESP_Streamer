/**
 * @file airplay_meta.h
 * @brief Which of a sender's metadata records reach the tile. Pure, so the host tests reach it.
 *
 * An iPhone playing an Apple Music radio station sends, about every 15 s, a record that carries
 * the station's name as the title with no artist and no album, with its own cover or none; about
 * 1.5 s later the track's record comes back. Taken as they came, the tile showed the station and
 * lost the track's cover for that second and a half, every 15 s — music unbroken (the panel's WS
 * log of such a session). Between those, the cover alone did the same: an empty one, then the
 * track's again 1.3–3 s later.
 *
 * So two things wait up to WINDOW_US before they are believed:
 *  - a title-only record over a track that has an artist (an interlude), with any cover that
 *    arrives while it waits. The track's record coming back drops both;
 *  - an empty cover ("none") over the shown one. A real cover arriving drops it.
 * Neither waits longer: a real change to a title-only item, or a track without a cover, shows
 * WINDOW_US late. The module calls this under its spinlock; nothing here allocates or frees.
 */
#pragma once

#include <cstdint>

namespace airplay_meta {

class Smoother {
public:
    static constexpr int64_t WINDOW_US = 5 * 1000 * 1000;

    enum class Names : uint8_t { APPLY, HOLD };
    enum class Cover : uint8_t { POST, HOLD };

    /// A names record: `title_only` (no artist, no album), whether the shown track has an artist,
    /// whether the record's title is the shown one. `drop_cover` says a held cover is now stale.
    Names on_names(bool title_only, bool shown_has_artist, bool same_title, int64_t now,
                   bool& drop_cover) {
        drop_cover = false;
        if (title_only && shown_has_artist && !same_title) {
            if (!title_since_) title_since_ = stamp(now);
            return Names::HOLD;
        }
        if (title_since_) {               // the track came back: that was an interlude
            title_since_ = 0;
            drop_cover = cover_since_ != 0;
            cover_since_ = 0;
        }
        return Names::APPLY;
    }

    /// A cover arrived (`empty`: "this item has none"). `drop_held` says the one held before is
    /// replaced by this one.
    Cover on_cover(bool empty, int64_t now, bool& drop_held) {
        drop_held = cover_since_ != 0;
        if (title_since_ || empty) {
            cover_since_ = stamp(now);
            cover_tied_  = title_since_ != 0;
            return Cover::HOLD;
        }
        cover_since_ = 0;
        return Cover::POST;
    }

    struct Due { bool title; bool cover; };
    /// What has waited long enough to be believed: show the held title, post the held cover.
    Due due(int64_t now) {
        Due d{false, false};
        if (title_since_ && now - title_since_ >= WINDOW_US) {
            title_since_ = 0;
            d.title = true;
            if (cover_since_ && cover_tied_) {
                cover_since_ = 0;
                d.cover = true;
            }
        }
        if (cover_since_ && !cover_tied_ && now - cover_since_ >= WINDOW_US) {
            cover_since_ = 0;
            d.cover = true;
        }
        return d;
    }

    bool holding_title() const { return title_since_ != 0; }
    bool holding_cover() const { return cover_since_ != 0; }
    /// The session ended: nothing held is shown; the caller frees the cover it held.
    void reset() { title_since_ = cover_since_ = 0; }

private:
    static int64_t stamp(int64_t now) { return now ? now : 1; }   // 0 means "nothing held"

    int64_t title_since_ = 0;
    int64_t cover_since_ = 0;
    bool    cover_tied_  = false;   ///< held because an interlude was, not for being empty
};

} // namespace airplay_meta
