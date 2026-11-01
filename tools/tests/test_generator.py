"""
test_generator.py — тести генерації артефактів

Перевіряє що UIJsonGenerator, StateMetaGenerator,
MqttTopicsGenerator та DisplayScreensGenerator створюють коректні файли.
"""
import json

import pytest

from generate_ui import (
    UIJsonGenerator,
    StateMetaGenerator,
    StateEnumsGenerator,
    MqttTopicsGenerator,
    DisplayScreensGenerator,
    ManifestValidator,
)


# ═══════════════════════════════════════════════════════════════
#  UIJsonGenerator
# ═══════════════════════════════════════════════════════════════

class TestUIJsonGenerator:
    """Тести генерації ui.json."""

    def test_generates_valid_json(self, valid_project, thermostat_manifests):
        """Генерує валідний JSON з pages масивом."""
        gen = UIJsonGenerator()
        result = gen.generate(valid_project, thermostat_manifests)
        assert "pages" in result
        assert isinstance(result["pages"], list)

    def test_page_count_with_thermostat(self, valid_project, thermostat_manifests):
        """ui.json має 4 сторінки: Dashboard + thermostat + Network + System."""
        gen = UIJsonGenerator()
        result = gen.generate(valid_project, thermostat_manifests)
        assert len(result["pages"]) == 4

    def test_page_ids(self, valid_project, thermostat_manifests):
        """Перевірка ідентифікаторів сторінок."""
        gen = UIJsonGenerator()
        result = gen.generate(valid_project, thermostat_manifests)
        page_ids = [p["id"] for p in result["pages"]]
        assert "dashboard" in page_ids
        assert "network" in page_ids
        assert "system" in page_ids

    def test_pages_sorted_by_order(self, valid_project, thermostat_manifests):
        """Сторінки відсортовані по order."""
        gen = UIJsonGenerator()
        result = gen.generate(valid_project, thermostat_manifests)
        orders = [p.get("order", 50) for p in result["pages"]]
        assert orders == sorted(orders)

    def test_state_meta_in_output(self, valid_project, thermostat_manifests):
        """ui.json містить state_meta з метаданими."""
        gen = UIJsonGenerator()
        result = gen.generate(valid_project, thermostat_manifests)
        assert "state_meta" in result
        assert "thermostat.temperature" in result["state_meta"]

    def test_widget_keys_match_state(self, valid_project, thermostat_manifests):
        """Всі widget keys в ui.json існують у state маніфесту."""
        gen = UIJsonGenerator()
        result = gen.generate(valid_project, thermostat_manifests)
        # Збираємо всі state keys з маніфестів
        all_state_keys = set()
        for m in thermostat_manifests:
            all_state_keys.update(m.get("state", {}).keys())
        # Перевіряємо widget keys на module pages (не system)
        for page in result["pages"]:
            if page.get("system"):
                continue
            for card in page.get("cards", []):
                for w in card.get("widgets", []):
                    assert w["key"] in all_state_keys, \
                        f"Widget key '{w['key']}' не знайдено в state"

    def test_device_name_from_project(self, valid_project, thermostat_manifests):
        """device_name береться з project.json."""
        gen = UIJsonGenerator()
        result = gen.generate(valid_project, thermostat_manifests)
        assert result["device_name"] == "TestESP"

    def test_empty_modules_generates_system_pages(self, valid_project):
        """Без модулів — генеруються тільки Dashboard, Network, System."""
        gen = UIJsonGenerator()
        result = gen.generate(valid_project, [])
        page_ids = [p["id"] for p in result["pages"]]
        assert "dashboard" in page_ids
        assert "network" in page_ids
        assert "system" in page_ids
        assert len(result["pages"]) == 3

    def test_readwrite_keys_have_metadata(self, valid_project, thermostat_manifests):
        """readwrite ключі мають min/max/step в state_meta."""
        gen = UIJsonGenerator()
        result = gen.generate(valid_project, thermostat_manifests)
        sp_meta = result["state_meta"]["thermostat.setpoint"]
        assert sp_meta["access"] == "readwrite"
        assert "min" in sp_meta
        assert "max" in sp_meta
        assert "step" in sp_meta


