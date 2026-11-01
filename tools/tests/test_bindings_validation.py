"""Tests for validate_bindings() / unused_drivers() in generate_ui.py.

Mirrors test_inputs_validation.py: factory-function fixtures returning plain
dicts, call the free function with plain lists, assert on the lists.
"""
import sys
from pathlib import Path

TOOLS_DIR = Path(__file__).parent.parent
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

from generate_ui import validate_bindings, unused_drivers


# ── Fixtures ──────────────────────────────────────────────────────────

def _drivers():
    """All driver manifests keyed by name (mirrors real drivers/)."""
    return {
        "ds18b20":       {"driver": "ds18b20", "category": "sensor",
                          "hardware_type": "onewire_bus",
                          "requires_address": False, "multiple_per_bus": True,
                          "discovery": {"supported": True}},
        "relay":         {"driver": "relay", "category": "actuator",
                          "hardware_type": "gpio_output",
                          "requires_address": False, "multiple_per_bus": False},
        "ntc":           {"driver": "ntc", "category": "sensor",
                          "hardware_type": "adc_channel",
                          "requires_address": False, "multiple_per_bus": False},
        "pcf8574_relay": {"driver": "pcf8574_relay", "category": "actuator",
                          "hardware_type": "i2c_expander_output",
                          "requires_address": False, "multiple_per_bus": True},
        "pcf8574_input": {"driver": "pcf8574_input", "category": "sensor",
                          "hardware_type": "i2c_expander_input",
                          "requires_address": False, "multiple_per_bus": True},
        "digital_input": {"driver": "digital_input", "category": "sensor",
                          "hardware_type": "gpio_input",
                          "requires_address": False, "multiple_per_bus": False},
        "ld2410b":       {"driver": "ld2410b", "category": "sensor",
                          "hardware_type": "uart_bus",
                          "requires_address": False, "multiple_per_bus": True},
        # synthetic driver to exercise requires_address:true (invariant f)
        "addr_sensor":   {"driver": "addr_sensor", "category": "sensor",
                          "hardware_type": "onewire_bus",
                          "requires_address": True, "multiple_per_bus": True},
        # the two module-bound families that are neither sensor nor actuator
        "sdmmc_card":    {"driver": "sdmmc_card", "category": "storage",
                          "hardware_type": "sdmmc_slot",
                          "requires_address": False, "multiple_per_bus": False},
        "uart_link":     {"driver": "uart_link", "category": "link",
                          "hardware_type": "uart_bus",
                          "requires_address": False, "multiple_per_bus": False},
    }


def _dev_board():
    return {
        "board": "template_dev_v1",
        "gpio_outputs": [{"id": "relay_1"}, {"id": "relay_2"},
                         {"id": "relay_3"}, {"id": "relay_4"}],
        "onewire_buses": [{"id": "ow_1"}],
        "gpio_inputs": [{"id": "din_1"}],
        "adc_channels": [{"id": "adc_1"}, {"id": "adc_2"}],
    }


def _kc868a6_board():
    return {
        "board": "kc868_a6",
        "i2c_buses": [{"id": "i2c_0"}],
        "expander_outputs": [{"id": f"relay_{i}"} for i in range(1, 7)],
        "expander_inputs":  [{"id": f"din_{i}"} for i in range(1, 7)],
        "onewire_buses": [{"id": "ow_1"}, {"id": "ow_2"}],
        "adc_channels": [{"id": f"adc_{i}"} for i in range(1, 5)],
    }


def _uart_board():
    return {
        "board": "uart_test",
        "uart_buses": [{"id": "uart_1"}, {"id": "uart_2"}],
        "onewire_buses": [{"id": "ow_1"}],
    }


def _player_board():
    """The shape boards/p4_player has: a card slot and a satellite UART."""
    return {
        "board": "p4_player",
        "sdmmc_slots": [{"id": "sd_0"}],
        "uart_buses": [{"id": "uart_sat"}],
        "gpio_outputs": [{"id": "amp_enable"}],
    }


