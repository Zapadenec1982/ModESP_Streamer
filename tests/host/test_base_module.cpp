/**
 * @file test_base_module.cpp
 * @brief BaseModule's typed SharedState readers, and what they do with the wrong type.
 *
 * SharedState holds an etl::variant, and a reader that asks for the wrong alternative gets
 * nullptr from etl::get_if and quietly returns the caller's default. Nothing warns: the
 * writer and the reader are in different files, the types are chosen independently, and the
 * result is a value that is always exactly the default.
 *
 * That is not hypothetical. `audio.cpu_pct` and `audio.in_rate` are published as floats by
 * the audio module and were read with read_int by the panel, so the CPU indicator showed 0
 * on every build that ever shipped and the technical line under a Bluetooth track stayed
 * blank (plan 2026-09-21, item 12.4). read_number exists so a caller that wants a number
 * does not have to know which of the two numeric types the writer picked, and these cases
 * pin the conversion at the edges where it is easiest to get wrong.
 */

#include "doctest.h"

#include "modesp/base_module.h"
#include "modesp/module_manager.h"
#include "modesp/shared_state.h"

#include <cmath>
#include <limits>

namespace {

/// A module that does nothing but expose the protected readers to a test.
class Peek : public modesp::BaseModule {
public:
    Peek() : BaseModule("peek", modesp::ModulePriority::LOW) {}

    using BaseModule::read_bool;
    using BaseModule::read_float;
    using BaseModule::read_int;
    using BaseModule::read_number;
    using BaseModule::read_string;
    using BaseModule::state_set;
    using BaseModule::state_version;
};

struct Rig {
    modesp::SharedState   state;
    modesp::ModuleManager modules;
    Peek                  peek;

    Rig() {
        modules.register_module(peek);
        REQUIRE(modules.init_all(state));
    }
};

}  // namespace

TEST_CASE("read_int on a float key returns the default, which is the fault read_number fixes") {
    Rig rig;
    rig.peek.state_set("audio.cpu_pct", 47.0f);

    // The behaviour as it is, pinned deliberately: read_int is not broken, it is exact, and
    // a caller asking for an int32_t from a float variant is asking for something that is
    // not there. The bug was the call site, so the test that documents it lives here beside
    // the reader that replaced it.
    CHECK(rig.peek.read_int("audio.cpu_pct", -1) == -1);
    CHECK(rig.peek.read_number("audio.cpu_pct", -1) == 47);
}

TEST_CASE("read_number takes either numeric type and rounds rather than truncates") {
    Rig rig;

    rig.peek.state_set("t.i", (int32_t)42);
    CHECK(rig.peek.read_number("t.i") == 42);

    // Half a percent of CPU is not worth a digit, but the direction of the error is worth
    // pinning: a load of 49.6 % reported as 49 is a rounding choice made by accident.
    rig.peek.state_set("t.f", 49.6f);
    CHECK(rig.peek.read_number("t.f") == 50);
    rig.peek.state_set("t.f", 49.4f);
    CHECK(rig.peek.read_number("t.f") == 49);
    rig.peek.state_set("t.f", 0.5f);
    CHECK(rig.peek.read_number("t.f") == 1);

    // Negative values round away from zero, so -0.5 and 0.5 are the same distance from the
    // number they land on. audio.lim_gr is the negative float this store actually holds.
    rig.peek.state_set("t.f", -49.6f);
    CHECK(rig.peek.read_number("t.f") == -50);
    rig.peek.state_set("t.f", -0.5f);
    CHECK(rig.peek.read_number("t.f") == -1);
    rig.peek.state_set("t.f", -0.4f);
    CHECK(rig.peek.read_number("t.f") == 0);
}

TEST_CASE("read_number carries the rates and loads it was added for without loss") {
    Rig rig;
    const int32_t rates[] = {44100, 48000, 88200, 96000, 176400, 192000};
    for (int32_t hz : rates) {
        rig.peek.state_set("audio.in_rate", (float)hz);
        CHECK(rig.peek.read_number("audio.in_rate") == hz);
    }
    for (int32_t pct = 0; pct <= 100; pct++) {
        rig.peek.state_set("audio.cpu_pct", (float)pct);
        CHECK(rig.peek.read_number("audio.cpu_pct") == pct);
    }
}

TEST_CASE("a float that cannot be an int32_t returns the default instead of undefined behaviour") {
    Rig rig;

    // Converting an out-of-range float to an integer type is UB in C++, not a wrap: on this
    // toolchain it yields whatever the instruction happens to produce. There is no int32_t
    // that means "that was not a number", so the caller's default is the honest answer.
    rig.peek.state_set("t.f", 1e30f);
    CHECK(rig.peek.read_number("t.f", -7) == -7);
    rig.peek.state_set("t.f", -1e30f);
    CHECK(rig.peek.read_number("t.f", -7) == -7);
    rig.peek.state_set("t.f", std::numeric_limits<float>::infinity());
    CHECK(rig.peek.read_number("t.f", -7) == -7);
    rig.peek.state_set("t.f", -std::numeric_limits<float>::infinity());
    CHECK(rig.peek.read_number("t.f", -7) == -7);
    rig.peek.state_set("t.f", std::numeric_limits<float>::quiet_NaN());
    CHECK(rig.peek.read_number("t.f", -7) == -7);

    // The largest magnitude that still fits is kept, so the guard cannot be "anything big
    // is refused".
    rig.peek.state_set("t.f", 2147483520.0f);
    CHECK(rig.peek.read_number("t.f", -7) == 2147483520);
}

TEST_CASE("read_number refuses the types that are not numbers, and the keys that are absent") {
    Rig rig;

    rig.peek.state_set("t.b", true);
    CHECK(rig.peek.read_number("t.b", -7) == -7);
    rig.peek.state_set("t.s", "192000");
    CHECK(rig.peek.read_number("t.s", -7) == -7);
    CHECK(rig.peek.read_number("t.missing", -7) == -7);
}

TEST_CASE("state_version moves with a tracked change, and only with one") {
    Rig rig;
    const uint32_t before = rig.peek.state_version();
    rig.peek.state_set("t.v", (int32_t)1);
    const uint32_t after = rig.peek.state_version();
    CHECK(after != before);
    CHECK(after != 0);

    // The same value again is no change, and a quiet set is not seen at all: a module that
    // skips its reads on an unmoved version has to have every key it skips written tracked.
    rig.peek.state_set("t.v", (int32_t)1);
    CHECK(rig.peek.state_version() == after);
    rig.peek.state_set("t.v", (int32_t)2, false);
    CHECK(rig.peek.state_version() == after);
    rig.peek.state_set("t.v", (int32_t)3);
    CHECK(rig.peek.state_version() != after);

    // A module that is not registered has no store: 0, which a reader takes as "changed".
    Peek loose;
    CHECK(loose.state_version() == 0);
}