# ═══════════════════════════════════════════════════════════════
#  StateMetaGenerator
# ═══════════════════════════════════════════════════════════════

class TestStateMetaGenerator:
    """Тести генерації state_meta.h."""

    def test_generates_header(self, thermostat_manifests):
        """Генерує C++ header з pragma once."""
        gen = StateMetaGenerator()
        result = gen.generate(thermostat_manifests)
        assert "#pragma once" in result

    def test_contains_namespace(self, thermostat_manifests):
        """Генерований файл містить modesp::gen namespace."""
        gen = StateMetaGenerator()
        result = gen.generate(thermostat_manifests)
        assert "namespace modesp::gen" in result

    def test_contains_writable_keys(self, thermostat_manifests):
        """state_meta.h містить тільки readwrite ключі з min/max/step."""
        gen = StateMetaGenerator()
        result = gen.generate(thermostat_manifests)
        assert '"thermostat.setpoint"' in result
        assert '"thermostat.hysteresis"' in result

    def test_excludes_readonly_keys(self, thermostat_manifests):
        """state_meta.h НЕ містить read-only ключі."""
        gen = StateMetaGenerator()
        result = gen.generate(thermostat_manifests)
        assert '"thermostat.temperature"' not in result
        assert '"thermostat.compressor"' not in result

    def test_meta_count(self, thermostat_manifests):
        """STATE_META_COUNT дорівнює кількості readwrite ключів."""
        gen = StateMetaGenerator()
        result = gen.generate(thermostat_manifests)
        # thermostat має 2 readwrite: setpoint, hysteresis
        assert "STATE_META_COUNT = 2" in result

    def test_empty_manifests_placeholder(self):
        """Без маніфестів — placeholder запис (щоб уникнути zero-size array)."""
        gen = StateMetaGenerator()
        result = gen.generate([])
        assert "placeholder" in result

    def test_find_state_meta_helper(self, thermostat_manifests):
        """Генерує find_state_meta helper функцію."""
        gen = StateMetaGenerator()
        result = gen.generate(thermostat_manifests)
        assert "find_state_meta" in result

    def test_correct_min_max_values(self, thermostat_manifests):
        """Перевіряє що min/max значення правильні для setpoint."""
        gen = StateMetaGenerator()
        result = gen.generate(thermostat_manifests)
        # setpoint: min=-35.0, max=0.0, step=0.5
        assert "-35.0f" in result
        assert "0.5f" in result

    def test_persist_field_in_struct(self, thermostat_manifests):
        """StateMeta struct містить поле persist."""
        gen = StateMetaGenerator()
        result = gen.generate(thermostat_manifests)
        assert "bool persist;" in result

    def test_persist_true_for_setpoint(self, thermostat_manifests):
        """thermostat.setpoint має persist=true."""
        gen = StateMetaGenerator()
        result = gen.generate(thermostat_manifests)
        # Шукаємо рядок з setpoint — він повинен мати true після writable true
        for line in result.split("\n"):
            if '"thermostat.setpoint"' in line:
                assert "true, true," in line, \
                    f"setpoint повинен мати writable=true, persist=true: {line}"
                break

    def test_persist_false_for_non_persist_key(self):
        """Ключі без persist — persist=false в state_meta."""
        manifest = [{
            "state": {
                "test.value": {
                    "type": "float",
                    "access": "readwrite",
                    "min": 0.0, "max": 100.0, "step": 1.0
                }
            }
        }]
        gen = StateMetaGenerator()
        result = gen.generate(manifest)
        for line in result.split("\n"):
            if '"test.value"' in line:
                assert "true, false," in line, \
                    f"test.value повинен мати persist=false: {line}"
                break

    def test_default_val_field_in_struct(self, thermostat_manifests):
        """StateMeta struct містить поле default_val."""
        gen = StateMetaGenerator()
        result = gen.generate(thermostat_manifests)
        assert "float default_val;" in result

    def test_default_val_for_setpoint(self, thermostat_manifests):
        """thermostat.setpoint має default_val=-18.0f."""
        gen = StateMetaGenerator()
        result = gen.generate(thermostat_manifests)
        assert "-18.0f" in result

    def test_empty_manifests_has_persist_false(self):
        """Placeholder запис має persist=false."""
        gen = StateMetaGenerator()
        result = gen.generate([])
        assert "false, false," in result or "false, 0.0f" in result


