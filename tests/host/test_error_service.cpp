/**
 * @file test_error_service.cpp
 * @brief ErrorService::report() from any task; the bus only from the main loop.
 *
 * report() used to run ModuleManager::publish() synchronously — every module's on_message()
 * on whatever task had something to say — with no lock around the record or the counter.
 * These cases pin the contract that replaced it: any thread may report, nothing reaches the
 * bus until on_update() runs, and nothing is lost on the way.
 */

#include "doctest.h"

#include "modesp/error_service.h"
#include "modesp/module_manager.h"
#include "modesp/shared_state.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace {

/// A module that counts what the bus delivers to it.
class Listener : public modesp::BaseModule {
public:
    Listener() : BaseModule("listener", modesp::ModulePriority::LOW) {}

    void on_message(const etl::imessage& msg) override {
        if (msg.get_message_id() == modesp::msg_id::SYSTEM_ERROR) {
            errors++;
            last_source = static_cast<const modesp::MsgSystemError&>(msg).source.c_str();
        } else if (msg.get_message_id() == modesp::msg_id::SYSTEM_SAFE_MODE) {
            safe_mode++;
            reason = static_cast<const modesp::MsgSafeMode&>(msg).reason.c_str();
        }
    }

    int errors = 0;
    int safe_mode = 0;
    std::string last_source;
    std::string reason;
};

struct Rig {
    modesp::SharedState   state;
    modesp::ModuleManager modules;
    modesp::ErrorService  errors;
    Listener              listener;

    Rig() {
        modules.register_module(errors);
        modules.register_module(listener);
        REQUIRE(modules.init_all(state));
    }
};

int32_t as_int(const etl::optional<modesp::StateValue>& v) {
    REQUIRE(v.has_value());
    const auto* p = etl::get_if<int32_t>(&v.value());
    REQUIRE(p != nullptr);
    return *p;
}

int32_t int_key(const modesp::SharedState& s, const char* key) {
    return as_int(s.get(key));
}

/// SharedState's change callback: every value error.count takes, in order.
void record_count(const modesp::StateKey& key, const modesp::StateValue& value, void* user) {
    if (key != modesp::StateKey("error.count")) return;
    if (const auto* p = etl::get_if<int32_t>(&value)) {
        static_cast<std::vector<int32_t>*>(user)->push_back(*p);
    }
}

/// Holds a thread at the first critical section it leaves, until resumed: the preemption a
/// loaded machine produces now and then, on demand. Only a thread that called here() stops.
/// Both waits are bounded, so a design that makes another reporter wait on the held one
/// fails a check instead of hanging the suite.
struct Park {
    std::atomic<bool> parked{false};
    std::atomic<bool> resume{false};
    std::atomic<bool> timed_out{false};

    Park()  { current = this; host_after_exit_critical.store(&hook); }
    ~Park() { host_after_exit_critical.store(nullptr); current = nullptr; }

    static void here() { armed = true; }

    bool wait() const {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!parked.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        return parked.load();
    }

private:
    static inline Park* current = nullptr;
    static inline thread_local bool armed = false;

    static void hook() {
        if (!armed) return;
        armed = false;
        Park& p = *current;
        p.parked.store(true);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!p.resume.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        p.timed_out.store(!p.resume.load());
    }
};

} // namespace

TEST_CASE("a report is recorded at once but reaches the bus only from the main loop") {
    Rig rig;

    rig.errors.report("audio", -7, modesp::ErrorSeverity::ERROR, "underrun");
    CHECK(rig.errors.error_count() == 1);
    CHECK(int_key(rig.state, "error.count") == 1);
    CHECK(rig.errors.history().size() == 1);
    CHECK(rig.listener.errors == 0);           // not yet: no on_update has run

    rig.modules.update_all(10);
    CHECK(rig.listener.errors == 1);
    CHECK(rig.listener.last_source == "audio");

    rig.modules.update_all(10);
    CHECK(rig.listener.errors == 1);           // delivered once
}

TEST_CASE("INFO is recorded and counted but never put on the bus") {
    Rig rig;
    rig.errors.report("sys", 0, modesp::ErrorSeverity::INFO, "note");
    rig.modules.update_all(10);
    CHECK(rig.errors.error_count() == 1);
    CHECK(rig.listener.errors == 0);
}

