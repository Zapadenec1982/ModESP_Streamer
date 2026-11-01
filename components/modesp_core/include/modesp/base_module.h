/**
 * @file base_module.h
 * @brief Базовий клас для всіх модулів ModESP v4
 *
 * Кожен модуль (драйвер, сервіс, бізнес-логіка) наслідує BaseModule.
 * Модуль отримує доступ до:
 *   - publish()      → відправити повідомлення на шину
 *   - state_set/get  → читати/писати SharedState
 *   - on_init()      → ініціалізація (викликається один раз)
 *   - on_update()    → періодичне оновлення (100Hz)
 *   - on_message()   → обробка вхідних повідомлень
 *   - on_stop()      → graceful shutdown
 *
 * Модуль НЕ знає про App, ModuleManager, або Message Bus напряму.
 * Зв'язок встановлюється через ModuleManager при реєстрації.
 */

#pragma once

#include "modesp/types.h"
#include "features_config.h"
#include "state_meta.h"          // gen::find_state_command — which keys are commands
#include "etl/message.h"
#include "etl/optional.h"
#include "etl/queue.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace modesp {

// Forward declarations — модуль не знає деталей реалізації
class ModuleManager;
class SharedState;
// Binding-контекст живе в modesp_hal — core лишається hal-agnostic
// (лише посилання у сигнатурі on_bind, без include).
class DriverManager;
class HAL;
struct BindingTable;

class BaseModule {
public:
    BaseModule(const char* name, ModulePriority priority = ModulePriority::NORMAL)
        : name_(name)
        , priority_(priority)
    {}

    virtual ~BaseModule() = default;

    // ── Lifecycle (перевизначаються модулем) ──
    virtual bool on_init()                            { return true; }
    virtual void on_update(uint32_t dt_ms)            {}
    virtual void on_message(const etl::imessage& msg) {}
    virtual void on_stop()                            {}

    /// Викликається ModuleManager::bind_all() ОДИН раз між реєстрацією та
    /// init_all (Phase 2), коли HAL і драйвери вже ініціалізовані. Модуль
    /// із залізом резолвить тут свої драйвери/бекенди за bindings.json —
    /// main.cpp не мусить знати імен модулів. Модулі без заліза ігнорують.
    virtual void on_bind(DriverManager& drivers,
                         const BindingTable& bindings,
                         HAL& hal) {}

    // ── Ідентифікація ──
    const char*    name()     const { return name_; }
    ModulePriority priority() const { return priority_; }

    // ── Стан модуля ──
    enum class State : uint8_t {
        CREATED,
        INITIALIZED,
        RUNNING,
        STOPPED,
        ERROR
    };
    State state() const { return state_; }

    // ── Heartbeat (оновлюється автоматично ModuleManager) ──
    uint32_t last_update_ms() const { return last_update_ms_; }

    // ── Commands ────────────────────────────────────────────────────────────────────
    //
    // A command is an EVENT: "next track", "stop". It was being carried as a SharedState
    // key — the sender wrote player.cmd = 4 and the module polled it and wrote 0 back —
    // and a shared variable is the wrong shape for an event three times over. Two presses
    // between two polls are one press, because the second overwrote the first. The reset to
    // zero is a second write that every subscriber sees, so a button push went out over the
    // WebSocket and MQTT twice, as "4" and then as "0". And a value that only means
    // something for one tick sat in the state map for ever, taking a slot and appearing in
    // GET /api/state as a number nobody can interpret.
    //
    // A queue per module fixes all three: nothing is lost up to its depth, nothing is
    // broadcast, and nothing is stored. The keys stay in the manifests, because that is
    // what the web UI's buttons and the MQTT topics are built from — `"command": true`
    // there is what routes the write here instead of into the store.
    //
    // 8 deep: a person cannot press a transport button eight times inside one 10 ms tick,
    // and a control point that sends more than that in a burst is not being ignored quietly
    // (post_command returns false and the caller says so).
    struct Command {
        const char* key   = nullptr;  ///< canonical literal from gen::STATE_COMMANDS
        int32_t     value = 0;
    };
    static constexpr size_t COMMAND_QUEUE_DEPTH = 8;

    /// Post a command to this module. `key` is matched against the generated command table,
    /// so a caller may pass a pointer into a request buffer; what is queued is the table's
    /// own literal. Safe from any task. False when the key is not one of this module's
    /// commands, or the queue is full.
    bool post_command(const char* key, int32_t value);

    /// How many commands are waiting. For tests and diagnostics.
    size_t pending_commands() const;

protected:
    // ── API для модулів (доступний після реєстрації) ──
    
    // Відправити повідомлення на шину (всім підписникам)
    void publish(const etl::imessage& msg);

    // Записати значення в SharedState
    /// @param track_change false = тихий set (не тригерить WS broadcast)
    bool state_set(const StateKey& key, const StateValue& value, bool track_change = true);

