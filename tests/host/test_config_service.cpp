/**
 * @file test_config_service.cpp
 * @brief The board/bindings parser, against the files it will actually meet.
 *
 * config is a CRITICAL module: what its parser accepts, the board boots with, and what it
 * rejects sends the board to safe mode. Both sides matter. The fixtures are the real
 * boards/p4_player files, the bindings fixtures the Python suite already validates, and the
 * broken copies under tools/tests/fixtures/config/ — truncated, over capacity, over size —
 * each of which used to be accepted with a shrug.
 */

#include "doctest.h"

#include "modesp/services/config_service.h"

#include <string>

namespace {

const std::string kRoot     = MODESP_ROOT;
const std::string kBoard    = kRoot + "/boards/p4_player";
const std::string kFixtures = kRoot + "/tools/tests/fixtures";
const std::string kBroken   = kFixtures + "/config";

// BoardConfig is ~9 KB and BindingTable ~13 KB — static, not on the test's stack.
modesp::BoardConfig  g_board;
modesp::BindingTable g_table;

} // namespace

TEST_CASE("the real p4_player board and bindings load, and the numbers match the files") {
    modesp::ConfigService cfg;
    REQUIRE(cfg.load(kBoard.c_str()));

    const auto& b = cfg.board_config();
    CHECK(std::string(b.board_name.c_str()) == "p4_player");
    CHECK(std::string(b.board_version.c_str()) == "0.4");

    REQUIRE(b.i2c_buses.size() == 1);
    CHECK(b.i2c_buses[0].sda == 7);
    CHECK(b.i2c_buses[0].scl == 8);
    CHECK(b.i2c_buses[0].freq_hz == 400000u);

    REQUIRE(b.dsi_displays.size() == 1);
    CHECK(std::string(b.dsi_displays[0].chip.c_str()) == "st7701");
    CHECK(b.dsi_displays[0].lanes == 2);
    CHECK(b.dsi_displays[0].v_res == 800);
    CHECK(b.dsi_displays[0].rotation == 270);   // microphone up
    CHECK(b.dsi_displays[0].ldo_chan == 3);
    CHECK(b.dsi_displays[0].ldo_mv == 2500);

    REQUIRE(b.sdmmc_slots.size() == 1);
    CHECK(b.sdmmc_slots[0].width == 4);
    CHECK(b.sdmmc_slots[0].d0 == 39);
    CHECK(b.sdmmc_slots[0].cd == -1);
    CHECK(std::string(b.sdmmc_slots[0].mount_point.c_str()) == "/sd");

    REQUIRE(b.i2c_touch.size() == 1);
    CHECK(b.i2c_touch[0].address == 0x5D);
    CHECK(b.i2c_touch[0].x_max == 800);

    REQUIRE(b.encoders.size() == 1);
    CHECK(b.encoders[0].clk == 32);
    CHECK(b.encoders[0].dt == 28);
    CHECK(b.encoders[0].pull_up);

    REQUIRE(b.gpio_inputs.size() == 1);
    CHECK(b.gpio_inputs[0].gpio == 35);
    CHECK(b.gpio_inputs[0].invert);

    REQUIRE(b.gpio_outputs.size() == 1);
    CHECK(b.gpio_outputs[0].gpio == 34);
    CHECK(b.gpio_outputs[0].active_high);

    REQUIRE(b.i2s_buses.size() == 2);
    CHECK(b.i2s_buses[0].master);
    CHECK(b.i2s_buses[0].mclk == 49);
    CHECK(b.i2s_buses[0].din == 29);
    CHECK(b.i2s_buses[0].slot_bits == 32);
    CHECK(b.i2s_buses[0].sample_rate_hz == 48000u);
    CHECK_FALSE(b.i2s_buses[1].master);
    CHECK(b.i2s_buses[1].port == 1);
    CHECK(b.i2s_buses[1].mclk == -1);

    REQUIRE(b.uart_buses.size() == 1);
    CHECK(b.uart_buses[0].baud_rate == 460800u);
    CHECK(b.uart_buses[0].rx_buffer == 8192);   // 178 ms of wire at 460800 (uart_link_driver.cpp)

    CHECK(b.remote_devices.empty());

    const auto& t = cfg.binding_table();
    REQUIRE(t.bindings.size() == 9);
    CHECK(std::string(t.bindings[4].hardware_id.c_str()) == "i2s_a");
    CHECK(std::string(t.bindings[4].driver_type.c_str()) == "i2s_std");
    CHECK(std::string(t.bindings[4].role.c_str()) == "port_main");
    CHECK(std::string(t.bindings[4].module_name.c_str()) == "audio");
    CHECK(t.bindings[4].address.empty());
    CHECK(t.bindings[4].settings.empty());

    // The last two rows say which UART the satellite is on and which slot holds the card,
    // which used to be the literals "uart_sat" and "sd_0" inside modules/bt and
    // modules/storage. Whether or not they name a driver, hardware_for() must
    // still answer (module, role) -> hardware id: that is what a module asks when it needs
    // a board resource it is not given a driver for, such as the audio module's BT port.
    char hw[modesp::MAX_HAL_ID_LEN + 1] = "";
    CHECK(t.hardware_for("bt", "sat_link", hw, sizeof hw));
    CHECK(std::string(hw) == "uart_sat");
    CHECK(t.hardware_for("storage", "card", hw, sizeof hw));
    CHECK(std::string(hw) == "sd_0");
    // The BT module asks for the audio module's Bluetooth port by role rather than by name.
    CHECK(t.hardware_for("audio", "port_bt", hw, sizeof hw));
    CHECK(std::string(hw) == "i2s_b");
    // A miss empties the buffer instead of leaving the previous answer in it.
    CHECK_FALSE(t.hardware_for("storage", "nothing", hw, sizeof hw));
    CHECK(hw[0] == 0);
    CHECK_FALSE(t.hardware_for("nobody", "card", hw, sizeof hw));

    // The card slot names a driver of its own: drivers/sdmmc_card owns the
    // controller, the power rail and the FAT mount, and modules/storage asks the registry
    // for it in on_bind() like every other module-bound backend. Not "sdmmc": a driver
    // folder is an IDF component and that name is ESP-IDF's own SD/MMC protocol component,
    // which a driver of that name shadows — the build then fails inside esp_hosted.
    // The satellite's UART names drivers/uart_link for the same split: the wire is the
    // driver's, and what is said over it is modules/bt's. Between them, this board has no
    // driver-less binding at all - so that case is pinned by its own fixture below, not by
    // this file.
    for (const auto& b : t.bindings) {
        INFO("role: ", b.role.c_str());
        if (b.role == "card") {
            CHECK(std::string(b.hardware_id.c_str()) == "sd_0");
            CHECK(std::string(b.driver_type.c_str()) == "sdmmc_card");
        }
        if (b.role == "sat_link") {
            CHECK(std::string(b.hardware_id.c_str()) == "uart_sat");
            CHECK(std::string(b.driver_type.c_str()) == "uart_link");
        }
    }
}

