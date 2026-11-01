/**
 * @file eq_store.h
 * @brief The EQ as one document with two editors — the panel's DSP page and the web.
 *
 * Until the web could edit it, the EQ lived in the panel's
 * screen and its module wrote the files. A second editor writing the same files on its own
 * schedule would overwrite the first — that is how an EQ was once lost. So the document lives
 * here, once: the current bands, the user's own set («Власний»), the preset that is on, and a
 * revision that moves with every change.
 *
 * The files. eq.json is what plays and eq_custom.json is
 * «Власний»; each carries "preset", the sound it was saved as. The bands decide what plays and
 * the name only names it: a name whose curve this build does not have plays as it was saved,
 * under «Власний», and does not replace the set eq_custom.json holds. Before this build first
 * writes over a file an older build left, it keeps a copy beside it, <name>.v2, and never
 * writes over that copy; a file it cannot read is moved aside to <name>.bad, never lost.
 *
 * Who does what:
 *   - the screen and the web put changes in (put / put_bands / choose), under a mutex — they
 *     run on the LVGL, main and HTTP tasks;
 *   - the audio module, the owner, watches the revision on the main task: it alone hands the
 *     bands to the audio core (EqBus is single-producer) and it alone writes the files, at a
 *     moment that is not in the way (a LittleFS write stops both cores' caches, and the
 *     speakers click — see modules/audio).
 *
 * An edit says which revision it was made from; one made from an older revision is refused,
 * so a change from the other editor is never silently undone.
 */

#pragma once

#include "eq_presets.h"

#include <atomic>
#include <cstdint>
#include <mutex>

namespace audio_core {

struct EqDoc {
    EqConfig cfg;                   ///< what is heard (eq.json)
    EqConfig custom;                ///< «Власний» (eq_custom.json); what plays, when it is on
    int      preset = EQ_PRESET_CUSTOM;
};

class EqStore {
public:
    /// Read the two files, once; later calls do nothing. No eq.json, or an unreadable one: the
    /// flat ten-band set. No usable eq_custom.json: «Власний» is what is heard. Writes nothing.
    void load(const char* eq_path, const char* custom_path);
    bool loaded() const;

    /// What load() met that is worth a line in the log (modules/audio), as bits.
    enum Note : uint8_t {
        EQ_UNREADABLE     = 1,   ///< eq.json is there and this build cannot read it: flat
        CUSTOM_UNREADABLE = 2,   ///< eq_custom.json is there and cannot be read
        CUSTOM_FROM_COPY  = 4,   ///< «Власний» came from the .v2 copies (load())
    };
    uint8_t load_notes() const;

    /// Starts at 1 once loaded; 0 is never a revision, so it can mean "refused".
    uint32_t revision() const;
    /// A copy of the document, and the revision it is.
    uint32_t get(EqDoc& out) const;
    /// The sound that plays, without copying the document (the «Звук» sheet, every tick).
    int preset() const;

    /// The whole document as an editor holds it (the panel's screen, which applies the preset
    /// rules itself). Returns the new revision.
    uint32_t put(const EqDoc& doc);
    /// Section 0's bands, set by hand, made from revision `base`: the result is «Власний».
    /// 0 when `base` is not the current revision.
    uint32_t put_bands(const EqBand* bands, int count, uint32_t base);
    /// Preset `p`, chosen from revision `base`. 0 when `base` is stale or `p` is not a preset;
    /// the current revision when it is already on.
    uint32_t choose(int p, uint32_t base);

    /// Something changed since the files were last written.
    bool dirty() const;
    /// Write eq_custom.json when «Власний» stopped playing or changed while it did not (while
    /// it plays, eq.json holds it), then eq.json — in that order, so a write cut off between
    /// the two leaves the user's set on disk. The revision counts as saved either way — a
    /// failing write must not be retried every tick. One caller: the owner.
    bool save(const char* eq_path, const char* custom_path);

    // ── the web's say in when it is saved (modules/audio) ──
    /// A web edit at `now_ms`: hold the write while the web is still dragging.
    void web_touched(int64_t now_ms) { web_ms_.store(now_ms, std::memory_order_release); }
    int64_t web_touched_at() const { return web_ms_.load(std::memory_order_acquire); }
    /// The web page is left: write now rather than after the quiet time.
    void keep() { keep_.store(true, std::memory_order_release); }
    bool take_keep() { return keep_.exchange(false, std::memory_order_acq_rel); }

private:
    uint32_t bump_locked(bool custom_moved);
    bool custom_from_copies_locked(const char* eq_path, const char* custom_path);

    mutable std::mutex mx_;
    EqDoc    doc_;
    uint32_t rev_ = 0;
    uint32_t saved_rev_ = 0;
    bool     custom_dirty_ = false;
    bool     loaded_ = false;
    uint8_t  notes_ = 0;
    // Set by load(), cleared by the save that dealt with them; only the owner saves.
    bool     eq_unreadable_ = false;       ///< moved aside before it is written over
    bool     custom_unreadable_ = false;
    bool     older_eq_ = false;            ///< an older build's file: copied before the first write
    bool     older_custom_ = false;
    std::atomic<int64_t> web_ms_{0};
    std::atomic<bool>    keep_{false};
};

EqStore& eq_store();

} // namespace audio_core
