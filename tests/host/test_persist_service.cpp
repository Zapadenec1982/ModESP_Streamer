/**
 * @file test_persist_service.cpp
 * @brief PersistService at boot: what it restores, and where it may still look for the
 *        positional "p<i>" keys of the format before BUG-012.
 *
 * radio.cat_quality, a state added to the radio manifest with default 1, came up on the panel
 * as 3. A positional key left by an older firmware sat at its index in STATE_META, and the
 * migration — which ran on every boot, for every persisted state without a hashed key — took
 * it, stored it under the hashed key and erased the original. So the cases here are the devices
 * that boot meets:
 *
 *   - a device that has run the hashed format, with positional leftovers: none of them may
 *     reach a state, not even on the first boot of this code
 *   - a device from before the hashed format: migrated once, then never again
 *   - a fresh device: nothing to migrate, the marker is the only write
 *   - a power cut at any write of that first boot: the next boot ends where an
 *     uninterrupted one would have
 *   - every boot after: nothing is written at all
 *
 * NVS underneath is mocks/nvs.h, an in-memory store with IDF v5.5.5's type and same-value
 * rules. The states are the real generated table, picked by type at run time, so a manifest
 * change cannot quietly turn a case into one that tests nothing.
 */

#include "doctest.h"

#include "modesp/services/persist_service.h"
#include "modesp/shared_state.h"
#include "nvs.h"
#include "state_meta.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>

using modesp::gen::StateMeta;

namespace {

const char* const NS = "persist";
// The marker's NVS name and values are on-device format: renaming the key runs the pass
// again on every device. Spelled out here so that a change to them is a deliberate one.
const char* const MARK = "legacy_keys";
constexpr int32_t MIGRATING = 1;
constexpr int32_t RETIRED   = 2;

/// The key PersistService stores a state under: djb2 of its name, "s" + 7 hex digits. The
/// first test pins it, because changing it orphans every value every device has stored.
std::string hashed(const char* state_key) {
    uint32_t h = 5381;
    for (const char* p = state_key; *p; p++) h = ((h << 5) + h) + static_cast<unsigned char>(*p);
    char out[16];
    snprintf(out, sizeof(out), "s%07lx", (unsigned long)(h & 0x0FFFFFFFul));
    return out;
}

std::string positional(size_t index) { return "p" + std::to_string(index); }

/// The `nth` persisted state of `type` in the generated table, and its index there.
struct Pick {
    const StateMeta* meta = nullptr;
    size_t index = 0;
};
Pick persisted(const char* type, int nth = 0) {
    for (size_t i = 0; i < modesp::gen::STATE_META_COUNT; i++) {
        const auto& m = modesp::gen::STATE_META[i];
        if (m.persist && strcmp(m.type, type) == 0 && nth-- == 0) return {&m, i};
    }
    return {};
}

int32_t def_i32(const Pick& p) { return static_cast<int32_t>(p.meta->default_val); }
bool    def_bool(const Pick& p) { return p.meta->default_val != 0.0f; }
/// Not the default, and inside the range where the range allows it.
int32_t other_i32(const Pick& p) {
    const int32_t d = def_i32(p);
    return d + 1 <= static_cast<int32_t>(p.meta->max_val) ? d + 1 : d - 1;
}

// What an older firmware left behind, written through the NVS API so the store's type
// rules apply to the fixtures too.
void put_i32(const std::string& key, int32_t v) {
    nvs_handle_t h = 0;
    REQUIRE(nvs_open(NS, NVS_READWRITE, &h) == ESP_OK);
    REQUIRE(nvs_set_i32(h, key.c_str(), v) == ESP_OK);
    nvs_close(h);
}
void put_u8(const std::string& key, uint8_t v) {
    nvs_handle_t h = 0;
    REQUIRE(nvs_open(NS, NVS_READWRITE, &h) == ESP_OK);
    REQUIRE(nvs_set_u8(h, key.c_str(), v) == ESP_OK);
    nvs_close(h);
}

bool stored(const std::string& key) { return host_nvs::snapshot(NS).count(key) != 0; }

/// The i32 stored under `key`; INT32_MIN when there is none.
int32_t stored_i32(const std::string& key) {
    nvs_handle_t h = 0;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return INT32_MIN;
    int32_t v = INT32_MIN;
    if (nvs_get_i32(h, key.c_str(), &v) != ESP_OK) v = INT32_MIN;
    nvs_close(h);
    return v;
}

int positional_keys_left() {
    int n = 0;
    for (const auto& kv : host_nvs::snapshot(NS)) {
        const std::string& k = kv.first;
        if (k.size() > 1 && k[0] == 'p' && k.find_first_not_of("0123456789", 1) == std::string::npos) n++;
    }
    return n;
}

/// One boot: a new SharedState and PersistService over the NVS the last boot left.
struct Boot {
    modesp::SharedState    state;
    modesp::PersistService persist;

