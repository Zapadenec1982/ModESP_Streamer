/**
 * @file sdmmc_card_driver.cpp
 * @brief Bring a microSD card up on the SDMMC controller and mount FAT32 on it.
 *
 * Every line here came from modules/storage, unchanged in order and in effect.
 * It is here because none of it is a decision the module makes: a module that reads files
 * should not know what a power rail, a bus width or a mount config is, and the next board
 * that keeps its files somewhere else should need a second driver, not a second module.
 *
 * Two facts shape it, both from docs/CONCEPT.md, "microSD":
 *
 *   - The pin assignment came from a reference design, not from a schematic, so a failure is
 *     reported WITH the pins that were tried.
 *   - The P4 has ONE SDMMC controller with two slots and ESP-Hosted owns the other for the
 *     C6. That is why host.init/host.deinit below are no-ops, and it is the single most
 *     load-bearing line in this file.
 */

#include "sdmmc_card_driver.h"

#include "modesp/hal/hal.h"
#include "modesp/hal/driver_registry.h"

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "soc/soc_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>

static const char* TAG = "sdmmc_card";

namespace {

/// Copy into a fixed buffer, always NUL-terminated. The status line is shown on the panel,
/// so it is a small fixed buffer rather than a pointer into something that may go away.
void set_str(char* dst, size_t cap, const char* src) {
    if (!cap) return;
    size_t i = 0;
    for (; src && src[i] && i < cap - 1; ++i) dst[i] = src[i];
    dst[i] = 0;
}

/// Identify the card when the bus works but the filesystem does not.
void probe_card(sdmmc_host_t& host, sdmmc_slot_config_t& slot) {
    if (sdmmc_host_init_slot((int)host.slot, &slot) != ESP_OK) return;
    static sdmmc_card_t probe;
    if (sdmmc_card_init(&host, &probe) != ESP_OK) {
        ESP_LOGW(TAG, "probe: card did not initialise");
        return;
    }
    const uint64_t bytes = (uint64_t)probe.csd.capacity * probe.csd.sector_size;
    // log_bus_width is log2 of the width, not the width — printing it raw would claim a
    // 2-bit bus, which does not exist.
    ESP_LOGW(TAG, "probe: '%s' %llu MB, %s, %d-bit bus, sector %d B, %d kHz — the BUS is fine",
             probe.cid.name, bytes / (1024ULL * 1024ULL),
             (probe.ocr & (1 << 30)) ? "SDHC/SDXC" : "SDSC",
             1 << probe.log_bus_width, probe.csd.sector_size,
             probe.max_freq_khz);
}

/// ESP-Hosted already brought the SDMMC controller up for the C6 on the other slot, so the
/// card must not init or tear it down. Straight from Espressif's own
/// esp_hosted/examples/mcu_hosted_sdio_sdmmc_combined, which is exactly this arrangement.
esp_err_t sdmmc_host_init_noop(void)   { return ESP_OK; }
esp_err_t sdmmc_host_deinit_noop(void) { return ESP_OK; }

} // namespace

void SdmmcCardDriver::configure(const modesp::SdmmcSlotConfig* slot) {
    cfg_ = slot;
}

const char* SdmmcCardDriver::mount_point() const {
    if (!cfg_ || cfg_->mount_point.empty()) return "/sd";
    return cfg_->mount_point.c_str();
}

