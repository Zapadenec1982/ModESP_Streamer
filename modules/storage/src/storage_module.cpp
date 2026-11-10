/**
 * @file storage_module.cpp
 * @brief microSD: mount the card, report what is on it.
 *
 * This is the first half of a local player. It answers the question that gates the rest and
 * that nothing else can answer — does the TF slot work on this panel, with these pins, next
 * to ESP-Hosted. Playback is a separate step and needs a decoder decision
 * (docs/CONCEPT.md, "Pinned dependencies").
 *
 * How the card is brought up is drivers/sdmmc_card's business: the power rail, the
 * retries, the FAT mount and the reason ESP-Hosted must not have its host torn down all live
 * there now, with the evidence for them. What is left here is the part that is about a
 * PLAYER rather than about a card: mount at init — with the Wi-Fi link already up, so that
 * if the two cannot coexist it shows immediately rather than the first time someone plays a
 * file — publish what the panel shows, and count what is playable.
 */

#include "storage_module.h"

#include "modesp/hal/driver_registry.h"
#include "modesp/hal/hal.h"
#include "modesp/hal/storage_volume.h"

#include "esp_log.h"

#include <cstring>
#include <dirent.h>
#include <strings.h>

static const char* TAG = "Storage";

namespace {

/// What we consider playable later. Counting them now is also the cheapest proof that
/// reading the card actually works, not just that it enumerated.
bool is_audio(const char* name) {
    const char* dot = strrchr(name, '.');
    if (!dot) return false;
    static const char* const EXT[] = {".wav", ".flac", ".mp3", ".m4a", ".aac"};
    for (const char* e : EXT) if (strcasecmp(dot, e) == 0) return true;
    return false;
}

} // namespace

StorageModule::StorageModule()
    : BaseModule("storage", modesp::ModulePriority::LOW)
{}

void StorageModule::on_bind(modesp::DriverManager&, const modesp::BindingTable& bindings,
                            modesp::HAL& hal) {
    for (const auto& b : bindings.bindings) {
        if (!(b.module_name == "storage")) continue;              // only our bindings
        const char* type = b.driver_type.c_str();
        if (!modesp::DriverRegistry::has_storage(type)) continue; // sensors are DriverManager's
        auto* vol = modesp::DriverRegistry::create_storage(type, b, hal);
        if (!vol) {
            ESP_LOGW(TAG, "volume '%s' [hw=%s, role=%s] create failed",
                     type, b.hardware_id.c_str(), b.role.c_str());
            continue;
        }
        if (!(b.role == "card")) {
            ESP_LOGW(TAG, "unknown storage role '%s' ignored", b.role.c_str());
            continue;
        }
        if (vol_) { ESP_LOGW(TAG, "second 'card' volume ignored"); continue; }
        vol_ = vol;
        ESP_LOGI(TAG, "card '%s' [hw=%s]", type, b.hardware_id.c_str());
    }
}

bool StorageModule::on_init() {
    publish("no slot");
    state_set("storage.sd_tracks", (int32_t)0);

    if (!vol_) {
        // Not an error: a board with no card slot, or a slot whose driver is not in this
        // build, is a board that plays from the network. It must still boot.
        ESP_LOGI(TAG, "no 'card' volume bound — local files disabled");
        return true;
    }

    vol_->mount();
    publish(vol_->status());
    if (vol_->mounted()) scan(vol_->mount_point());
    return true;
}

/// Everything the panel shows about the card, from whatever the volume currently says.
/// storage.sd_free_mb is published without persisting: it is measured once at mount (see
/// IStorageVolume::free_bytes) and is not a setting anybody changes.
void StorageModule::publish(const char* status) {
    const bool     ok    = vol_ && vol_->mounted();
    const uint64_t total = ok ? vol_->capacity_bytes() : 0;
    const uint64_t freeb = ok ? vol_->free_bytes()     : 0;
    state_set("storage.sd_present", ok);
    state_set("storage.sd_status", status ? status : "");
    state_set("storage.sd_total_mb", (int32_t)(total / (1024 * 1024)));
    state_set("storage.sd_free_mb", (int32_t)(freeb / (1024 * 1024)), false);
}

void StorageModule::scan(const char* mount_point) {
    // Root only, and capped. A deep walk of a full card at boot would delay everything else
    // for seconds; the real library index belongs to the player, not here.
    DIR* dir = opendir(mount_point);
    if (!dir) {
        ESP_LOGW(TAG, "opendir(%s) failed — mounted but unreadable", mount_point);
        state_set("storage.sd_status", "unreadable");
        return;
    }
    int tracks = 0, entries = 0;
    struct dirent* de;
    while ((de = readdir(dir)) != nullptr && entries < 512) {
        entries++;
        if (de->d_type != DT_DIR && is_audio(de->d_name)) tracks++;
    }
    closedir(dir);
    state_set("storage.sd_tracks", (int32_t)tracks);
    ESP_LOGI(TAG, "%s: %d entries, %d playable in the root", mount_point, entries, tracks);
}

void StorageModule::on_update(uint32_t) {}

const char* StorageModule::mount_point() const {
    return vol_ ? vol_->mount_point() : "/sd";
}

bool StorageModule::mounted() const { return vol_ && vol_->mounted(); }

/// Deliberately does NOT unmount. stop_all() is only ever followed by
/// esp_restart(), and it stops modules in reverse init order, which puts this module ahead of
/// the player — whose task may at that moment be inside an fread or a cover read on the card.
/// Unmounting under it frees the FIL, the FAT context and the diskio impl that read is still
/// using: a panic on the way to a reboot, before the restart's settings flush and over the
/// coredump. The volume is read-only (storage_volume.h), so there is nothing to flush; the
/// card stays mounted until the chip resets.
void StorageModule::on_stop() {}