def _b(rows):
    return {"bindings": rows}


def _run(board, bindings, drivers=None):
    errors, warnings = [], []
    validate_bindings(board, bindings, drivers or _drivers(), errors, warnings)
    return errors, warnings


# ── Valid real boards ─────────────────────────────────────────────────

class TestValidBoards:
    def test_dev_board_valid(self):
        b = _b([
            {"hardware": "ow_1", "driver": "ds18b20", "role": "air_temp", "module": "equipment"},
            {"hardware": "relay_1", "driver": "relay", "role": "actuator_1", "module": "equipment"},
        ])
        errors, _ = _run(_dev_board(), b)
        assert errors == []

    def test_kc868a6_valid(self):
        b = _b([
            {"hardware": "relay_1", "driver": "pcf8574_relay", "role": "compressor", "module": "equipment"},
            {"hardware": "relay_2", "driver": "pcf8574_relay", "role": "evap_fan", "module": "equipment"},
            {"hardware": "ow_1", "driver": "ds18b20", "role": "air_temp", "module": "equipment",
             "address": "28:8C:5E:45:D4:08:44:09"},
            {"hardware": "ow_1", "driver": "ds18b20", "role": "evap_temp", "module": "equipment",
             "address": "28:40:0A:45:D4:72:7E:F0"},
            {"hardware": "din_1", "driver": "pcf8574_input", "role": "door_contact", "module": "equipment"},
        ])
        errors, _ = _run(_kc868a6_board(), b)
        assert errors == []


# ── Invariants ────────────────────────────────────────────────────────

