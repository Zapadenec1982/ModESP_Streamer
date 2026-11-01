/**
 * @file station_logo.h
 * @brief The presets' logos kept on the panel: which stations want one and how each stands, the
 *        file a logo lives in, and the shelf the Player draws them from (docs/NETWORK.md,
 *        "Covers and logos").
 *
 * A preset button shows its station's logo in a 40 px disc. The logo is the station's favicon on
 * Radio Browser, found by the station's uuid — a station typed in by hand has none — and most
 * favicons are HTTPS, which the panel may fetch only while the stream holds no TLS session. So
 * the logos are kept: fetched when they can be, decoded once into the disc's own 40 x 40 pixels,
 * and written to the user partition, one small file per station, where the next boot finds them.
 * A logo is on the shelf the moment it is kept; its file waits until nothing is heard (Quiet).
 *
 * Pure, like the rest of radio_catalog: the module fetches, decodes and does the files
 * (modules/radio/src/logos.cpp); what is decided here is pinned on the host
 * (tests/host/test_station_logo.cpp).
 */

#pragma once

#include "radio_catalog.h"   // UUID_LEN

#include <cstddef>
#include <cstdint>

namespace station_logo {

/// The preset's disc, in the panel's own pixels: a logo is drawn as it is kept, unscaled.
constexpr int SIDE   = 40;
constexpr int PIXELS = SIDE * SIDE;
/// One per preset (radio_core::PRESETS).
constexpr int WANT = 6;
constexpr int UUID_LEN = radio_catalog::UUID_LEN;

/// The folder in the user partition the logos live in, and the end of a logo's file name: the
/// station's uuid and this.
constexpr const char* DIR = "logos";
constexpr const char* EXT = ".565";
/// A file: "SLG1", the side twice as 16-bit little-endian, then the pixels row by row, RGB565
/// little-endian. 3208 bytes, one 4 KB block of the partition.
constexpr size_t HEADER     = 8;
constexpr size_t FILE_BYTES = HEADER + 2 * (size_t)PIXELS;

/// A Radio Browser stationuuid as the service writes it: 8-4-4-4-12 lower-case hex digits. Only
/// such a string names a file, so nothing from a station list reaches a path but hex and dashes.
bool valid_uuid(const char* s);
/// "<uuid>.565". False for a uuid that is not one, or no room.
bool file_name(const char* uuid, char* out, size_t out_sz);
/// The uuid a file of this name holds. False for any other name — a candidate safe_file left,
/// or something that is not a logo.
bool uuid_of(const char* name, char* uuid, size_t uuid_sz);
/// The file's bytes for these pixels: FILE_BYTES, or 0 when `out` is smaller.
size_t encode(const uint16_t* px, uint8_t* out, size_t out_sz);
/// The pixels out of a file's bytes. False for anything this firmware did not write.
bool decode(const uint8_t* file, size_t len, uint16_t* px);

/// How long a station waits before its logo is asked for again: one whose favicon could not be
/// had — none listed, a dead address, a format the panel does not decode — and one whose lookup
/// found Radio Browser not answering. Both also at the next boot: the waits are not kept.
constexpr int64_t RETRY_LATE_US = 6LL * 3600 * 1000000;
constexpr int64_t RETRY_SOON_US = 10LL * 60 * 1000000;

enum class State : uint8_t {
    NEW,    ///< just wanted: its file not looked for yet
    HAVE,   ///< on the shelf
    NEED,   ///< no logo: to be fetched
    BUSY,   ///< with the task that fetches
    WAIT,   ///< a fetch failed: not again before `retry_us`
};

struct Entry {
    char    uuid[UUID_LEN] = "";
    State   state = State::NEW;
    int64_t retry_us = 0;
    bool    unsaved = false;   ///< on the shelf, not in its file yet (Plan::kept)
};

/// The stations whose logos are wanted, and how each one stands. The radio module's, main task.
class Plan {
public:
    /// The presets' uuids, slot by slot — nullptr, "" or a string that is no uuid for a preset
    /// without one. The wanted set becomes those, each once: a uuid still wanted keeps its state
    /// and its wait, one no longer wanted is dropped. True when the set changed.
    bool want(const char* const* uuids, int n);
    int          count() const { return n_; }
    const Entry& at(int i) const { return e_[i]; }
    /// The entry for `uuid`, or −1.
    int  find(const char* uuid) const;
    void set(int i, State s) { e_[i].state = s; }
    /// A fetch failed: WAIT until now + delay.
    void wait(int i, int64_t now_us, int64_t delay_us);
    /// The entries due a fetch at `now_us` — NEED, and WAIT whose time has come — up to `max`
    /// indices into `out`. Their count.
    int  due(int64_t now_us, int* out, int max) const;
    /// The entry's logo, new or changed, is on the shelf: HAVE, and its file is still to be
    /// written, once the panel is quiet. Not fetched again meanwhile; a restart before the write
    /// finds no file, and the station is fetched again.
    void kept(int i);
    /// An entry whose logo is still to be written, or −1.
    int  to_save() const;
    /// Its file is written, or could not be: nothing more to write for this logo.
    void saved(int i);

private:
    Entry e_[WANT];
    int   n_ = 0;
};

/// How long nothing must have been heard before a logo's file is written or removed: a LittleFS
/// write stops both cores' caches, and with music playing the speakers click (docs/AUDIO.md, the
/// EQ file). A skip or a station rebuffering is a gap, not the end of the music, so the standby
/// face's four seconds (docs/UI.md, "Standby").
constexpr uint32_t QUIET_MS = 4000;

/// Whether the panel has been quiet long enough for the files. The shelf does not wait for it.
class Quiet {
public:
    /// `dt_ms` since the last call; `silent`: nothing is heard now. True once that has held for
    /// QUIET_MS; a sound starts the count again.
    bool tick(uint32_t dt_ms, bool silent);

private:
    uint32_t ms_ = 0;
};

/// A logo as the Player draws it.
struct Thumb {
    char     uuid[UUID_LEN] = "";
    uint16_t px[PIXELS] = {};
};

/// The logos the panel has for the stations on presets now. Written by the radio module and
/// published through radio_catalog::logo_shelf(), which the Player reads.
struct Shelf {
    int   count = 0;
    Thumb item[WANT];

    /// The logo of `uuid`, or nullptr.
    const Thumb* find(const char* uuid) const;
    /// `px` becomes `uuid`'s logo, in its place or a free one. False when the shelf is full.
    bool put(const char* uuid, const uint16_t* px);
    /// Only the logos `plan` still wants stay.
    void keep(const Plan& plan);
};

}  // namespace station_logo