# ═══════════════════════════════════════════════════════════════
#  MqttTopicsGenerator
# ═══════════════════════════════════════════════════════════════

class TestMqttTopicsGenerator:
    """Тести генерації mqtt_topics.h."""

    def test_generates_header(self, thermostat_manifests):
        """Генерує C++ header."""
        gen = MqttTopicsGenerator()
        result = gen.generate(thermostat_manifests)
        assert "#pragma once" in result

    def test_publish_topics(self, thermostat_manifests):
        """Містить правильні publish topics."""
        gen = MqttTopicsGenerator()
        result = gen.generate(thermostat_manifests)
        assert '"thermostat.temperature"' in result
        assert '"thermostat.compressor"' in result
        assert '"thermostat.state"' in result

    def test_subscribe_topics(self, thermostat_manifests):
        """Містить правильні subscribe topics."""
        gen = MqttTopicsGenerator()
        result = gen.generate(thermostat_manifests)
        assert '"thermostat.setpoint"' in result
        assert '"thermostat.hysteresis"' in result

    def test_publish_count(self, thermostat_manifests):
        """MQTT_PUBLISH_COUNT правильний."""
        gen = MqttTopicsGenerator()
        result = gen.generate(thermostat_manifests)
        assert "MQTT_PUBLISH_COUNT = 3" in result

    def test_subscribe_count(self, thermostat_manifests):
        """MQTT_SUBSCRIBE_COUNT правильний."""
        gen = MqttTopicsGenerator()
        result = gen.generate(thermostat_manifests)
        assert "MQTT_SUBSCRIBE_COUNT = 2" in result

    def test_empty_manifests(self):
        """Без маніфестів — порожні масиви."""
        gen = MqttTopicsGenerator()
        result = gen.generate([])
        assert "MQTT_PUBLISH_COUNT = 0" in result
        assert "MQTT_SUBSCRIBE_COUNT = 0" in result


# ═══════════════════════════════════════════════════════════════
#  DisplayScreensGenerator
# ═══════════════════════════════════════════════════════════════