TEST_CASE("a bindings file with addresses and per-binding settings parses them") {
    REQUIRE(modesp::ConfigService::load_bindings_file((kFixtures + "/bindings_full.json").c_str(), g_table));
    REQUIRE(g_table.bindings.size() == 8);
    CHECK(std::string(g_table.bindings[1].address.c_str()) == "28:FF:AA:BB:CC:DD:EE:01");
    CHECK(std::string(g_table.bindings[7].role.c_str()) == "door_contact");

    REQUIRE(modesp::ConfigService::load_bindings_file((kBroken + "/bindings_settings.json").c_str(), g_table));
    REQUIRE(g_table.bindings.size() == 2);
    CHECK(g_table.bindings[0].setting_or("beta", 0.0f) == doctest::Approx(3900.0f));
    CHECK(g_table.bindings[0].setting_or("offset", 0.0f) == doctest::Approx(0.5f));
    CHECK(g_table.bindings[0].setting_or("missing", -1.0f) == doctest::Approx(-1.0f));
    CHECK(g_table.bindings[1].settings.empty());   // the settings object did not desync the walk
    CHECK(std::string(g_table.bindings[1].role.c_str()) == "second");
}

TEST_CASE("a binding with no 'driver' parses as a plain hardware assignment") {
    // Such a row says which board resource plays a role for a module, and the module then
    // uses the HAL resource directly - DriverManager creates nothing for it and must not warn
    // about it either. boards/p4_player had two of these until both were given a driver,
    // which is why this lives in a fixture now: the parser still has to accept
    // the missing field, and hardware_for() still has to answer for it.
    REQUIRE(modesp::ConfigService::load_bindings_file(
        (kBroken + "/bindings_assignment.json").c_str(), g_table));
    REQUIRE(g_table.bindings.size() == 3);
    CHECK(std::string(g_table.bindings[0].driver_type.c_str()) == "relay");
    CHECK(g_table.bindings[1].driver_type.empty());
    CHECK(g_table.bindings[2].driver_type.empty());

    char hw[modesp::MAX_HAL_ID_LEN + 1] = "";
    CHECK(g_table.hardware_for("bt", "sat_link", hw, sizeof hw));
    CHECK(std::string(hw) == "uart_1");
    CHECK(g_table.hardware_for("storage", "card", hw, sizeof hw));
    CHECK(std::string(hw) == "sd_0");
}