TEST_CASE("FATAL enters safe mode immediately and publishes MsgSafeMode after the errors") {
    Rig rig;
    rig.errors.report("config", -1, modesp::ErrorSeverity::FATAL, "board.json unusable");
    CHECK(rig.errors.is_safe_mode());
    CHECK(rig.listener.safe_mode == 0);

    rig.modules.update_all(10);
    CHECK(rig.listener.errors == 1);
    CHECK(rig.listener.safe_mode == 1);
    CHECK(rig.listener.reason == "board.json unusable");

    // A second FATAL does not re-enter safe mode.
    rig.errors.report("config", -2, modesp::ErrorSeverity::FATAL, "again");
    rig.modules.update_all(10);
    CHECK(rig.listener.safe_mode == 1);
    CHECK(rig.listener.errors == 2);
}

TEST_CASE("a burst deeper than the queue keeps every record and counts the lost deliveries") {
    Rig rig;
    const int burst = MODESP_ERROR_QUEUE_DEPTH + 3;
    for (int i = 0; i < burst; ++i) {
        rig.errors.report("net", i, modesp::ErrorSeverity::WARNING, "burst");
    }
    CHECK(rig.errors.error_count() == (size_t)burst);
    CHECK(rig.errors.dropped_messages() == 3);

    rig.modules.update_all(10);
    CHECK(rig.listener.errors == MODESP_ERROR_QUEUE_DEPTH);

    // The queue is empty again: the next report goes through.
    rig.errors.report("net", 99, modesp::ErrorSeverity::WARNING, "after");
    rig.modules.update_all(10);
    CHECK(rig.listener.errors == MODESP_ERROR_QUEUE_DEPTH + 1);
    CHECK(rig.errors.dropped_messages() == 3);
}

TEST_CASE("reports from several threads are all counted, and the bus still runs on the caller of update_all") {
    Rig rig;
    constexpr int threads = 4;
    static constexpr int per_thread = 50;   // static: read inside the lambdas without a capture on every compiler

    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t) {
        pool.emplace_back([&rig, t] {
            for (int i = 0; i < per_thread; ++i) {
                rig.errors.report("worker", t * 1000 + i, modesp::ErrorSeverity::INFO, "tick");
            }
        });
    }
    for (auto& th : pool) th.join();

    CHECK(rig.errors.error_count() == (size_t)(threads * per_thread));
    CHECK(int_key(rig.state, "error.count") == threads * per_thread);
    CHECK(rig.listener.errors == 0);   // INFO never queues; nothing ran on the workers
}

TEST_CASE("a reporter held up between its count and the store cannot leave a smaller count published") {
    // The interleaving behind a 195 == 200 in the case above (seen under a firmware build),
    // made to happen every time: A counts the first error and is preempted as it leaves the
    // critical section, B reports the second from start to finish, A runs on. A used to write
    // the count it left with, 1, over B's 2, and the key stayed one short.
    std::vector<int32_t> published;   // before rig: the store keeps a pointer to it
    Rig rig;
    rig.state.set_persist_callback(record_count, &published);

    Park park;
    std::thread a([&rig] {
        Park::here();
        rig.errors.report("held", 1, modesp::ErrorSeverity::INFO, "first");
    });
    CHECK(park.wait());

    rig.errors.report("quick", 2, modesp::ErrorSeverity::INFO, "second");
    const auto meanwhile = rig.state.get("error.count");   // A is still held

    park.resume.store(true);
    a.join();

    CHECK_FALSE(park.timed_out.load());       // B never waited for A
    CHECK(as_int(meanwhile) == 2);            // B's report() returned with its error published
    CHECK(rig.errors.error_count() == 2);
    CHECK(int_key(rig.state, "error.count") == 2);
    CHECK(std::is_sorted(published.begin(), published.end()));   // never went back, not even for a moment
}

TEST_CASE("a module reporting from inside on_message does not deadlock or recurse into the bus") {
    class Reporter : public modesp::BaseModule {
    public:
        explicit Reporter(modesp::ErrorService& e) : BaseModule("reporter"), errors_(e) {}
        void on_message(const etl::imessage& msg) override {
            if (msg.get_message_id() == modesp::msg_id::SYSTEM_ERROR && !done_) {
                done_ = true;
                errors_.report("reporter", 1, modesp::ErrorSeverity::WARNING, "reaction");
            }
        }
    private:
        modesp::ErrorService& errors_;
        bool done_ = false;
    };

    modesp::SharedState   state;
    modesp::ModuleManager modules;
    modesp::ErrorService  errors;
    Listener              listener;
    Reporter              reporter(errors);
    modules.register_module(errors);
    modules.register_module(listener);
    modules.register_module(reporter);
    REQUIRE(modules.init_all(state));

    errors.report("origin", 0, modesp::ErrorSeverity::WARNING, "first");
    modules.update_all(10);
    // The reaction was queued during the drain and delivered in the same pass.
    CHECK(listener.errors == 2);
    CHECK(listener.last_source == "reporter");
}