class TestDisplayScreensGenerator:
    """Тести генерації display_screens.h (ієрархічне дерево меню)."""

    def test_generates_header(self, thermostat_manifests):
        """Генерує C++ header."""
        gen = DisplayScreensGenerator()
        result = gen.generate(thermostat_manifests)
        assert "#pragma once" in result

    def test_main_value(self, thermostat_manifests):
        """Містить main_value з thermostat (module, label, key, format)."""
        gen = DisplayScreensGenerator()
        result = gen.generate(thermostat_manifests)
        assert '"thermostat.temperature"' in result
        assert "MAIN_VALUES_COUNT = 1" in result

    def test_menu_tree_counts(self, thermostat_manifests):
        """1 submenu (root) + 2 items = 3 nodes."""
        gen = DisplayScreensGenerator()
        result = gen.generate(thermostat_manifests)
        assert "MENU_NODES_COUNT = 3" in result
        assert "MENU_ROOT_COUNT = 1" in result

    def test_submenu_label_from_ui_page(self, thermostat_manifests):
        """Submenu label береться з ui.page якщо menu_label відсутній."""
        gen = DisplayScreensGenerator()
        result = gen.generate(thermostat_manifests)
        assert '"Холодильна камера", "", "", "", DisplayItemType::SUBMENU' in result

    def test_menu_label_override(self, valid_thermostat):
        """display.menu_label має пріоритет над ui.page."""
        m = json.loads(json.dumps(valid_thermostat))
        m["display"]["menu_label"] = "Морозилка"
        gen = DisplayScreensGenerator()
        result = gen.generate([m])
        assert '"Морозилка", "", "", "", DisplayItemType::SUBMENU' in result

    def test_edit_constraints_from_state(self, thermostat_manifests):
        """min/max/step/unit для editable item беруться зі state."""
        gen = DisplayScreensGenerator()
        result = gen.generate(thermostat_manifests)
        # thermostat.setpoint: float readwrite, min -35, max 0, step 0.5
        line = next(l for l in result.splitlines() if '"thermostat.setpoint"' in l)
        assert "DisplayItemType::EDIT_FLOAT" in line
        assert "-35.0f, 0.0f, 0.5f" in line
        assert '"°C"' in line

    def test_submenu_child_indices(self, thermostat_manifests):
        """first_child submenu вказує на перший item після root-блоку."""
        gen = DisplayScreensGenerator()
        result = gen.generate(thermostat_manifests)
        line = next(l for l in result.splitlines()
                    if "DisplayItemType::SUBMENU" in l and "// empty" not in l)
        assert "1, 2}," in line

    def test_readonly_item_is_value(self, valid_thermostat):
        """Item з access=read → VALUE (не editable)."""
        m = json.loads(json.dumps(valid_thermostat))
        m["display"]["menu_items"].append(
            {"label": "Температура", "key": "thermostat.temperature"})
        gen = DisplayScreensGenerator()
        result = gen.generate([m])
        line = next(l for l in result.splitlines()
                    if '"thermostat.temperature"' in l
                    and "DisplayItemType" in l)
        assert "DisplayItemType::VALUE" in line

    def test_enum_options_table(self, valid_thermostat):
        """readwrite int з options → EDIT_ENUM + таблиця опцій."""
        m = json.loads(json.dumps(valid_thermostat))
        m["state"]["thermostat.mode"] = {
            "type": "int", "access": "readwrite", "default": 0,
            "options": [{"value": 0, "label": "Авто"},
                        {"value": 1, "label": "Ручний"}],
        }
        m["display"]["menu_items"].append(
            {"label": "Режим", "key": "thermostat.mode"})
        gen = DisplayScreensGenerator()
        result = gen.generate([m])
        assert "OPTIONS_thermostat_mode[]" in result
        assert '{"Авто", 0},' in result
        assert '{"Ручний", 1},' in result
        line = next(l for l in result.splitlines() if '"thermostat.mode"' in l)
        assert "DisplayItemType::EDIT_ENUM" in line
        assert "OPTIONS_thermostat_mode, 2" in line

    def test_bool_item_gets_on_off_labels(self, valid_thermostat):
        """bool item отримує options з on_label/off_label."""
        m = json.loads(json.dumps(valid_thermostat))
        m["state"]["thermostat.compressor"]["on_label"] = "Працює"
        m["state"]["thermostat.compressor"]["off_label"] = "Стоп"
        m["display"]["menu_items"].append(
            {"label": "Компресор", "key": "thermostat.compressor"})
        gen = DisplayScreensGenerator()
        result = gen.generate([m])
        assert '{"Стоп", 0},' in result
        assert '{"Працює", 1},' in result

    def test_item_format_override(self, valid_thermostat):
        """format в menu_item має пріоритет над дефолтом."""
        m = json.loads(json.dumps(valid_thermostat))
        m["display"]["menu_items"][0]["format"] = "%.2f"
        gen = DisplayScreensGenerator()
        result = gen.generate([m])
        line = next(l for l in result.splitlines() if '"thermostat.setpoint"' in l)
        assert '"%.2f"' in line

    def test_default_format_by_type(self, thermostat_manifests):
        """Без format — дефолт за типом state (float → %.1f)."""
        gen = DisplayScreensGenerator()
        result = gen.generate(thermostat_manifests)
        line = next(l for l in result.splitlines() if '"thermostat.setpoint"' in l)
        assert '"%.1f"' in line

    def test_empty_manifests(self):
        """Без маніфестів — порожні масиви."""
        gen = DisplayScreensGenerator()
        result = gen.generate([])
        assert "MAIN_VALUES_COUNT = 0" in result
        assert "MENU_NODES_COUNT = 0" in result
        assert "MENU_ROOT_COUNT = 0" in result

    def test_node_limit_guard(self, valid_thermostat):
        """> 255 вузлів — SystemExit (uint8_t індекси)."""
        m = json.loads(json.dumps(valid_thermostat))
        m["display"]["menu_items"] = [
            {"label": f"Item {i}", "key": "thermostat.setpoint"}
            for i in range(300)
        ]
        gen = DisplayScreensGenerator()
        with pytest.raises(SystemExit):
            gen.generate([m])

    def test_deterministic_output(self, thermostat_manifests):
        """Повторна генерація дає ідентичний результат."""
        gen = DisplayScreensGenerator()
        assert gen.generate(thermostat_manifests) == gen.generate(thermostat_manifests)


