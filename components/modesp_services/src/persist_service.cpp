/**
 * @file persist_service.cpp
 * @brief Auto-persist readwrite settings to NVS
 *
 * При boot: ітерує STATE_META, для persist==true читає NVS → SharedState.
 * При runtime: SharedState callback ставить dirty flag, on_update() пише з debounce.
 *
 * BUG-012 fix: NVS ключі на основі djb2 хешу імені state key (стабільні
 * при додаванні/видаленні/перестановці ключів). Автоматична міграція зі
 * старих позиційних ключів "p0".."p32" при першому boot.
 *
 * BUG-025 fix: debounce_timer_ більше НЕ скидається в on_state_changed().
 * Runtime-лічильник, що змінювався щосекунди, скидав глобальний таймер,
 * через що жоден persist key (включаючи setpoint-и) не флашився.
 *
 * NVS wear fix: runtime counters (per-second timers, cycle counters) прибрані
 * з persist — скидаються при ребуті, не зношують NVS.
 *
 * NVS wear, second pass: a volume knob changes a persist key many times in a row, and the
 * BUG-025 rule — debounce from the FIRST dirty flag, never restarted — turned that into one
 * write every DEBOUNCE_MS for as long as the knob moved, each with a different intermediate
 * value. The window is now restarted while changes keep arriving, but only up to
 * MAX_DEFER_MS, so a value that never settles still reaches NVS and BUG-025 cannot return.
 * A sweep of the knob costs one write, of the value it lands on.
 *
 * flush_to_nvs() also reads before it writes and skips keys whose stored value already
 * matches. NVS does that check internally too, so this saves no flash by itself — what it
 * buys is a truthful log: "Saved X to NVS" now means the contents of the flash changed,
 * which is the number worth watching when judging wear.
 *
 * Legacy keys, one pass. A positional key "p<i>" names STATE_META[i] of the firmware that
 * wrote it; a manifest that gains or loses a state shifts every index after it. The
 * BUG-012 migration nevertheless ran on EVERY boot, for every persisted state that had no
 * hashed key yet — which is every state nobody has changed — so each state added to a
 * manifest inherited whatever stale positional value sat at its index. That way
 * radio.cat_quality (new, default 1) came up as 3, and the migration then wrote the 3 under
 * its hashed key and erased the positional one: the wrong value became the stored one.
 *
 * Positional keys are now read by one pass in a device's life, and only where they can
 * still mean something: on a device without a single hashed key, i.e. the first boot after
 * the positional firmware. A device with any hashed key went through that boot long ago,
 * and what is left under "p<i>" is stale whatever the index. Either way the pass ends by
 * erasing every "p<n>" and writing LEGACY_RETIRED, and after that nothing positional is
 * read again. A pass cut short by a power failure runs again on the next boot, and a
 * migration has to resume there rather than be taken for finished because of the hashed
 * keys it already wrote, so LEGACY_MIGRATING goes in before the first of them. Every step
 * is idempotent, and LEGACY_RETIRED is the last write.
 */

#include "modesp/services/persist_service.h"
#include "modesp/services/nvs_helper.h"
#include "state_meta.h"
#include "esp_log.h"
#include "esp_system.h"

#include <cstdio>
#include <cstring>

static const char* TAG = "Persist";