class TestInvariants:
    def test_a_board_none_skips(self):
        errors, warnings = _run(None, _b([{"hardware": "x", "driver": "y", "role": "r", "module": "m"}]))
        assert errors == [] and warnings == []

    def test_a_bindings_none_skips(self):
        errors, _ = _run(_dev_board(), None)
        assert errors == []

    def test_b_board_duplicate_id(self):
        board = _dev_board()
        board["gpio_inputs"] = [{"id": "relay_1"}]  # collides with gpio_outputs
        errors, _ = _run(board, _b([]))
        assert any("duplicate hardware id 'relay_1'" in e for e in errors)

    def test_c_hardware_not_in_board(self):
        b = _b([{"hardware": "ow_99", "driver": "ds18b20", "role": "air_temp", "module": "equipment"}])
        errors, _ = _run(_dev_board(), b)
        assert any("ow_99" in e and "not in board" in e for e in errors)

    def test_d_driver_no_manifest(self):
        b = _b([{"hardware": "ow_1", "driver": "nonesuch", "role": "air_temp", "module": "equipment"}])
        errors, _ = _run(_dev_board(), b)
        assert any("nonesuch" in e and "no manifest" in e for e in errors)

    def test_e_hardware_type_mismatch(self):
        # ntc (adc_channel) bound to ow_1 (onewire_bus)
        b = _b([{"hardware": "ow_1", "driver": "ntc", "role": "air_temp", "module": "equipment"}])
        errors, _ = _run(_dev_board(), b)
        assert any("hardware_type" in e and "ntc" in e for e in errors)

    def test_f_no_address_ok_when_not_required(self):
        b = _b([{"hardware": "ow_1", "driver": "ds18b20", "role": "air_temp", "module": "equipment"}])
        errors, _ = _run(_dev_board(), b)
        assert errors == []

    def test_f_address_required(self):
        b = _b([{"hardware": "ow_1", "driver": "addr_sensor", "role": "air_temp", "module": "equipment"}])
        errors, _ = _run(_dev_board(), b)
        assert any("requires an 'address'" in e for e in errors)

    def test_g_reuse_single_per_bus(self):
        b = _b([
            {"hardware": "relay_1", "driver": "relay", "role": "a", "module": "equipment"},
            {"hardware": "relay_1", "driver": "relay", "role": "b", "module": "equipment"},
        ])
        errors, _ = _run(_dev_board(), b)
        assert any("relay_1" in e and "multiple_per_bus" in e for e in errors)

    def test_g_reuse_multi_with_addresses_ok(self):
        b = _b([
            {"hardware": "ow_1", "driver": "ds18b20", "role": "a", "module": "equipment", "address": "AA"},
            {"hardware": "ow_1", "driver": "ds18b20", "role": "b", "module": "equipment", "address": "BB"},
        ])
        errors, _ = _run(_dev_board(), b)
        assert errors == []

    def test_g_reuse_multi_missing_address(self):
        b = _b([
            {"hardware": "ow_1", "driver": "ds18b20", "role": "a", "module": "equipment", "address": "AA"},
            {"hardware": "ow_1", "driver": "ds18b20", "role": "b", "module": "equipment"},
        ])
        errors, _ = _run(_dev_board(), b)
        assert any("distinct 'address'" in e for e in errors)

    def test_g_reuse_multi_dup_address(self):
        b = _b([
            {"hardware": "ow_1", "driver": "ds18b20", "role": "a", "module": "equipment", "address": "AA"},
            {"hardware": "ow_1", "driver": "ds18b20", "role": "b", "module": "equipment", "address": "AA"},
        ])
        errors, _ = _run(_dev_board(), b)
        assert any("duplicate addresses" in e for e in errors)

    def test_h_duplicate_role_in_module(self):
        b = _b([
            {"hardware": "relay_1", "driver": "relay", "role": "compressor", "module": "equipment"},
            {"hardware": "relay_2", "driver": "relay", "role": "compressor", "module": "equipment"},
        ])
        errors, _ = _run(_dev_board(), b)
        assert any("duplicate role 'compressor'" in e for e in errors)

    def test_i_missing_required_fields(self):
        b = _b([{"hardware": "ow_1"}])  # no driver/role/module
        errors, _ = _run(_dev_board(), b)
        assert any("missing required field" in e for e in errors)

    def test_i_driver_is_not_required(self):
        # A row with no 'driver' is a plain hardware ASSIGNMENT, and legal.
        # DriverManager creates nothing for it; the module uses the HAL resource directly.
        b = _b([{"hardware": "ow_1", "role": "air_temp", "module": "equipment"}])
        errors, _ = _run(_dev_board(), b)
        assert errors == []


# -- storage and link drivers bind like any other ----------------------

class TestModuleBoundFamilies:
    def test_card_and_link_bind(self):
        b = _b([
            {"hardware": "sd_0", "driver": "sdmmc_card", "role": "card", "module": "storage"},
            {"hardware": "uart_sat", "driver": "uart_link", "role": "sat_link", "module": "bt"},
        ])
        errors, _ = _run(_player_board(), b)
        assert errors == []

    def test_card_driver_on_a_uart_is_a_type_mismatch(self):
        b = _b([{"hardware": "uart_sat", "driver": "sdmmc_card", "role": "card", "module": "storage"}])
        errors, _ = _run(_player_board(), b)
        assert any("hardware_type" in e and "sdmmc_card" in e for e in errors)

    def test_driver_the_build_does_not_have(self):
        # A binding naming a driver that is not in drivers/ - a typo, or a board file from a
        # build that had it. An ERROR, because a mis-wired binding must not reach the
        # firmware as a silent runtime skip.
        b = _b([{"hardware": "sd_0", "driver": "sd_spi", "role": "card", "module": "storage"}])
        errors, _ = _run(_player_board(), b)
        assert any("sd_spi" in e and "no manifest" in e for e in errors)


# ── UART bus / LD2410b multi-channel ──────────────────────────────────