# ══════════════════════════════════════════════════════════════
#  Long string keys
# ══════════════════════════════════════════════════════════════

def _manifest(module, state):
    return {"manifest_version": 1, "module": module, "state": state}


class TestLongStateKeys:
    """`"long": true` picks the keys SharedState keeps in its side table."""

    def test_long_key_is_listed(self):
        m = _manifest("player", {
            "player.track": {"type": "string", "access": "read", "long": True},
            "player.state": {"type": "string", "access": "read"},
        })
        out = StateMetaGenerator().generate([m])
        assert '"player.track",' in out
        assert "STATE_LONG_KEY_COUNT = 1" in out
        # A key without the flag must not be in the table: every entry costs 256 bytes.
        long_block = out.split("STATE_LONG_KEYS[] = {")[1]
        assert '"player.state"' not in long_block.split("};")[0]

    def test_capacity_matches_the_list(self):
        """The etl::vector in SharedState is sized from this, so it cannot be too small."""
        m = _manifest("radio", {
            "radio.url": {"type": "string", "access": "read", "long": True},
            "radio.next_url": {"type": "string", "access": "read", "long": True},
        })
        out = StateMetaGenerator().generate([m])
        assert "STATE_LONG_KEY_COUNT = 2" in out
        assert "#define MODESP_LONG_STATE_ENTRIES 2" in out

    def test_no_long_keys_still_compiles(self):
        """C++ has no zero-size array, so an empty list needs a placeholder."""
        out = StateMetaGenerator().generate([_manifest("x", {})])
        assert "STATE_LONG_KEY_COUNT = 0" in out
        assert "#define MODESP_LONG_STATE_ENTRIES 1" in out

    def test_long_on_a_number_is_an_error(self):
        v = ManifestValidator()
        v.validate_manifest(_manifest("audio", {
            "audio.volume": {"type": "int", "access": "readwrite",
                             "min": 0, "max": 100, "step": 1, "long": True},
        }), "audio")
        assert any("'long' applies to strings only" in e for e in v.errors)


# ══════════════════════════════════════════════════════════════
#  Command keys
# ══════════════════════════════════════════════════════════════

