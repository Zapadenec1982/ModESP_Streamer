/**
 * @file test_watchdog_service.cpp
 * @brief WatchdogService and ErrorService together: a stalled task is one error.
 *
 * The watchdog publishes MsgModuleTimeout, and ErrorService makes a report out of it. The
 * watchdog used to call report() itself as well, so every stall counted twice in
 * error.count, the history and the MsgSystemError deliveries.
 */

#include "doctest.h"

#include "esp_timer.h"
#include "modesp/error_service.h"
#include "modesp/liveness.h"
#include "modesp/module_manager.h"
#include "modesp/shared_state.h"
#include "modesp/watchdog_service.h"

#include <string>

namespace {

/// Counts the MsgSystemError deliveries.
class Listener : public modesp::BaseModule {
public:
    Listener() : BaseModule("listener", modesp::ModulePriority::LOW) {}
    void on_message(const etl::imessage& msg) override {
        if (msg.get_message_id() == modesp::msg_id::SYSTEM_ERROR) errors++;
    }
    int errors = 0;
};

/// An empty liveness table and a host clock at 0, before and after the case.
struct Clean {
    Clean()  { wipe(); }
    ~Clean() { wipe(); }
    static void wipe() {
        host_timer_us() = 0;
        for (size_t i = 0; i < modesp::liveness::MAX_ENTRIES; i++)
            if (modesp::liveness::Entry* e = modesp::liveness::entry(i))
                modesp::liveness::unwatch(e->name);
    }
};

std::string string_key(const modesp::SharedState& s, const char* key) {
    auto v = s.get(key);
    REQUIRE(v.has_value());
    const auto* p = etl::get_if<modesp::StringValue>(&v.value());
    REQUIRE(p != nullptr);
    return p->c_str();
}

} // namespace

TEST_CASE("a task that stops making progress is reported once") {
    Clean clean;
    modesp::SharedState     state;
    modesp::ModuleManager   modules;
    modesp::ErrorService    errors;
    modesp::WatchdogService watchdog(errors, modules);
    Listener                listener;
    modules.register_module(errors);
    modules.register_module(watchdog);
    modules.register_module(listener);
    REQUIRE(modules.init_all(state));

    static modesp::liveness::Counter beat{0};
    REQUIRE(modesp::liveness::watch("stuck_task", &beat, 3000));

    modules.update_all(1000);                 // the watchdog's first look: where the counter stands
    host_timer_us() = 4000 * 1000;            // four seconds on, and the counter has not moved
    modules.update_all(1000);

    CHECK(string_key(state, "system.stalled") == "stuck_task");
    CHECK(errors.error_count() == 1);
    CHECK(errors.history().size() == 1);

    host_timer_us() = 6000 * 1000;            // still stuck: no second report of the same stall
    modules.update_all(1000);
    modules.update_all(1000);
    CHECK(errors.error_count() == 1);
    CHECK(listener.errors == 1);
}