namespace modesp {

PersistService::PersistService()
    : BaseModule("persist", ModulePriority::CRITICAL)
{
    memset(dirty_, 0, sizeof(dirty_));
}

// BUG-012: стабільний NVS ключ — djb2 хеш від імені state key
// Формат: "s" + 7 hex символів = 8 chars (ESP-IDF NVS limit: 15 chars)
void PersistService::make_nvs_key(const char* state_key, char* out, size_t out_size) {
    uint32_t hash = 5381;
    for (const char* p = state_key; *p; p++) {
        hash = ((hash << 5) + hash) + static_cast<unsigned char>(*p);
    }
    snprintf(out, out_size, "s%07lx", (unsigned long)(hash & 0x0FFFFFFFul));
}

// Legacy: позиційний ключ для міграції зі старого формату
void PersistService::make_legacy_nvs_key(size_t index, char* out, size_t out_size) {
    snprintf(out, out_size, "p%u", (unsigned)index);
}

bool PersistService::has_hashed_key(nvs_handle_t h) {
    for (size_t i = 0; i < gen::STATE_META_COUNT; i++) {
        if (!gen::STATE_META[i].persist) continue;
        char key[16];
        make_nvs_key(gen::STATE_META[i].key, key, sizeof(key));
        if (nvs_helper::batch_has_key(h, key)) return true;
    }
    return false;
}

int PersistService::erase_legacy_keys(nvs_handle_t h) {
    int erased = 0;
    for (size_t n = 0; n < gen::STATE_META_COUNT + LEGACY_KEY_MARGIN; n++) {
        char key[8];
        make_legacy_nvs_key(n, key, sizeof(key));
        if (nvs_helper::batch_has_key(h, key) && nvs_helper::batch_erase_key(h, key)) erased++;
    }
    return erased;
}

// player-fork: every esp_restart() writes what is pending. The routes that restart flushed by
// hand and the rest did not: a setting changed in the seconds before an update over HTTP (until
// it flushed too), an MQTT update or the Wi-Fi restart was lost with it. A shutdown handler runs
// on every esp_restart(), whoever calls it; a panic does not run them, and has nothing a flush
// could be trusted with anyway. ESP-Hosted's own restart when the C6 stops answering is such a
// panic (it calls abort()), so what changed in the seconds before it is still lost.
static PersistService* s_restart_flush = nullptr;
static void flush_on_restart() {
    if (s_restart_flush) s_restart_flush->flush_now();
}

bool PersistService::on_init() {
    if (!ext_state_) {
        ESP_LOGE(TAG, "SharedState не встановлений — викличте set_state() перед init");
        return false;
    }

    // Відновлюємо збережені значення з NVS
    restore_from_nvs();

    // Реєструємо persist callback
    ext_state_->set_persist_callback(on_state_changed, this);

    s_restart_flush = this;
    if (esp_register_shutdown_handler(flush_on_restart) != ESP_OK) {
        ESP_LOGW(TAG, "no shutdown handler: a restart may lose the last %lu ms of settings",
                 (unsigned long)DEBOUNCE_MS);
    }

    ESP_LOGI(TAG, "Initialized (debounce=%lu ms)", DEBOUNCE_MS);
    return true;
}

void PersistService::restore_from_nvs() {
    if (!ext_state_) return;

    int restored = 0;
    int migrated = 0;

    // Batch: один handle для всіх read операцій (зменшує heap фрагментацію)
    auto h_ro = nvs_helper::batch_open(NVS_NAMESPACE, true);
    // RW handle для міграції (відкриваємо лише якщо потрібно)
    nvs_handle_t h_rw = 0;

    // Positional keys: one pass at most — see "Legacy keys, one pass" in the file header.
    int32_t legacy = 0;   // absent: this code has not run on this device yet
    nvs_helper::batch_read_i32(h_ro, LEGACY_MARK_KEY, legacy);
    const bool read_legacy = legacy == LEGACY_MIGRATING
                          || (legacy != LEGACY_RETIRED && !has_hashed_key(h_ro));
    // Before the first migrated value is written, record that a migration is under way.
    auto begin_migration = [&]() {
        if (h_rw) return;
        h_rw = nvs_helper::batch_open(NVS_NAMESPACE, false);
        nvs_helper::batch_write_i32(h_rw, LEGACY_MARK_KEY, LEGACY_MIGRATING);
    };

    for (size_t i = 0; i < gen::STATE_META_COUNT; i++) {
        const auto& meta = gen::STATE_META[i];
        if (!meta.persist) continue;

        char nvs_key[16];
        make_nvs_key(meta.key, nvs_key, sizeof(nvs_key));

        char legacy_key[8];
        make_legacy_nvs_key(i, legacy_key, sizeof(legacy_key));

        if (strcmp(meta.type, "float") == 0) {
            float val = 0.0f;
            if (nvs_helper::batch_read_float(h_ro, nvs_key, val)) {
                ext_state_->set(meta.key, val);
                ESP_LOGI(TAG, "Restored %s = %.2f (key: %s)",
                         meta.key, static_cast<double>(val), nvs_key);
                restored++;
            } else if (read_legacy && nvs_helper::batch_read_float(h_ro, legacy_key, val)) {
                // Міграція: legacy → новий ключ
                ext_state_->set(meta.key, val);
                begin_migration();
                nvs_helper::batch_write_float(h_rw, nvs_key, val);
                nvs_helper::batch_erase_key(h_rw, legacy_key);
                ESP_LOGI(TAG, "Migrated %s = %.2f (%s → %s)",
                         meta.key, static_cast<double>(val), legacy_key, nvs_key);
                restored++;
                migrated++;
            } else {
                ext_state_->set(meta.key, meta.default_val);
                ESP_LOGI(TAG, "Default %s = %.2f (not in NVS)",
                         meta.key, static_cast<double>(meta.default_val));
            }
        } else if (strcmp(meta.type, "int") == 0) {
            int32_t val = 0;
            if (nvs_helper::batch_read_i32(h_ro, nvs_key, val)) {
                ext_state_->set(meta.key, val);
                ESP_LOGI(TAG, "Restored %s = %ld (key: %s)",
                         meta.key, (long)val, nvs_key);
                restored++;
            } else if (read_legacy && nvs_helper::batch_read_i32(h_ro, legacy_key, val)) {
                ext_state_->set(meta.key, val);
                begin_migration();
                nvs_helper::batch_write_i32(h_rw, nvs_key, val);
                nvs_helper::batch_erase_key(h_rw, legacy_key);
                ESP_LOGI(TAG, "Migrated %s = %ld (%s → %s)",
                         meta.key, (long)val, legacy_key, nvs_key);
                restored++;
                migrated++;
            } else {
                ext_state_->set(meta.key, static_cast<int32_t>(meta.default_val));
            }
        } else if (strcmp(meta.type, "bool") == 0) {
            bool val = false;
            if (nvs_helper::batch_read_bool(h_ro, nvs_key, val)) {
                ext_state_->set(meta.key, val);
                ESP_LOGI(TAG, "Restored %s = %s (key: %s)",
                         meta.key, val ? "true" : "false", nvs_key);
                restored++;
            } else if (read_legacy && nvs_helper::batch_read_bool(h_ro, legacy_key, val)) {
                ext_state_->set(meta.key, val);
                begin_migration();
                nvs_helper::batch_write_bool(h_rw, nvs_key, val);
                nvs_helper::batch_erase_key(h_rw, legacy_key);
                ESP_LOGI(TAG, "Migrated %s = %s (%s → %s)",
                         meta.key, val ? "true" : "false", legacy_key, nvs_key);
                restored++;
                migrated++;
            } else {
                ext_state_->set(meta.key, meta.default_val != 0.0f);
            }
        }
    }

    if (legacy != LEGACY_RETIRED) {
        if (!h_rw) h_rw = nvs_helper::batch_open(NVS_NAMESPACE, false);
        const int erased = erase_legacy_keys(h_rw);
        const bool retired = nvs_helper::batch_write_i32(h_rw, LEGACY_MARK_KEY, LEGACY_RETIRED);
        // Once in a device's life, and the only record of what was found: WARN, so that a
        // quiet log level still shows it.
        ESP_LOGW(TAG, "Positional keys: %d migrated, %d stale erased, %s",
                 migrated, erased, retired ? "never read again" : "marker NOT written");
    }

    nvs_helper::batch_close(h_ro);
    if (h_rw) nvs_helper::batch_close(h_rw);

    ESP_LOGI(TAG, "Restored %d keys from NVS", restored);
    if (migrated > 0) {
        ESP_LOGI(TAG, "Migrated %d keys from legacy positional format", migrated);
    }
}

void PersistService::on_state_changed(const StateKey& key, const StateValue& value, void* user_data) {
    auto* self = static_cast<PersistService*>(user_data);
    (void)value;

    // Шукаємо ключ в STATE_META з persist==true
    for (size_t i = 0; i < gen::STATE_META_COUNT && i < MAX_PERSIST_KEYS; i++) {
        const auto& meta = gen::STATE_META[i];
        if (!meta.persist) continue;

        // Порівнюємо ключі
        if (key == StateKey(meta.key)) {
            self->dirty_[i] = true;
            // Note for on_update(), not a timer reset: this runs in the caller's context,
            // and the deferral has to stay bounded. See BUG-025 in the file header.
            self->changed_ = true;
            return;
        }
    }
}

void PersistService::on_update(uint32_t dt_ms) {
    // Перевіряємо чи є dirty keys
    bool has_dirty = false;
    for (size_t i = 0; i < gen::STATE_META_COUNT && i < MAX_PERSIST_KEYS; i++) {
        if (dirty_[i]) {
            has_dirty = true;
            break;
        }
    }

    if (!has_dirty) {
        defer_ms_ = 0;
        changed_ = false;
        return;
    }

    // A value that is still moving is not worth a flash cycle: restart the window so a knob
    // being turned costs one write — of where it stops — instead of one every DEBOUNCE_MS
    // all the way up. Bounded by MAX_DEFER_MS, because an unbounded restart is exactly what
    // stopped every key from ever being written (BUG-025).
    if (changed_) {
        changed_ = false;
        if (defer_ms_ < MAX_DEFER_MS) debounce_timer_ = 0;
    }
    debounce_timer_ += dt_ms;
    defer_ms_ += dt_ms;
    if (debounce_timer_ < DEBOUNCE_MS) return;

    flush_to_nvs();
    debounce_timer_ = 0;
    defer_ms_ = 0;
}

// BUG-014: примусовий запис — викликається перед restart
void PersistService::flush_now() {
    flush_to_nvs();
    debounce_timer_ = 0;
    defer_ms_ = 0;
}

void PersistService::flush_to_nvs() {
    if (!ext_state_) return;

    // Batch: один handle для всіх write операцій (зменшує heap фрагментацію)
    auto h = nvs_helper::batch_open(NVS_NAMESPACE, false);
    if (!h) return;

    int saved = 0;

    for (size_t i = 0; i < gen::STATE_META_COUNT && i < MAX_PERSIST_KEYS; i++) {
        if (!dirty_[i]) continue;
        dirty_[i] = false;

        const auto& meta = gen::STATE_META[i];
        if (!meta.persist) continue;

        auto val = ext_state_->get(meta.key);
        if (!val.has_value()) continue;

        // BUG-012: стабільний хеш-ключ замість позиційного
        char nvs_key[16];
        make_nvs_key(meta.key, nvs_key, sizeof(nvs_key));

        // Read before writing. NVS makes the same comparison internally and will not
        // touch flash for an unchanged value, so this is not where the wear is saved — it
        // is so the log below reports writes that actually happened. A "Saved" line per
        // second is alarming; a "Saved" line that means nothing is worse.
        bool ok = false;
        if (strcmp(meta.type, "float") == 0) {
            const auto* fp = etl::get_if<float>(&val.value());
            if (fp) {
                float cur = 0.0f;
                if (nvs_helper::batch_read_float(h, nvs_key, cur) && cur == *fp) continue;
                ok = nvs_helper::batch_write_float(h, nvs_key, *fp);
            } else {
                // BUG-023: fallback — int32_t → float конвертація
                const auto* ip = etl::get_if<int32_t>(&val.value());
                if (ip) ok = nvs_helper::batch_write_float(h, nvs_key, static_cast<float>(*ip));
            }
        } else if (strcmp(meta.type, "int") == 0) {
            const auto* ip = etl::get_if<int32_t>(&val.value());
            if (ip) {
                int32_t cur = 0;
                if (nvs_helper::batch_read_i32(h, nvs_key, cur) && cur == *ip) continue;
                ok = nvs_helper::batch_write_i32(h, nvs_key, *ip);
            } else {
                // BUG-023: fallback — float → int32_t конвертація
                const auto* fp = etl::get_if<float>(&val.value());
                if (fp) ok = nvs_helper::batch_write_i32(h, nvs_key, static_cast<int32_t>(*fp));
            }
        } else if (strcmp(meta.type, "bool") == 0) {
            const auto* bp = etl::get_if<bool>(&val.value());
            if (bp) {
                bool cur = false;
                if (nvs_helper::batch_read_bool(h, nvs_key, cur) && cur == *bp) continue;
                ok = nvs_helper::batch_write_bool(h, nvs_key, *bp);
            }
        }

        if (ok) {
            saved++;
            ESP_LOGI(TAG, "Saved %s to NVS (key: %s)", meta.key, nvs_key);
        } else {
            ESP_LOGW(TAG, "Failed to save %s to NVS", meta.key);
        }
    }

    // Один commit + close для всіх записів
    nvs_helper::batch_close(h);

    if (saved > 0) {
        ESP_LOGI(TAG, "Flushed %d keys to NVS", saved);
    }
}

} // namespace modesp