bool SdmmcCardDriver::mount() {
    if (!cfg_) return false;
    if (mounted_) return true;

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = cfg_->slot;
    host.max_freq_khz = (int)cfg_->freq_khz;

    // The controller belongs to ESP-Hosted. Letting the FAT layer init it logs "already
    // initialized" and looks harmless, but its deinit on any failure would tear the C6 link
    // down with it — the esp_hosted changelog lists that as a fixed bug for a reason
    // (CHANGELOG 2.12.11). Taking Wi-Fi down with a card
    // that failed to mount is the worst possible trade, so this stays even when it looks
    // like an omission.
    host.init   = &sdmmc_host_init_noop;
    host.deinit = &sdmmc_host_deinit_noop;

#if SOC_SDMMC_IO_POWER_EXTERNAL
    // On the P4 the SD *IO* rail is fed by an on-chip LDO, not by the 3V3 that powers
    // everything else — so with it off the bus is unpowered and the card never answers
    // CMD0, which reads exactly like wrong pins. Channel 4 is Espressif's default for this
    // chip and matches the combined SDIO+SD example.
    if (cfg_->ldo_chan > 0) {
        sd_pwr_ctrl_ldo_config_t ldo_cfg = {};
        ldo_cfg.ldo_chan_id = cfg_->ldo_chan;
        sd_pwr_ctrl_handle_t pwr = nullptr;
        if (sd_pwr_ctrl_new_on_chip_ldo(&ldo_cfg, &pwr) == ESP_OK) {
            host.pwr_ctrl_handle = pwr;
            ldo_ = pwr;
            ESP_LOGI(TAG, "SD IO rail: on-chip LDO ch%u", (unsigned)cfg_->ldo_chan);
        } else {
            ESP_LOGW(TAG, "on-chip LDO ch%u unavailable", (unsigned)cfg_->ldo_chan);
        }
    }
#endif

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = cfg_->width;
    slot.clk = cfg_->clk;
    slot.cmd = cfg_->cmd;
    slot.d0  = cfg_->d0;
    if (cfg_->width == 4) {
        slot.d1 = cfg_->d1;
        slot.d2 = cfg_->d2;
        slot.d3 = cfg_->d3;
    }
    slot.cd = cfg_->cd;
    // The card's own pull-ups are not guaranteed on a bare TF socket, and a floating CMD
    // line reads as a card that never answers.
    slot.flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mcfg = {};
    mcfg.format_if_mount_failed = false;   // never reformat a user's card on a wiring mistake
    mcfg.max_files = 4;
    mcfg.allocation_unit_size = 16 * 1024;

    const char* mp = mount_point();
    sdmmc_card_t* card = nullptr;

    // Up to three attempts, power-cycling the card between them.
    //
    // A CPU reset does not reset the card: the on-chip LDO keeps the socket powered across a
    // warm boot, so a card left mid-transaction by the previous run answers nothing and the
    // failure looks like wrong pins. Dropping the rail and raising it again is the only reset
    // an SD card has. Espressif's combined SDIO+SD example does the same thing with a
    // board-specific power GPIO; here the rail is the LDO, so the LDO is what gets cycled.
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 3; attempt++) {
        if (attempt > 0) {
            ESP_LOGW(TAG, "mount attempt %d failed (%s) — power-cycling the card",
                     attempt, esp_err_to_name(err));
            if (ldo_) {
                sd_pwr_ctrl_del_on_chip_ldo(static_cast<sd_pwr_ctrl_handle_t>(ldo_));
                ldo_ = nullptr;
                host.pwr_ctrl_handle = nullptr;
                vTaskDelay(pdMS_TO_TICKS(200));          // let the card fully discharge
                sd_pwr_ctrl_ldo_config_t again = {};
                again.ldo_chan_id = cfg_->ldo_chan;
                sd_pwr_ctrl_handle_t pwr = nullptr;
                if (sd_pwr_ctrl_new_on_chip_ldo(&again, &pwr) == ESP_OK) {
                    host.pwr_ctrl_handle = pwr;
                    ldo_ = pwr;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        err = esp_vfs_fat_sdmmc_mount(mp, &host, &slot, &mcfg, &card);
        if (err == ESP_OK) break;
        // FR_NO_FILESYSTEM is not a bus problem — retrying cannot format the card, and
        // saying so plainly beats two more identical failures.
        if (err == ESP_FAIL) {
            // FR_NO_FILESYSTEM: the reads SUCCEEDED, there just was no valid boot sector in
            // what came back. That is two different faults wearing the same face — either
            // the card genuinely is not FAT32, or the bus is marginal and reads return
            // corruption. Commands are short and slow and survive what data transfers do not.
            //
            // One retry at 400 kHz tells them apart, and the firmware should do that itself
            // rather than leave someone guessing: if it mounts slowly, the format was fine
            // all along and the problem is signal integrity.
            if (host.max_freq_khz > SDMMC_FREQ_PROBING) {
                ESP_LOGW(TAG, "no filesystem at %d kHz — retrying at %d kHz to tell a bad "
                              "format from a marginal bus", host.max_freq_khz, SDMMC_FREQ_PROBING);
                host.max_freq_khz = SDMMC_FREQ_PROBING;
                err = esp_vfs_fat_sdmmc_mount(mp, &host, &slot, &mcfg, &card);
                if (err == ESP_OK) {
                    ESP_LOGE(TAG, "MOUNTED AT 400 kHz: the card is fine, the BUS is not. "
                                  "20 MHz in 4-bit needs proper pull-ups on CMD/DAT — the "
                                  "internal ones are too weak. Lower freq_khz in board.json "
                                  "or fit external 10k pull-ups.");
                    break;
                }
            }
            probe_card(host, slot);
            ESP_LOGW(TAG, "no FAT filesystem at any speed — the card itself is not FAT32. "
                          "ESP-IDF's FatFs has no exFAT support at all, and FAT16 cannot "
                          "cover 32 GB, so FAT32 is the only option.");
            break;
        }
    }
    if (err != ESP_OK) {
        // Print the pins with the failure: this is the one setting on the board that is not
        // confirmed, so the log has to be enough to tell a wrong pin from an empty slot.
        ESP_LOGW(TAG, "mount %s failed: %s (slot %u, %u-bit, clk=%d cmd=%d d0=%d, %u kHz)",
                 mp, esp_err_to_name(err), (unsigned)cfg_->slot, (unsigned)cfg_->width,
                 (int)cfg_->clk, (int)cfg_->cmd, (int)cfg_->d0, (unsigned)cfg_->freq_khz);
        set_str(status_, sizeof status_,
                err == ESP_ERR_TIMEOUT ? "картка не відповідає"
                : err == ESP_FAIL      ? "немає FAT32"
                                       : esp_err_to_name(err));
        return false;
    }

    card_ = card;
    mounted_ = true;
    capacity_b_ = (uint64_t)card->csd.capacity * card->csd.sector_size;
    set_str(label_, sizeof label_, card->cid.name);
    set_str(status_, sizeof status_, "ok");
    ESP_LOGI(TAG, "mounted %s: %s, %llu MB, %u-bit @ %u kHz",
             mp, card->cid.name, capacity_b_ / (1024ULL * 1024ULL),
             (unsigned)cfg_->width, (unsigned)card->max_freq_khz);
    measure_free();
    return true;
}

void SdmmcCardDriver::measure_free() {
    uint64_t total_b = 0, free_b = 0;
    if (esp_vfs_fat_info(mount_point(), &total_b, &free_b) == ESP_OK) free_b_ = free_b;
}

void SdmmcCardDriver::unmount() {
    if (!mounted_) return;
    esp_vfs_fat_sdcard_unmount(mount_point(), static_cast<sdmmc_card_t*>(card_));
    mounted_ = false;
    card_ = nullptr;
    // The LDO is deliberately NOT released: dropping the socket's rail buys nothing while
    // risking a card left mid-write on a board where something else has begun writing to it.
    // Shutdown does not come through here: StorageModule::on_stop() leaves the card mounted
    // until the chip resets, because a reader on another task may still be inside it.
}

// ── registry glue ──────────────────────────────────────────────────────────────────────────

namespace {

SdmmcCardDriver s_pool[modesp::MAX_SDMMC_SLOTS];
size_t      s_n = 0;

modesp::storage::IStorageVolume* sdmmc_card_factory(const modesp::Binding& b, modesp::HAL& hal) {
    if (s_n >= modesp::MAX_SDMMC_SLOTS) {
        ESP_LOGE(TAG, "sdmmc_card pool exhausted (max %u)", (unsigned)modesp::MAX_SDMMC_SLOTS);
        return nullptr;
    }
    auto* cfg = hal.find_sdmmc_slot(
        etl::string_view(b.hardware_id.c_str(), b.hardware_id.size()));
    if (!cfg) {
        ESP_LOGE(TAG, "slot '%s' is not in board.json", b.hardware_id.c_str());
        return nullptr;
    }
    auto& drv = s_pool[s_n++];
    drv.configure(cfg);
    return &drv;
}

} // namespace

MODESP_REGISTER_STORAGE(sdmmc_card, &sdmmc_card_factory)