    Boot() {
        persist.set_state(&state);
        REQUIRE(persist.on_init());
    }
    int32_t i32(const Pick& p) const {
        auto v = state.get(p.meta->key);
        REQUIRE(v.has_value());
        const auto* ip = etl::get_if<int32_t>(&v.value());
        REQUIRE(ip != nullptr);
        return *ip;
    }
    bool boolean(const Pick& p) const {
        auto v = state.get(p.meta->key);
        REQUIRE(v.has_value());
        const auto* bp = etl::get_if<bool>(&v.value());
        REQUIRE(bp != nullptr);
        return *bp;
    }
};

/// The states the cases use. Three ints and a bool, all persisted.
struct Cast {
    Pick a = persisted("int", 0);
    Pick b = persisted("int", 1);
    Pick d = persisted("int", 2);
    Pick c = persisted("bool", 0);

    Cast() {
        REQUIRE_MESSAGE(a.meta, "the generated table has no persisted int state");
        REQUIRE_MESSAGE(d.meta, "the generated table has fewer than three persisted ints");
        REQUIRE_MESSAGE(c.meta, "the generated table has no persisted bool state");
    }
};

/// A device that has run the hashed format: `a` was changed once and has its hashed key.
/// Positional leftovers sit at the index of `b` and `c` — states still at their default, so
/// without a hashed key, which is exactly where radio.cat_quality was — and past the end of
/// this table, as a longer one would have left them.
void seed_hashed_device(const Cast& s) {
    put_i32(hashed(s.a.meta->key), other_i32(s.a));
    put_i32(positional(s.b.index), other_i32(s.b));
    put_u8(positional(s.c.index), def_bool(s.c) ? 0 : 1);
    put_i32(positional(modesp::gen::STATE_META_COUNT + 3), 7);
}

/// A device from before the hashed format: positional keys only. `d`'s is of another type
/// — how stale keys outlived the old migration — and one lies past the end of the table.
void seed_positional_device(const Cast& s) {
    put_i32(positional(s.a.index), other_i32(s.a));
    put_i32(positional(s.b.index), other_i32(s.b));
    put_u8(positional(s.c.index), def_bool(s.c) ? 0 : 1);
    put_u8(positional(s.d.index), 1);
    put_i32(positional(modesp::gen::STATE_META_COUNT + 3), 7);
}

} // namespace

TEST_CASE("persist: the hashed key is the one every device already stores under") {
    // djb2 over the state key, computed outside this code. make_nvs_key() must keep
    // producing these: a different key is a device that forgot all its settings.
    CHECK(hashed("radio.cat_quality") == "s9b8c362");
    CHECK(hashed("audio.volume") == "s43bd2bd");

    // And PersistService reads what is stored there.
    Cast s;
    host_nvs::reset();
    put_i32(hashed(s.a.meta->key), other_i32(s.a));
    put_i32(MARK, RETIRED);
    Boot boot;
    CHECK(boot.i32(s.a) == other_i32(s.a));
}

TEST_CASE("persist: a device that has run the hashed format takes no positional value, "
          "not even on this code's first boot") {
    Cast s;
    host_nvs::reset();
    seed_hashed_device(s);

    {
        Boot first;
        CHECK(first.i32(s.a) == other_i32(s.a));          // its own value, from its hashed key
        CHECK(first.i32(s.b) == def_i32(s.b));            // the cat_quality case: default, not p<i>
        CHECK(first.boolean(s.c) == def_bool(s.c));
        CHECK_FALSE(stored(hashed(s.b.meta->key)));       // and nothing stale was persisted
        CHECK_FALSE(stored(hashed(s.c.meta->key)));
        CHECK(positional_keys_left() == 0);               // the leftovers are gone
        CHECK(stored_i32(MARK) == RETIRED);
    }

    const long before = host_nvs::writes();
    Boot second;
    CHECK(host_nvs::writes() == before);                  // a settled boot writes nothing
    CHECK(second.i32(s.a) == other_i32(s.a));
    CHECK(second.i32(s.b) == def_i32(s.b));
}

