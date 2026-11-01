/**
 * @file config_service.h
 * @brief Configuration service — mounts LittleFS, parses board.json and bindings.json
 *
 * ConfigService runs at boot (CRITICAL priority, before HAL).
 * It reads JSON config files from a LittleFS partition and fills
 * BoardConfig + BindingTable structs used by HAL and DriverManager.
 *
 * All JSON parsing uses jsmn with buffers that live only for the parse — zero stack.
 *
 * The parse is strict on purpose: a capacity overflow, a file that does not fit the parse
 * buffer, or a malformed section fails the load, and because config is a CRITICAL module the
 * boot goes to safe mode instead of coming up half-configured. A partially configured board
 * that boots as if it were healthy can cost a flashed panel.
 */

#pragma once

#include "modesp/base_module.h"
#include "modesp/hal/hal_types.h"

namespace modesp {

class ConfigService : public BaseModule {
public:
    ConfigService() : BaseModule("config", ModulePriority::CRITICAL) {}

    bool on_init() override;

    const BoardConfig&  board_config()  const { return board_config_; }
    const BindingTable& binding_table() const { return binding_table_; }

    // JSON parsing limits — ONE copy of each number. The boot parser sizes its buffers and
    // bounds fread()/jsmn_parse() with them, POST /api/bindings caps its body with
    // MAX_JSON_SIZE, and the root CMakeLists.txt reads MAX_JSON_SIZE out of this header to
    // refuse a board.json that would not fit. A file that fills the buffer is rejected as
    // too big (see read_json_file in the .cpp), never parsed as if it were whole.
    // KC868-A6 board.json ~2.3 KB, ~240 jsmn tokens; the P4 panel ~3.6 KB.
    static constexpr size_t MAX_JSON_SIZE = 4096;
    static constexpr size_t MAX_TOKENS    = 512;

    /// Parse <dir>/board.json, <user_dir>/devices.json (optional) and <dir>/bindings.json
    /// into the live tables. false = something is wrong with the configuration and the tables
    /// must not be trusted.
    ///
    /// Two directories because two kinds of file. board.json and bindings.json are build
    /// output and live in the image partition; devices.json is a registry someone builds at
    /// runtime by subscribing devices, and that lives in the `userdata` partition a flash
    /// does not touch (UserStore). on_init() passes both; host tests pass one fixture
    /// directory, which then serves for both.
    bool load(const char* dir, const char* user_dir = nullptr);

    /// Parse one board.json / bindings.json into a caller-owned table, without touching the
    /// live ones. POST /api/bindings validates a candidate file with exactly the parser boot
    /// will use before it replaces /data/bindings.json; host tests point these at fixtures.
    static bool load_board_file(const char* path, BoardConfig& board);
    static bool load_bindings_file(const char* path, BindingTable& table);

private:
    BoardConfig  board_config_;
    BindingTable binding_table_;

    bool mount_littlefs();
    static bool parse_board_json(const char* path, BoardConfig& board);
    static bool parse_devices_json(const char* path, BoardConfig& board);
    static bool parse_bindings_json(const char* path, BindingTable& table);
};

} // namespace modesp
