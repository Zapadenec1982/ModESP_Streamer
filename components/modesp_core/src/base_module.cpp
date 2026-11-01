/**
 * @file base_module.cpp
 * @brief Реалізація BaseModule — делегування до ModuleManager/SharedState
 */

#include "modesp/base_module.h"
#include "modesp/module_manager.h"
#include "modesp/shared_state.h"

#include "esp_log.h"
#include <cstring>

namespace modesp {

bool BaseModule::post_command(const char* key, int32_t value) {
    // Resolve to the generated table's literal first: `key` may point into an HTTP request
    // buffer that is gone by the time on_update() runs, and the table also says which module
    // the key belongs to, so a command cannot be posted to the wrong one.
    const auto* cmd = gen::find_state_command(key);
    if (!cmd) return false;
    if (strcmp(cmd->module, name_) != 0) return false;

    bool queued = false;
    portENTER_CRITICAL(&commands_mux_);
    if (!commands_.full()) {
        commands_.push(Command{cmd->key, value});
        queued = true;
    } else {
        commands_dropped_++;
    }
    portEXIT_CRITICAL(&commands_mux_);

    if (!queued) {
        ESP_LOGW("Module", "%s: command queue full, dropped %s=%ld (%lu so far)",
                 name_, cmd->key, (long)value, (unsigned long)commands_dropped_);
    }
    return queued;
}

bool BaseModule::next_command(Command& out) {
    bool got = false;
    portENTER_CRITICAL(&commands_mux_);
    if (!commands_.empty()) {
        out = commands_.front();
        commands_.pop();
        got = true;
    }
    portEXIT_CRITICAL(&commands_mux_);
    return got;
}

size_t BaseModule::pending_commands() const {
    portENTER_CRITICAL(&commands_mux_);
    const size_t n = commands_.size();
    portEXIT_CRITICAL(&commands_mux_);
    return n;
}

bool BaseModule::post_command_to(const char* module_name, const char* key, int32_t value) {
    if (!manager_) return false;
    BaseModule* m = manager_->find(module_name);
    return m && m->post_command(key, value);
}

bool BaseModule::read_string(const char* key, char* out, size_t out_sz) const {
    if (!out || out_sz == 0) return false;
    out[0] = 0;
    if (!shared_state_) return false;
    return shared_state_->get_string(key, out, out_sz);
}

uint32_t BaseModule::state_version() const {
    return shared_state_ ? shared_state_->version() : 0;
}

void BaseModule::publish(const etl::imessage& msg) {
    if (manager_) {
        manager_->publish(msg);
    }
}

bool BaseModule::state_set(const StateKey& key, const StateValue& value, bool track_change) {
    if (shared_state_) {
        return shared_state_->set(key, value, track_change);
    }
    return false;
}

bool BaseModule::state_set(const char* key, int32_t value, bool track_change) {
    return shared_state_ ? shared_state_->set(key, value, track_change) : false;
}

bool BaseModule::state_set(const char* key, float value, bool track_change) {
    return shared_state_ ? shared_state_->set(key, value, track_change) : false;
}

bool BaseModule::state_set(const char* key, bool value, bool track_change) {
    return shared_state_ ? shared_state_->set(key, value, track_change) : false;
}

bool BaseModule::state_set(const char* key, const char* value, bool track_change) {
    return shared_state_ ? shared_state_->set(key, value, track_change) : false;
}

etl::optional<StateValue> BaseModule::state_get(const StateKey& key) const {
    if (shared_state_) {
        return shared_state_->get(key);
    }
    return etl::nullopt;
}

etl::optional<StateValue> BaseModule::state_get(const char* key) const {
    return shared_state_ ? shared_state_->get(key) : etl::nullopt;
}

} // namespace modesp