    // Зручні обгортки для state_set
    bool state_set(const char* key, int32_t value, bool track_change = true);
    bool state_set(const char* key, float value, bool track_change = true);
    bool state_set(const char* key, bool value, bool track_change = true);
    bool state_set(const char* key, const char* value, bool track_change = true);

    // Прочитати значення з SharedState
    etl::optional<StateValue> state_get(const StateKey& key) const;
    etl::optional<StateValue> state_get(const char* key) const;

    // BUG-013: типізовані reader-и (замість дублювання в кожному модулі)
    float read_float(const char* key, float def = 0.0f) const {
        auto v = state_get(key);
        if (!v.has_value()) return def;
        const auto* fp = etl::get_if<float>(&v.value());
        return fp ? *fp : def;
    }
    bool read_bool(const char* key, bool def = false) const {
        auto v = state_get(key);
        if (!v.has_value()) return def;
        const auto* bp = etl::get_if<bool>(&v.value());
        return bp ? *bp : def;
    }
    int32_t read_int(const char* key, int32_t def = 0) const {
        auto v = state_get(key);
        if (!v.has_value()) return def;
        const auto* ip = etl::get_if<int32_t>(&v.value());
        return ip ? *ip : def;
    }
    /// A whole number out of a key that may be stored as either int or float.
    ///
    /// read_int() is etl::get_if<int32_t>, which on a variant currently holding a float
    /// hands back nullptr — so the caller gets its default, silently, for ever. That is a
    /// type mismatch between two files and nothing warns about it: `audio.cpu_pct` and
    /// `audio.in_rate` are published as floats (audio_module.cpp), read with read_int by
    /// the panel, and the CPU indicator therefore showed 0 on every build that ever
    /// shipped. Whoever writes the key decides the type; whoever reads a NUMBER should not
    /// have to know which one was picked.
    ///
    /// Rounded rather than truncated: a 49.6 % load shown as 50 is a display choice, shown
    /// as 49 is a rounding error, and shown as 0 is a bug report. Out of range — including
    /// NaN, which fails the first comparison — returns `def`, because int32_t cannot say
    /// "that was not a number" and a converted-out-of-range float is undefined behaviour.
    int32_t read_number(const char* key, int32_t def = 0) const {
        auto v = state_get(key);
        if (!v.has_value()) return def;
        if (const auto* ip = etl::get_if<int32_t>(&v.value())) return *ip;
        const auto* fp = etl::get_if<float>(&v.value());
        if (!fp) return def;
        const float f = *fp;
        // 2^31 is not representable as a float bound, so the nearest float below it is the
        // honest one. The negated form catches NaN with the same test.
        constexpr float LIMIT = 2147483520.0f;
        if (!(f >= -LIMIT && f <= LIMIT)) return def;
        return (int32_t)(f >= 0.0f ? f + 0.5f : f - 0.5f);
    }
    // Copy a string state value into out (zero-heap). Returns false (out left "") if the
    // key is missing or not a string. out is always NUL-terminated.
    //
    // Goes through SharedState::get_string() rather than get(): a StateValue carries only
    // the inline string, and for a key the manifest marks `long` (a path, a URI) the whole
    // text lives in the side table. Reading one of those through get() would hand back a
    // prefix, which is a file that cannot be opened.
    bool read_string(const char* key, char* out, size_t out_sz) const;

    /// The store's change count (SharedState::version()): it moves with every tracked
    /// state_set() that changed a value, so a module that reads its settings on every pass
    /// can skip the reads while it stands still. A key written with track_change = false does
    /// not move it. 0 when the store could not be read in time: take that as "changed".
    uint32_t state_version() const;

    /// Take the next command, oldest first. False when there is none. Call it in a loop
    /// from on_update(): the point of the queue is that a burst is not collapsed into one.
    bool next_command(Command& out);

    /// Post to ANOTHER module by name, for the senders that are modules themselves — the
    /// screen, the MQTT client, the DLNA renderer stopping the stream engine. False if no
    /// such module is registered, the key is not one of its commands, or its queue is full.
    bool post_command_to(const char* module_name, const char* key, int32_t value);

    // Перевірити чи feature активна (з features_config.h)
    bool has_feature(const char* feature_name) const {
        return modesp::gen::is_feature_active(name(), feature_name);
    }

private:
    friend class ModuleManager;  // ModuleManager встановлює зв'язки

    const char*    name_;
    ModulePriority priority_;
    State          state_ = State::CREATED;
    volatile uint32_t last_update_ms_ = 0;

    // Зворотні посилання (встановлює ModuleManager при реєстрації)
    ModuleManager* manager_ = nullptr;
    SharedState*   shared_state_ = nullptr;

    // Posted from the HTTP, MQTT and BLE tasks; drained on main. A spinlock rather than a
    // mutex because both sides are a handful of instructions and neither may block.
    etl::queue<Command, COMMAND_QUEUE_DEPTH> commands_;
    mutable portMUX_TYPE commands_mux_ = portMUX_INITIALIZER_UNLOCKED;
    uint32_t commands_dropped_ = 0;
};

} // namespace modesp