class TestUartBus:
    def test_single_presence_binding_valid(self):
        # No address → presence channel; requires_address is False → OK.
        b = _b([
            {"hardware": "uart_1", "driver": "ld2410b", "role": "presence", "module": "equipment"},
        ])
        errors, _ = _run(_uart_board(), b)
        assert errors == []

    def test_multi_channel_distinct_addresses_valid(self):
        # One physical sensor (one UART) feeding several roles via address channels.
        b = _b([
            {"hardware": "uart_1", "driver": "ld2410b", "role": "presence",
             "module": "equipment", "address": "presence"},
            {"hardware": "uart_1", "driver": "ld2410b", "role": "move_dist",
             "module": "equipment", "address": "moving"},
            {"hardware": "uart_1", "driver": "ld2410b", "role": "still_dist",
             "module": "equipment", "address": "static"},
        ])
        errors, _ = _run(_uart_board(), b)
        assert errors == []

    def test_multi_channel_missing_address(self):
        # multiple_per_bus reuse requires a distinct address on each binding.
        b = _b([
            {"hardware": "uart_1", "driver": "ld2410b", "role": "a",
             "module": "equipment", "address": "presence"},
            {"hardware": "uart_1", "driver": "ld2410b", "role": "b", "module": "equipment"},
        ])
        errors, _ = _run(_uart_board(), b)
        assert any("distinct 'address'" in e for e in errors)

    def test_hardware_type_mismatch(self):
        # ld2410b (uart_bus) bound to ow_1 (onewire_bus).
        b = _b([{"hardware": "ow_1", "driver": "ld2410b", "role": "presence", "module": "equipment"}])
        errors, _ = _run(_uart_board(), b)
        assert any("hardware_type" in e and "ld2410b" in e for e in errors)


# ── unused_drivers helper ─────────────────────────────────────────────

class TestUnusedDrivers:
    def test_dev_unused_excludes_discovery(self):
        b = _b([
            {"hardware": "ow_1", "driver": "ds18b20", "role": "air_temp", "module": "equipment"},
            {"hardware": "relay_1", "driver": "relay", "role": "actuator_1", "module": "equipment"},
        ])
        result = unused_drivers(b, _drivers())
        # ds18b20 + relay are used; ds18b20 is discovery anyway → never suggested
        assert "ds18b20" not in result
        assert "relay" not in result
        assert "ntc" in result and "digital_input" in result and "pcf8574_relay" in result

    def test_bindings_none_returns_empty(self):
        assert unused_drivers(None, _drivers()) == []


# ── (i2) Orphaned bindings: binding → module absent from project.json ─

class TestOrphanedBindings:
    def _run_with_project(self, board, bindings, project_modules):
        errors, warnings = [], []
        validate_bindings(board, bindings, _drivers(), errors, warnings,
                          project_modules=project_modules)
        return errors, warnings

    def test_binding_to_missing_module_is_error(self):
        b = _b([
            {"hardware": "ow_1", "driver": "ds18b20", "role": "air_temp", "module": "equipment"},
            {"hardware": "relay_1", "driver": "relay", "role": "backlight", "module": "display"},
        ])
        errors, _ = self._run_with_project(_dev_board(), b, {"equipment"})
        assert any("display" in e and "not in project.json" in e for e in errors)

    def test_all_modules_present_no_error(self):
        b = _b([
            {"hardware": "ow_1", "driver": "ds18b20", "role": "air_temp", "module": "equipment"},
        ])
        errors, _ = self._run_with_project(_dev_board(), b, {"equipment", "display"})
        assert errors == []

    def test_none_project_modules_skips_check(self):
        """None-сентинел (нема project.json) — перевірка вимкнена, не порожній set."""
        b = _b([
            {"hardware": "ow_1", "driver": "ds18b20", "role": "air_temp", "module": "ghost_mod"},
        ])
        errors, _ = self._run_with_project(_dev_board(), b, None)
        assert not any("not in project.json" in e for e in errors)
