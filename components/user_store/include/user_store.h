/**
 * @file user_store.h
 * @brief Where a person's own files live, and how they got out of the build's way.
 *
 * Until 22.09.2026 the firmware wrote what someone makes — the EQ they tuned, their station
 * list, the track to resume, the BLE devices they subscribed — into `/data`, the same
 * LittleFS partition the build fills with board.json, ui.json and the web bundle. `idf.py
 * flash` rewrites that partition whole, and a board.json change needs a full flash because
 * board.json is not in the app. So every board.json change destroyed all of it. Measured on
 * 18.09.2026 (an EQ with two active bands and a six-station list, none of it in git) and
 * again on 22.09.2026 while fixing the I2S slot width.
 *
 * `tools/user_data.py` made that survivable by reading the panel before a flash and seeding
 * the files back into the image. Its own docstring says what this class is: "the fix is to
 * stop storing user files next to build output."
 *
 * So there are two partitions (partitions_p4_16mb.csv):
 *
 *   data      build output   rewritten by every `idf.py flash`
 *   userdata  a person's own NO image in the build, so a flash never touches it
 *
 * ── Three things this has to survive ───────────────────────────────────────────────────
 *
 * 1. A blank `userdata`. On a panel upgraded from the old table, 0xF80000 holds whatever the
 *    old `data` filesystem had there — not 0xFF, just meaningless. The mount fails and the
 *    partition is formatted. `data` must NOT be treated that way (a mount failure there is a
 *    broken image and belongs in safe mode), which is why they are mounted with different
 *    rules.
 *
 * 2. An app updated over the air onto the OLD partition table. Then there is no `userdata`
 *    partition at all, and `esp_vfs_littlefs_register` says so with ESP_ERR_NOT_FOUND. That
 *    must not be a boot failure: the store falls back to `/data` exactly as before, says so
 *    at WARN, and everything keeps working — with the old exposure to a full flash, which is
 *    what the warning is for.
 *
 * 3. The migration being interrupted. Power can go at any point during the first boot after
 *    an update. Correctness does not rest on the marker file: a file is copied only when the
 *    destination does not already have it, and nothing is ever deleted from the source. So
 *    re-running the migration any number of times, from any point, converges on the same
 *    result. The marker is written last and only says "do not bother looking again".
 */

#pragma once

#include <cstddef>

namespace modesp {

class UserStore {
public:
    /// Mount point of the `userdata` partition.
    static constexpr const char* DIR = "/userdata";
    /// Mount point of the build's own partition — the fallback, and what is migrated FROM.
    static constexpr const char* IMAGE_DIR = "/data";
    /// Marker file, inside DIR. Holds the schema number as text, nothing else.
    static constexpr const char* MARKER = ".store";
    /// Layout of the store itself. Bumped when what lives in DIR changes shape, not when one
    /// of the files inside it does — each file carries its own version (eq.json has one).
    static constexpr int SCHEMA = 1;

    /// The files the firmware writes while someone uses the device, and the only ones
    /// migrated. Build output is deliberately absent: board.json, bindings.json, ui.json and
    /// the web bundle belong to the image and are rewritten by every flash on purpose.
    /// nullptr-terminated so host tests and tools can walk it.
    static const char* const FILES[];

    /// Mount `userdata`, formatting it when it will not mount, and migrate once from
    /// IMAGE_DIR. Never fails a boot: on any problem the store falls back to IMAGE_DIR.
    /// Call after the `data` partition is mounted — the migration reads from it.
    static void begin();

    /// True when `userdata` is mounted, i.e. a full flash will no longer take user files.
    static bool separate();

    /// The directory user files live in right now: DIR when the store is up, IMAGE_DIR when
    /// it is not. For callers that want the directory rather than one file's path.
    static const char* dir() { return mounted_ ? DIR : IMAGE_DIR; }

    /// Path to READ `name` from: DIR when the store has it, else the image's own copy. The
    /// fallback is what gives a fresh panel the shipped eq.json on its first boot.
    static const char* read_path(const char* name, char* buf, size_t n);

    /// Path a runtime write must go to: DIR when mounted, IMAGE_DIR otherwise.
    static const char* write_path(const char* name, char* buf, size_t n);

    /// The two rules above, without the mount. `store_dir == nullptr` is "there is no
    /// store". Split out because a host test cannot mount a partition but the rules are
    /// exactly what a panel on the old table depends on.
    static const char* read_path_in(const char* store_dir, const char* image_dir,
                                    const char* name, char* buf, size_t n);
    static const char* write_path_in(const char* store_dir, const char* image_dir,
                                     const char* name, char* buf, size_t n);

    struct Report {
        int copied  = 0;   ///< files taken from `from` this time
        int kept    = 0;   ///< already in `to` — an earlier run did them, or the user has newer
        int absent  = 0;   ///< not in `from` either; nothing to do
        int failed  = 0;   ///< could not be copied; the source is still there to retry from
        bool marked = false;  ///< the marker was written (only when failed == 0)
    };

    /// Copy every file of FILES that `to` does not already have out of `from`.
    ///
    /// Idempotent and interruptible by construction, see the header comment. Exposed (and
    /// taking directories rather than using the constants) so the host tests can drive it:
    /// fresh device, device with old files, already migrated, interrupted halfway.
    static Report migrate(const char* from, const char* to);

    /// Schema number recorded in `<dir>/.store`, or -1 when there is no marker. A marker
    /// from a LATER schema is returned as written; begin() treats it as "leave this alone".
    static int marker_version(const char* dir);

    /// A file restored from a backup (POST /api/userdata) is not written over
    /// the one in use: the module that owns it holds its own copy in memory and would write
    /// that back. It is put beside it as `<name>.restore`, the device restarts, and begin()
    /// puts it in place before any module has read anything.
    static constexpr const char* STAGED = ".restore";

    /// Put every `<dir>/<name>.restore` of FILES in place of `<dir>/<name>`, and remove it.
    /// One that cannot be copied stays where it is, for the next boot. Returns how many moved.
    static int promote_staged(const char* dir);

private:
    static bool mounted_;
};

} // namespace modesp
