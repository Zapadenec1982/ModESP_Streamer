/**
 * @file stations.h
 * @brief The radio station list, its presets, and the JSON they live in.
 *
 * A radio has to work as a radio: plug it in and the last station plays, with no server in
 * the house and nothing to log into (docs/NETWORK.md, "Internet radio"). That means the list of
 * stations lives on the device, and this is it.
 *
 * Pure: no IDF, no filesystem, no network. Parsing and serialising are the parts worth
 * testing, and they are tested on the host; the module reads and writes the file around
 * them. The list is small enough to pass around by value-ish — sixty-four stations is more
 * than anyone tunes between, and each is a name and a URL.
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace radio_core {

constexpr int MAX_STATIONS = 64;
constexpr int NAME_LEN     = 64;
/// Long enough for the query strings station directories put on a stream URL.
constexpr int URL_LEN      = 224;
/// Six, as on a car radio: long press stores, short press tunes.
constexpr int PRESETS      = 6;
/// A Radio Browser stationuuid: 36 characters and the terminator.
constexpr int UUID_LEN     = 37;

struct Station {
    char name[NAME_LEN] = "";
    char url[URL_LEN]   = "";
    /// Radio Browser's id, when the station came from the catalog; "" for one typed in. It is
    /// what a play is counted against and what finds the station's logo again later.
    char uuid[UUID_LEN] = "";
    bool valid() const { return url[0] != '\0'; }
};

struct StationList {
    Station station[MAX_STATIONS];
    int     count = 0;
    /// Index into `station`, or −1 for an empty slot.
    int     preset[PRESETS] = {-1, -1, -1, -1, -1, -1};

    const Station* at(int i) const {
        return (i >= 0 && i < count) ? &station[i] : nullptr;
    }
    bool add(const char* name, const char* url, const char* uuid = nullptr);
    /// Index of a station with this URL, or −1. URLs identify a station across an edit;
    /// names do not, because a name is the part people change.
    int  find(const char* url) const;
};

/// A station's name as a row shows it: one line. A control character, a tab or a line break
/// typed or pasted into a web form or kept in Radio Browser's data, becomes a space, and the
/// spaces at either end go. Every name entering a list passes through it (StationList::add),
/// and so does a catalog row's (components/radio_catalog).
void tidy_name(char* s);

/// Parses `json` (`len` bytes) into `out`. False when the text is not a station list at
/// all; a list with some unreadable entries loses those entries and keeps the rest,
/// because one mistyped URL should not cost somebody their whole set.
bool parse(const char* json, size_t len, StationList& out);

/// Writes `list` as JSON into `out`. Returns the number of bytes written, or 0 if it does
/// not fit.
size_t serialize(const StationList& list, char* out, size_t out_sz);

/// The one list, shared between the radio module that owns it and the screen that draws it.
///
/// Same contract as the card index: the writer is the radio module, the readers are the
/// LVGL and main-loop tasks, and neither reader may block the writer. The list changes only
/// when it is loaded or a preset is stored, so a generation counter is enough — odd while
/// it is being written, even when settled, and a reader that sees it move retries.
class StationStore {
public:
    void set(const StationList& l);
    uint32_t generation() const;
    int      count() const;
    /// Copies entry i. False if the index is out of range or the list moved under us.
    bool     at(int i, Station& out) const;
    int      preset(int slot) const;
    /// Stores station in slot, or −1 to clear it. Writer side.
    void     set_preset(int slot, int station);
    /// A copy of the whole thing, for saving. Writer side.
    void     copy(StationList& out) const;
    /// A copy of the whole thing from another task — the web's GET /api/stations, which runs
    /// on the HTTP server's task. Reader side: retries while the writer is mid-write, and says
    /// which generation it copied, so an edit made from it can be checked against the list it
    /// was made from. False if the list kept moving.
    bool     snapshot(StationList& out, uint32_t& gen) const;

private:
    StationList list_;
    unsigned    gen_ = 0;
};

StationStore& stations();

/// The set a device with no file yet starts from — a handful of stations that have been
/// on the same URLs for years, so a new build plays something the moment it is asked to.
void defaults(StationList& out);

}  // namespace radio_core
