/**
 * @file user_store.cpp
 * @brief The userdata partition: mount, migrate once, and resolve paths. See user_store.h.
 */

#include "user_store.h"

#include "safe_file.h"
#include "esp_log.h"
#include "esp_littlefs.h"

#include <cstdio>
#include <cstring>

static const char* TAG = "UserStore";

namespace modesp {

bool UserStore::mounted_ = false;

// eq.json         the EQ someone tuned            (components/audio_dsp/eq_config.cpp)
// stations.json   their station list and presets  (modules/radio)
// last_track      the track to come back to       (modules/player)
// devices.json    remote devices they subscribed  (POST /api/devices)
// eq_custom.json  their own EQ while a preset plays (modules/ui_lvgl, the DSP page)
//
// board.json / bindings.json / ui.json / www are NOT here: they are build output, and a
// flash is supposed to replace them.
const char* const UserStore::FILES[] = {
    "eq.json",
    "stations.json",
    "last_track",
    "devices.json",
    "eq_custom.json",
    nullptr,
};

namespace {

bool join(char* out, size_t cap, const char* dir, const char* name) {
    const int n = snprintf(out, cap, "%s/%s", dir, name);
    return n > 0 && (size_t)n < cap;
}

} // namespace

bool UserStore::separate() { return mounted_; }

int UserStore::promote_staged(const char* dir) {
    int moved = 0;
    for (const char* const* f = FILES; *f; ++f) {
        char staged[160], path[160];
        if (!join(path, sizeof(path), dir, *f)) continue;
        const int n = snprintf(staged, sizeof(staged), "%s%s", path, STAGED);
        if (n <= 0 || (size_t)n >= sizeof(staged) || !safe_file::exists(staged)) continue;
        // copy, not rename: safe_file's write -> read back -> rename, so a power cut leaves
        // either the old file or the restored one, never half of either
        if (safe_file::copy(staged, path)) {
            remove(staged);
            moved++;
        } else {
            ESP_LOGW(TAG, "%s: the restored copy did not go in; it is kept for the next boot", path);
        }
    }
    if (moved) ESP_LOGW(TAG, "%d file(s) put in place from a backup in %s", moved, dir);
    return moved;
}

int UserStore::marker_version(const char* dir) {
    char path[160];
    if (!join(path, sizeof(path), dir, MARKER)) return -1;
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    char buf[16] = {};
    const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return -1;
    buf[n] = '\0';
    int v = -1;
    // A marker that does not hold a number is treated as no marker: the migration then runs
    // again, and because it never overwrites and never deletes, running again is harmless.
    if (sscanf(buf, "%d", &v) != 1) return -1;
    return v;
}

UserStore::Report UserStore::migrate(const char* from, const char* to) {
    Report r;

    for (const char* const* it = FILES; *it; ++it) {
        char src[160], dst[160];
        if (!join(src, sizeof(src), from, *it) || !join(dst, sizeof(dst), to, *it)) {
            r.failed++;
            continue;
        }

        // Leftovers from a copy that was cut short. They are candidates, never live files,
        // so removing them costs nothing and keeps the store free of half-written names.
        safe_file::sweep(dst);

        // The destination wins, always. This is what makes re-running safe: after the first
        // migration the file in the store is the one the person has been editing, and the
        // copy left behind in the image is a stale snapshot from before the flash.
        if (safe_file::exists(dst)) { r.kept++; continue; }
        if (!safe_file::exists(src)) { r.absent++; continue; }

        if (safe_file::copy(src, dst)) {
            r.copied++;
            ESP_LOGW(TAG, "migrated %s -> %s", src, dst);
        } else {
            // Not fatal and not final: the source is untouched, so the next boot tries again.
            r.failed++;
            ESP_LOGE(TAG, "could not migrate %s -> %s", src, dst);
        }
    }

    // Written last, and only when nothing failed — a partial migration must be retried, and
    // a marker would stop that. Its absence costs one directory walk per boot, no more.
    if (r.failed == 0) {
        char path[160];
        char text[8];
        const int len = snprintf(text, sizeof(text), "%d\n", SCHEMA);
        if (len > 0 && join(path, sizeof(path), to, MARKER)) {
            r.marked = safe_file::save(path, text, (size_t)len);
        }
    }
    return r;
}

void UserStore::begin() {
    if (mounted_) return;

    esp_vfs_littlefs_conf_t conf = {};
    conf.base_path = DIR;
    conf.partition_label = "userdata";
    // The one place in this firmware where an unmountable partition is formatted rather than
    // reported. It has to be: on a fresh panel the partition has never held a filesystem, and
    // on an upgraded one it holds the middle of the old `data` image. Both must end up as an
    // empty store without anyone being asked. `data` keeps format_if_mount_failed = false for
    // the opposite reason — there, a failed mount means the image is wrong.
    conf.format_if_mount_failed = true;
    conf.dont_mount = false;

    // A second mount is not free: esp_littlefs keeps an lfs_t, three 512-byte caches
    // (CONFIG_LITTLEFS_CACHE_SIZE), a 128-byte lookahead buffer and a descriptor table per
    // partition, on the order of 2.5 KB of internal heap with the default malloc strategy.
    // Left on the default deliberately - LITTLEFS_MALLOC_STRATEGY_SPIRAM would move it off
    // internal RAM, but it would put buffers used during flash operations in PSRAM, and
    // internal RAM is the thing this board is short of for reasons that are cheaper to fix
    // elsewhere (stage 7). Worth measuring on the bench next to the stage 7 numbers.

    const esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NOT_FOUND) {
            // The app was updated over the air onto a partition table that predates the
            // split. Everything works; it works the old way. WARN, not ERROR: nothing is
            // broken, but the next full flash will take the EQ with it, and this line is the
            // only warning anyone gets.
            ESP_LOGW(TAG, "no 'userdata' partition — this panel still has the old table");
            ESP_LOGW(TAG, "user files stay in %s and a full flash will erase them; "
                          "a full flash (table included) is what fixes it - see "
                          "tools/user_data.py, 'THE ONE-TIME MOVE'", IMAGE_DIR);
        } else {
            ESP_LOGE(TAG, "userdata mount failed: %s — falling back to %s",
                     esp_err_to_name(err), IMAGE_DIR);
        }
        mounted_ = false;
        promote_staged(IMAGE_DIR);   // a restore on the old table was staged there
        return;
    }
    mounted_ = true;
    // Before the migration and before any module reads its file: what a backup brought.
    promote_staged(DIR);

    const int marker = marker_version(DIR);
    if (marker >= SCHEMA) {
        ESP_LOGI(TAG, "user files in %s (schema %d)", DIR, marker);
        return;
    }

    const Report r = migrate(IMAGE_DIR, DIR);
    // At INFO because it is the answer to "where did my EQ go" and it is printed once per
    // boot at most. The per-file lines above are WARN so they survive the shipping log level.
    ESP_LOGI(TAG, "migration %s -> %s: %d copied, %d kept, %d absent, %d failed%s",
             IMAGE_DIR, DIR, r.copied, r.kept, r.absent, r.failed,
             r.marked ? "" : " (not marked — it will run again)");
    if (r.failed) {
        ESP_LOGW(TAG, "%d file(s) did not migrate; the originals are still in %s",
                 r.failed, IMAGE_DIR);
    }
}

const char* UserStore::read_path_in(const char* store_dir, const char* image_dir,
                                    const char* name, char* buf, size_t n) {
    if (store_dir && join(buf, n, store_dir, name) && safe_file::exists(buf)) return buf;
    // The store does not have it: either there is no store, or this file has never been
    // written. Either way the image's copy is the right answer — that is how a fresh panel
    // gets the shipped eq.json.
    if (!join(buf, n, image_dir, name)) buf[0] = '\0';
    return buf;
}

const char* UserStore::write_path_in(const char* store_dir, const char* image_dir,
                                     const char* name, char* buf, size_t n) {
    if (!join(buf, n, store_dir ? store_dir : image_dir, name)) buf[0] = '\0';
    return buf;
}

const char* UserStore::read_path(const char* name, char* buf, size_t n) {
    return read_path_in(mounted_ ? DIR : nullptr, IMAGE_DIR, name, buf, n);
}

const char* UserStore::write_path(const char* name, char* buf, size_t n) {
    return write_path_in(mounted_ ? DIR : nullptr, IMAGE_DIR, name, buf, n);
}

} // namespace modesp