TEST_CASE("loading twice does not accumulate") {
    modesp::ConfigService cfg;
    REQUIRE(cfg.load(kBoard.c_str()));
    REQUIRE(cfg.load(kBoard.c_str()));
    CHECK(cfg.board_config().i2s_buses.size() == 2);
    CHECK(cfg.binding_table().bindings.size() == 9);
}

TEST_CASE("a truncated board.json is refused") {
    CHECK_FALSE(modesp::ConfigService::load_board_file((kBroken + "/board_truncated.json").c_str(), g_board));
}

TEST_CASE("a board.json bigger than the parse buffer is refused, not parsed truncated") {
    CHECK_FALSE(modesp::ConfigService::load_board_file((kBroken + "/board_oversize.json").c_str(), g_board));
}

TEST_CASE("a panel entry that leaves out a timing is refused, not defaulted") {
    // hal_types.h used to carry the JC4880P443C's numbers as struct defaults, so
    // an entry missing a line parsed fine and the panel was driven at ANOTHER panel's
    // timings — a rolling or torn picture with nothing in the log naming the missing field.
    // The fixture omits 'vfp' and nothing else.
    CHECK_FALSE(modesp::ConfigService::load_board_file(
        (kBroken + "/board_panel_incomplete.json").c_str(), g_board));
    // Same reasoning for the touch controller's I2C address: 0x5D is how ADDR-low is
    // strapped, and a board that strapped it high answers at 0x14.
    CHECK_FALSE(modesp::ConfigService::load_board_file(
        (kBroken + "/board_touch_no_address.json").c_str(), g_board));
}

TEST_CASE("more entries than a table holds is an error, not a silent drop") {
    CHECK_FALSE(modesp::ConfigService::load_board_file((kBroken + "/board_overcapacity.json").c_str(), g_board));
    CHECK_FALSE(modesp::ConfigService::load_bindings_file((kBroken + "/bindings_overcapacity.json").c_str(), g_table));
    CHECK_FALSE(modesp::ConfigService::load_bindings_file((kBroken + "/bindings_settings_overcapacity.json").c_str(), g_table));
}