TEST_CASE("persist: a device from before the hashed format is migrated once, and never again") {
    Cast s;
    host_nvs::reset();
    seed_positional_device(s);

    {
        Boot first;
        CHECK(first.i32(s.a) == other_i32(s.a));
        CHECK(first.i32(s.b) == other_i32(s.b));
        CHECK(first.boolean(s.c) == !def_bool(s.c));
        CHECK(first.i32(s.d) == def_i32(s.d));            // a u8 under an int's index is not its value
        CHECK(stored_i32(hashed(s.a.meta->key)) == other_i32(s.a));
        CHECK(stored_i32(hashed(s.b.meta->key)) == other_i32(s.b));
        CHECK(stored(hashed(s.c.meta->key)));
        CHECK_FALSE(stored(hashed(s.d.meta->key)));
        CHECK(positional_keys_left() == 0);               // migrated or not, every p<n> is gone
        CHECK(stored_i32(MARK) == RETIRED);
    }

    long before = host_nvs::writes();
    {
        Boot second;                                      // values now come from hashed keys
        CHECK(host_nvs::writes() == before);
        CHECK(second.i32(s.a) == other_i32(s.a));
        CHECK(second.i32(s.b) == other_i32(s.b));
        CHECK(second.boolean(s.c) == !def_bool(s.c));
    }

    // A positional key that turns up after the pass — a downgrade and back, say — is never
    // read: `d` has no hashed key, and this is the shape of the original bug.
    put_i32(positional(s.d.index), other_i32(s.d));
    before = host_nvs::writes();
    Boot third;
    CHECK(third.i32(s.d) == def_i32(s.d));
    CHECK(host_nvs::writes() == before);
}

TEST_CASE("persist: a fresh device writes the marker and nothing else") {
    Cast s;
    host_nvs::reset();

    {
        Boot first;
        CHECK(first.i32(s.a) == def_i32(s.a));
        CHECK(first.boolean(s.c) == def_bool(s.c));
        const auto ns = host_nvs::snapshot(NS);
        CHECK(ns.size() == 1);
        CHECK(stored_i32(MARK) == RETIRED);
    }

    const long before = host_nvs::writes();
    Boot second;
    CHECK(host_nvs::writes() == before);
}

TEST_CASE("persist: a migration cut short is resumed, not taken for finished") {
    Cast s;

    // What a power cut halfway through the positional device's first boot leaves: the
    // marker says a migration started, `a` already has its hashed key, `b` is still
    // positional. The hashed key alone would read as "this device ran the hashed format".
    host_nvs::reset();
    put_i32(MARK, MIGRATING);
    put_i32(hashed(s.a.meta->key), other_i32(s.a));
    put_i32(positional(s.b.index), other_i32(s.b));
    {
        Boot boot;
        CHECK(boot.i32(s.a) == other_i32(s.a));
        CHECK(boot.i32(s.b) == other_i32(s.b));           // migrated, because the marker said so
        CHECK(stored_i32(hashed(s.b.meta->key)) == other_i32(s.b));
        CHECK(positional_keys_left() == 0);
        CHECK(stored_i32(MARK) == RETIRED);
    }

    // The same store without the marker is a device that has run the hashed format, and
    // then `b`'s positional key is a leftover.
    host_nvs::reset();
    put_i32(hashed(s.a.meta->key), other_i32(s.a));
    put_i32(positional(s.b.index), other_i32(s.b));
    Boot boot;
    CHECK(boot.i32(s.b) == def_i32(s.b));
    CHECK(positional_keys_left() == 0);
}

TEST_CASE("persist: a power cut at any write of the first boot ends where no cut would have") {
    Cast s;

    struct Fixture {
        const char* name;
        std::function<void(const Cast&)> seed;
        bool migrates;   ///< the first boot takes positional values
    };
    const Fixture fixtures[] = {
        {"positional device", seed_positional_device, true},
        {"hashed device with leftovers", seed_hashed_device, false},
    };

    for (const auto& fx : fixtures) {
        CAPTURE(fx.name);

        // The uninterrupted first boot, and how many writes it makes.
        host_nvs::reset();
        fx.seed(s);
        const long w0 = host_nvs::writes();
        int32_t ref_a = 0, ref_b = 0;
        bool    ref_c = false;
        {
            Boot ref;
            ref_a = ref.i32(s.a);
            ref_b = ref.i32(s.b);
            ref_c = ref.boolean(s.c);
        }
        const long total = host_nvs::writes() - w0;
        const host_nvs::Namespace ref_ns = host_nvs::snapshot(NS);
        REQUIRE(total > 0);

        for (long k = 0; k < total; k++) {
            CAPTURE(k);
            host_nvs::reset();
            fx.seed(s);
            host_nvs::power_cut_after(k);
            {
                Boot cut;
                // Whatever got written, a device that has run the hashed format never
                // shows a stale value, not even on the boot that was cut.
                if (!fx.migrates) CHECK(cut.i32(s.b) == def_i32(s.b));
            }
            host_nvs::power_cut_after(-1);

            Boot next;
            CHECK(host_nvs::snapshot(NS) == ref_ns);
            CHECK(next.i32(s.a) == ref_a);
            CHECK(next.i32(s.b) == ref_b);
            CHECK(next.boolean(s.c) == ref_c);
        }
    }
}