class TestCommandKeys:
    """`"command": true` routes a write to the owning module's queue."""

    CMD = {"type": "int", "access": "readwrite", "min": 0, "max": 6, "step": 1,
           "command": True}

    def test_command_carries_its_module(self):
        out = StateMetaGenerator().generate([_manifest("player", {"player.cmd": self.CMD})])
        assert '{"player.cmd", "player"},' in out
        assert "STATE_COMMAND_COUNT = 1" in out
        assert "find_state_command" in out

    def test_command_stays_in_state_meta(self):
        """The range check runs before the post, so the key still needs its metadata."""
        out = StateMetaGenerator().generate([_manifest("player", {"player.cmd": self.CMD})])
        assert '{"player.cmd", "int", true,' in out

    def test_readonly_command_is_an_error(self):
        v = ManifestValidator()
        bad = dict(self.CMD, access="read")
        v.validate_manifest(_manifest("player", {"player.cmd": bad}), "player")
        assert any("needs access 'readwrite'" in e for e in v.errors)

    def test_string_command_is_an_error(self):
        v = ManifestValidator()
        v.validate_manifest(_manifest("player", {
            "player.cmd": {"type": "string", "access": "readwrite", "command": True},
        }), "player")
        assert any("needs a numeric type" in e for e in v.errors)

    def test_persisted_command_is_an_error(self):
        """Restoring "next track" on boot would press the button again."""
        v = ManifestValidator()
        v.validate_manifest(_manifest("player", {
            "player.cmd": dict(self.CMD, persist=True),
        }), "player")
        assert any("cannot be persisted" in e for e in v.errors)

    def test_no_commands_still_compiles(self):
        out = StateMetaGenerator().generate([_manifest("x", {})])
        assert "STATE_COMMAND_COUNT = 0" in out
        assert "placeholder" in out


# ══════════════════════════════════════════════════════════════
#  StateEnumsGenerator
# ══════════════════════════════════════════════════════════════

class TestStateEnumsGenerator:
    """The values behind a state key with options, so the modules stop copying them."""

    SOURCE = {
        "type": "int", "access": "readwrite", "default": 0,
        "options": [
            {"value": 0, "id": "off", "label": "Вимкнено"},
            {"value": 4, "id": "file", "label": "Файл"},
            {"value": 5, "id": "radio", "label": "Радіо"},
        ],
    }

    def test_constant_per_option(self):
        out = StateEnumsGenerator().generate([_manifest("audio", {"audio.source": self.SOURCE})])
        assert "constexpr int32_t AUDIO_SOURCE_OFF = 0;" in out
        assert "constexpr int32_t AUDIO_SOURCE_FILE = 4;" in out
        assert "constexpr int32_t AUDIO_SOURCE_RADIO = 5;" in out

    def test_range_follows_the_options(self):
        """8.1a found the manifest saying 0..3 while the code used 0..5; same list now."""
        out = StateEnumsGenerator().generate([_manifest("audio", {"audio.source": self.SOURCE})])
        assert "AUDIO_SOURCE_MIN = 0;" in out
        assert "AUDIO_SOURCE_MAX = 5;" in out

    def test_options_without_ids_emit_nothing(self):
        """Opt-in per key: a Ukrainian label is not a C identifier."""
        no_ids = {"type": "int", "access": "readwrite",
                  "options": [{"value": 0, "label": "Вимк"}]}
        out = StateEnumsGenerator().generate([_manifest("audio", {"audio.x": no_ids})])
        assert "AUDIO_X" not in out
        assert "namespace modesp::gen" in out       # still a compilable header

    def test_header_is_compilable_when_empty(self):
        out = StateEnumsGenerator().generate([_manifest("x", {})])
        assert "#pragma once" in out
        assert "} // namespace modesp::gen" in out

    def test_bad_option_id_is_an_error(self):
        """An id becomes a constant name, so it has to be a C identifier.

        The JSON schema carries the pattern and fires first; the same check in
        validate_manifest() is the backstop for a manifest reaching the validator without
        the schema. Either message is a pass — what matters is that the build stops.
        """
        v = ManifestValidator()
        v.validate_manifest(_manifest("audio", {
            "audio.source": {"type": "int", "access": "readwrite", "min": 0, "max": 1,
                             "step": 1,
                             "options": [{"value": 0, "id": "no-dashes", "label": "x"}]},
        }), "audio")
        assert any("is not a C identifier" in e or "options/0/id" in e for e in v.errors)