TEST_CASE("a member with no value is refused, not walked past the parsed tokens") {
    // Non-strict jsmn counts {"beta" 3900} as a 2-member object holding 2 tokens, not 4, and
    // {"hardware"} as an object with one member. Unchecked, the walk read tokens past ntokens
    // and returned true with a setting or a hardware id made of whatever lay there.
    CHECK_FALSE(modesp::ConfigService::load_bindings_file((kBroken + "/bindings_settings_no_colon.json").c_str(), g_table));
    CHECK_FALSE(modesp::ConfigService::load_bindings_file((kBroken + "/bindings_member_no_value.json").c_str(), g_table));
    // {"bindings"}: one token read past the end, its type taken for the array's. What lay
    // there decided the answer, so this one only failed when the heap happened to hold an array.
    CHECK_FALSE(modesp::ConfigService::load_bindings_file((kBroken + "/bindings_key_only.json").c_str(), g_table));
}

TEST_CASE("a bindings file without a bindings array is refused; an empty array is fine") {
    CHECK_FALSE(modesp::ConfigService::load_bindings_file((kBroken + "/bindings_no_array.json").c_str(), g_table));
    REQUIRE(modesp::ConfigService::load_bindings_file((kBroken + "/bindings_empty.json").c_str(), g_table));
    CHECK(g_table.bindings.empty());
}

TEST_CASE("a missing file is refused") {
    CHECK_FALSE(modesp::ConfigService::load_board_file((kBroken + "/does_not_exist.json").c_str(), g_board));
    modesp::ConfigService cfg;
    CHECK_FALSE(cfg.load((kBroken + "/no_such_dir").c_str()));
    CHECK_FALSE(cfg.load(kBroken.c_str()));   // has broken files, no bindings.json at its root
}

TEST_CASE("devices.json merges into the board's remote devices, runtime row wins by id") {
    modesp::ConfigService cfg;
    REQUIRE(cfg.load((kBroken + "/with_devices").c_str()));
    const auto& devs = cfg.board_config().remote_devices;
    REQUIRE(devs.size() == 2);
    CHECK(std::string(devs[0].id.c_str()) == "sensor_a");
    CHECK(std::string(devs[0].identity.c_str()) == "a4:c1:38:00:00:02");   // overridden by devices.json
    CHECK(std::string(devs[0].transport.c_str()) == "ble");
    CHECK(std::string(devs[1].id.c_str()) == "panel_b");
    CHECK(std::string(devs[1].name.c_str()) == "PANEL-B");
    CHECK(cfg.binding_table().bindings.size() == 1);
}

TEST_CASE("a devices.json that breaks off is read as far as it holds, never past its tokens") {
    // Non-strict jsmn counts a member with no value towards its object's size; the walk read
    // the previous file's tokens behind it. devices.json is user data (POST /api/devices, a
    // restored backup), and a bad one must not keep the board from booting.
    {
        modesp::ConfigService cfg;                              // {"devices"}
        REQUIRE(cfg.load((kBroken + "/devices_key_only").c_str()));
        const auto& devs = cfg.board_config().remote_devices;
        REQUIRE(devs.size() == 1);                              // board.json's, untouched
        CHECK(std::string(devs[0].identity.c_str()) == "a4:c1:38:00:00:01");
    }
    {
        modesp::ConfigService cfg;                              // a good row, then {"identity"}
        REQUIRE(cfg.load((kBroken + "/devices_broken_row").c_str()));
        const auto& devs = cfg.board_config().remote_devices;
        REQUIRE(devs.size() == 2);
        CHECK(std::string(devs[0].id.c_str()) == "sensor_a");
        CHECK(std::string(devs[0].identity.c_str()) == "a4:c1:38:00:00:01");   // not overridden
        CHECK(std::string(devs[1].id.c_str()) == "panel_b");                    // read before it
        CHECK(std::string(devs[1].name.c_str()) == "PANEL-B");
    }
}
