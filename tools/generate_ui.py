#!/usr/bin/env python3
"""
ModESP v4 — Manifest-driven Code Generator

Reads module manifests and project.json, validates, and generates:
  1. data/ui.json              — merged UI schema for runtime (GET /api/ui)
  2. generated/state_meta.h    — constexpr metadata for API validation
  3. generated/mqtt_topics.h   — MQTT topic strings
  4. generated/display_screens.h — display/LCD menu data

WebUI (data/www/) is STATIC — not generated. It loads ui.json at runtime.

Usage:
  python tools/generate_ui.py
  python tools/generate_ui.py --project project.json --modules-dir modules \\
                              --output-data data --output-gen generated
"""

import json
import os
import re
import sys
import copy
import argparse
from pathlib import Path
from datetime import datetime

# Fix console encoding on Windows
if sys.platform == 'win32':
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    sys.stderr.reconfigure(encoding='utf-8', errors='replace')


# ═══════════════════════════════════════════════════════════════
#  Configuration
# ═══════════════════════════════════════════════════════════════

PROJECT_ROOT = Path(__file__).parent.parent

# ═══════════════════════════════════════════════════════════════
#  JSON Schema validation (tools/schemas/*.schema.json, draft-07)
# ═══════════════════════════════════════════════════════════════
# Структурний шар перед доменними перевірками: ловить опечатки полів
# (additionalProperties:false — 'persits' → помилка, не тихе ігнорування),
# неправильні типи ('priority': "high" → помилка, не TypeError у сортуванні)
# та невідомі widget-типи. jsonschema — обов'язкова залежність білду
# (tools/requirements.txt), як і для compile_scenario.py.

_SCHEMA_DIR = Path(__file__).resolve().parent / "schemas"
_SCHEMA_CACHE = {}


def schema_errors(data, schema_name, label):
    """Список рядків-помилок валідації `data` проти <schema_name>.schema.json."""
    try:
        import jsonschema
    except ImportError:
        print("ERROR: python package 'jsonschema' is required for manifest "
              "validation. Install: pip install -r tools/requirements.txt")
        sys.exit(1)
    if schema_name not in _SCHEMA_CACHE:
        path = _SCHEMA_DIR / f"{schema_name}.schema.json"
        _SCHEMA_CACHE[schema_name] = jsonschema.Draft7Validator(
            json.loads(path.read_text(encoding="utf-8")))
    out = []
    for err in sorted(_SCHEMA_CACHE[schema_name].iter_errors(data),
                      key=lambda e: list(map(str, e.absolute_path))):
        loc = "/".join(str(p) for p in err.absolute_path) or "<root>"
        out.append(f"[{label}] schema: {loc}: {err.message}")
    return out


# ── DataLogger channel/event collection (shared: chart widget + generated headers) ──

def collect_log_channels(manifests):
    """[{id, state_key, enable_key, requires, default, label}] з loggable.channels
    усіх модулів. Джерело істини і для datalogger_channels.h, і для chart-widget
    config у ui.json (щоб WebUI знав id→state_key для live-оновлень)."""
    out = []
    for m in manifests:
        for key, cfg in m.get("loggable", {}).get("channels", {}).items():
            out.append({
                "id": key.split(".")[-1],   # "equipment.air_temp" → "air_temp"
                "state_key": key,
                "enable_key": cfg.get("enable_key"),
                "requires": cfg.get("requires"),
                "default": cfg.get("default", False),
                "label": cfg.get("label", key.split(".")[-1]),
            })
    return out


def collect_log_events(manifests):
    """[{id, state_key, edge, label, label_on, label_off}] з loggable.events,
    відсортовані за id. BOTH-подія логується як id (rising) + id+1 (falling)
    — це знає і datalogger, і ChartWidget через цю таблицю в ui.json."""
    out = []
    for m in manifests:
        for key, cfg in m.get("loggable", {}).get("events", {}).items():
            out.append({
                "id": cfg["id"],
                "state_key": key,
                "edge": cfg.get("edge", "rising"),
                "label": cfg.get("label", key),
                "label_on": cfg.get("label_on"),
                "label_off": cfg.get("label_off"),
            })
    out.sort(key=lambda e: e["id"])
    return out


# Widget type → compatible state types
WIDGET_TYPE_COMPAT = {
    "gauge":        {"float", "int"},
    "slider":       {"float", "int"},
    "number_input": {"float", "int"},
    "select":       {"int", "string"},
    "indicator":    {"bool"},
    "toggle":       {"bool"},
    "button":       {"bool"},
    "status_text":  {"string"},
    "text_input":   {"string"},
    "color_picker": {"string"},
    "password_input": {"string"},
    "datetime_input": {"string"},
    "value":        {"float", "int", "bool", "string"},
    "chart":        {"float"},
}


# ═══════════════════════════════════════════════════════════════
#  Manifest Validator
# ═══════════════════════════════════════════════════════════════

def role_providers(manifests):
    """[(module_name, requires[])] для кожного модуля з top-level 'requires'.

    Capability-детекція провайдерів ролей: будь-який модуль, що декларує
    requires, постачає ролі для bindings/UI/фіч — жодних хардкодів імені
    'equipment' (аудит універсальності, Фаза 2)."""
    return [(m.get("module", "?"), m["requires"])
            for m in manifests if m.get("requires")]


class ManifestValidator:
    """Validates module manifests and cross-module consistency."""

    def __init__(self):
        self.errors = []
        self.warnings = []

    def validate_manifest(self, manifest, path):
        """Validate a single manifest. Returns True if valid."""
        name = manifest.get("module", "<unknown>")

        # Структурний шар: JSON Schema (типи полів, невідомі ключі, enum-и).
        # Схемні помилки — first-class: далі доменні перевірки не женемо,
        # бо вони можуть впасти на неправильних типах.
        sch = schema_errors(manifest, "module", name)
        if sch:
            self.errors.extend(sch)
            return False

        # Check manifest_version
        mv = manifest.get("manifest_version")
        if mv is None:
            self.errors.append(f"[{name}] Missing 'manifest_version' in {path}")
        elif mv != 1:
            self.errors.append(f"[{name}] Unsupported manifest_version={mv} in {path} (expected 1)")

        # Required top-level fields
        for field in ("module", "state"):
            if field not in manifest:
                self.errors.append(f"[{name}] Missing required field '{field}' in {path}")

        # Top-level requires (role-provider capability): list of objects, each
        # with non-empty 'role' + 'type' — downstream (V15, bindings page,
        # FeatureResolver) hard-indexes these fields.
        requires = manifest.get("requires")
        if requires is not None:
            if not isinstance(requires, list):
                self.errors.append(f"[{name}] 'requires' must be a list")
            else:
                for i, req in enumerate(requires):
                    if not isinstance(req, dict):
                        self.errors.append(
                            f"[{name}] requires[{i}] must be an object")
                        continue
                    for field in ("role", "type"):
                        val = req.get(field)
                        if not val or not isinstance(val, str):
                            self.errors.append(
                                f"[{name}] requires[{i}] missing non-empty "
                                f"string '{field}'")

        # Module name: naming convention + ETL capacity (etl::string<16> on device —
        # a longer name would be silently truncated at runtime).
        mod_name = manifest.get("module")
        if mod_name:
            if not re.match(r"^[a-z][a-z0-9_]*$", mod_name):
                self.errors.append(
                    f"[{name}] Invalid module name '{mod_name}' — must match ^[a-z][a-z0-9_]*$")
            elif len(mod_name) > 16:
                self.errors.append(
                    f"[{name}] Module name '{mod_name}' is {len(mod_name)} chars — max 16")

        if "state" not in manifest:
            return len(self.errors) == 0

        state = manifest["state"]

        # Validate each state key
        prefix = f"{name}."
        for key, info in state.items():
            # V13: state key should start with <module>.
            if not key.startswith(prefix):
                self.warnings.append(
                    f"[{name}] State key '{key}' does not start with '{prefix}'")

            # SharedState keys are etl::string<32> on device — a longer key would be
            # silently truncated, risking key collisions between distinct keys.
            if len(key) > 32:
                self.errors.append(
                    f"[{name}] State key '{key}' is {len(key)} chars — max 32")

            if "type" not in info:
                self.errors.append(f"[{name}] State key '{key}' missing 'type'")
            if "access" not in info:
                self.errors.append(f"[{name}] State key '{key}' missing 'access'")

            # player-fork: checks for the fork's manifest flags `command` and `long` (emitted
            # as STATE_COMMANDS / STATE_LONG_KEYS by StateMetaGenerator).
            # A command is an event delivered to the owning module's queue, so it must be
            # writable, it must be a number (the queue carries int32_t), and it cannot be
            # persisted — restoring "next track" on boot would press the button again.
            if info.get("command"):
                if info.get("access") != "readwrite":
                    self.errors.append(
                        f"[{name}] State key '{key}': command=true needs access "
                        f"'readwrite' — nothing could ever send it")
                if info.get("type") not in ("int", "float", "bool"):
                    self.errors.append(
                        f"[{name}] State key '{key}': command=true needs a numeric type, "
                        f"this one is '{info.get('type')}'")
                if info.get("persist"):
                    self.errors.append(
                        f"[{name}] State key '{key}': command=true cannot be persisted "
                        f"— the command would be replayed on the next boot")

            # `long` marks a string whose value lives in SharedState's side table rather
            # than inline in the variant. It only means anything for a string.
            if info.get("long") and info.get("type") != "string":
                self.errors.append(
                    f"[{name}] State key '{key}': 'long' applies to strings only, "
                    f"this one is '{info.get('type')}'")

            # persist_service.cpp only implements float/int/bool NVS batches — a
            # persisted string would be silently never saved nor restored.
            if info.get("persist") and info.get("type") == "string":
                self.errors.append(
                    f"[{name}] State key '{key}': persist=true is not supported for "
                    f"type 'string' — remove persist or use float/int/bool")

            # readwrite keys must have min/max/step (unless options present)
            if info.get("access") == "readwrite":
                if info.get("type") in ("float", "int") and "options" not in info:
                    for prop in ("min", "max", "step"):
                        if prop not in info:
                            self.errors.append(
                                f"[{name}] Readwrite key '{key}' missing '{prop}'")

        # Validate UI widgets reference existing state keys or inputs
        inputs = manifest.get("inputs", {})
        ui = manifest.get("ui", {})
        for card in ui.get("cards", []):
            for w in card.get("widgets", []):
                wkey = w.get("key", "")
                wtype = w.get("widget", "")

                # Chart widget використовує data_source, не state key
                if wtype == "chart":
                    continue

                # Check key exists in state or inputs (cross-module)
                if wkey in state:
                    state_type = state[wkey].get("type", "")
                elif wkey in inputs:
                    state_type = inputs[wkey].get("type", "")
                else:
                    self.errors.append(
                        f"[{name}] Widget key '{wkey}' not found in state or inputs")
                    continue

                # Check widget type compatibility
                compat = WIDGET_TYPE_COMPAT.get(wtype)
                if compat and state_type not in compat:
                    self.errors.append(
                        f"[{name}] Widget '{wtype}' incompatible with "
                        f"state type '{state_type}' for key '{wkey}'")

        # Validate MQTT keys reference existing state keys
        mqtt = manifest.get("mqtt", {})
        for key in mqtt.get("publish", []):
            if key not in state:
                self.errors.append(
                    f"[{name}] MQTT publish key '{key}' not found in state")
        for key in mqtt.get("subscribe", []):
            if key not in state:
                self.errors.append(
                    f"[{name}] MQTT subscribe key '{key}' not found in state")
            elif state[key].get("access") != "readwrite":
                # SECURITY: subscribing makes the device accept remote writes to
                # this key — a read-only key must not be in the subscribe list.
                self.errors.append(
                    f"[{name}] MQTT subscribe key '{key}' has access="
                    f"'{state[key].get('access')}' — must be 'readwrite'")

        # mqtt.alarm — QoS1+retain: лише для ключів, що публікуються
        publish_set = set(mqtt.get("publish", []))
        for key in mqtt.get("alarm", []):
            if key not in publish_set:
                self.errors.append(
                    f"[{name}] MQTT alarm key '{key}' is not in mqtt.publish")

        # mqtt.ha — HA discovery: ключ мусить публікуватися; component з
        # переліку, який реально вміє MqttService.
        HA_COMPONENTS = {"sensor", "binary_sensor"}
        for key, ha in mqtt.get("ha", {}).items():
            if key not in publish_set:
                self.errors.append(
                    f"[{name}] MQTT ha key '{key}' is not in mqtt.publish")
            if not isinstance(ha, dict) or not ha.get("name"):
                self.errors.append(
                    f"[{name}] MQTT ha entry '{key}' needs a non-empty 'name'")
                continue
            comp = ha.get("component", "sensor")
            if comp not in HA_COMPONENTS:
                self.errors.append(
                    f"[{name}] MQTT ha entry '{key}': component '{comp}' not "
                    f"supported (allowed: {', '.join(sorted(HA_COMPONENTS))})")
            # Значення потрапляють у C-літерали генерованого хедера і в
            # discovery-JSON без escaping — лапки/бекслеші/керуючі заборонені.
            for field in ("name", "device_class", "unit", "state_class"):
                val = str(ha.get(field, ""))
                if ('"' in val or '\\' in val
                        or any(ord(c) < 0x20 for c in val)):
                    self.errors.append(
                        f"[{name}] MQTT ha entry '{key}': field '{field}' must "
                        f"not contain quotes, backslashes or control characters")

        # Validate display section (main_value + hierarchical menu)
        display = manifest.get("display", {})
        mv = display.get("main_value", {})
        if mv:
            if not mv.get("key"):
                self.errors.append(
                    f"[{name}] Display main_value missing 'key'")
            elif mv["key"] not in state:
                self.errors.append(
                    f"[{name}] Display main_value key '{mv['key']}' not found in state")
        menu_label = display.get("menu_label")
        if menu_label is not None and (not isinstance(menu_label, str)
                                       or not menu_label.strip()):
            self.errors.append(
                f"[{name}] Display menu_label must be a non-empty string")
        for item in display.get("menu_items", []):
            if not item.get("label"):
                self.errors.append(
                    f"[{name}] Display menu_item missing 'label' "
                    f"(key '{item.get('key', '?')}')")
            if not item.get("key"):
                self.errors.append(
                    f"[{name}] Display menu_item '{item.get('label', '?')}' "
                    f"missing 'key'")
            elif item["key"] not in state:
                self.errors.append(
                    f"[{name}] Display menu_item key '{item['key']}' not found in state")
            else:
                info = state[item["key"]]
                # readwrite string without options is not editable on a device menu
                if (info.get("access") == "readwrite"
                        and info.get("type") == "string"
                        and "options" not in info):
                    self.warnings.append(
                        f"[{name}] Display menu_item key '{item['key']}' is a "
                        f"readwrite string — shown as view-only on display")

        # V14: features.*.controls_settings keys must exist in state
        features = manifest.get("features", {})
        for feat_name, feat in features.items():
            for cs_key in feat.get("controls_settings", []):
                if cs_key not in state:
                    self.errors.append(
                        f"[{name}] Feature '{feat_name}' controls_settings "
                        f"key '{cs_key}' not found in state")

        # V16: constraints.*.values.*.requires_feature must exist in features
        constraints = manifest.get("constraints", {})
        for c_key, constraint in constraints.items():
            for val_str, rule in constraint.get("values", {}).items():
                rf = rule.get("requires_feature")
                if rf and rf not in features:
                    self.errors.append(
                        f"[{name}] Constraint '{c_key}' value '{val_str}' "
                        f"requires_feature '{rf}' not found in features")

        # player-fork: an option `id` becomes a C identifier in generated/state_enums.h.
        for key, info in state.items():
            for opt in info.get("options", []):
                oid = opt.get("id")
                if oid is None:
                    continue
                if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", oid):
                    self.errors.append(
                        f"[{name}] State key '{key}' option id '{oid}' is not a C "
                        f"identifier — it becomes a constant name in state_enums.h")

        # V17: options values must be int
        for key, info in state.items():
            if "options" in info:
                for opt in info["options"]:
                    if not isinstance(opt.get("value"), int):
                        self.errors.append(
                            f"[{name}] State key '{key}' option value "
                            f"'{opt.get('value')}' must be int")

        # V18: state key with options but UI widget != "select" → warning
        for card in ui.get("cards", []):
            for w in card.get("widgets", []):
                wkey = w.get("key", "")
                wtype = w.get("widget", "")
                if wkey in state and "options" in state.get(wkey, {}) and wtype != "select":
                    self.warnings.append(
                        f"[{name}] State key '{wkey}' has options but "
                        f"widget is '{wtype}' (expected 'select')")

        # V19: visible_when format validation (cards + widgets)
        def _validate_visible_when(vw, context):
            if not isinstance(vw, dict):
                self.errors.append(f"[{name}] {context}: visible_when must be object")
                return
            if "key" not in vw or not isinstance(vw["key"], str):
                self.errors.append(f"[{name}] {context}: visible_when must have 'key' (string)")
                return
            ops = [op for op in ("eq", "neq", "in") if op in vw]
            if len(ops) != 1:
                self.errors.append(
                    f"[{name}] {context}: visible_when must have exactly one operator "
                    f"(eq/neq/in), found: {ops}")
                return
            op = ops[0]
            if op == "in" and not isinstance(vw["in"], list):
                self.errors.append(f"[{name}] {context}: visible_when 'in' must be array")

        for card in ui.get("cards", []):
            if "visible_when" in card:
                _validate_visible_when(card["visible_when"],
                                       f"card '{card.get('title', '?')}'")
            for w in card.get("widgets", []):
                if "visible_when" in w:
                    _validate_visible_when(w["visible_when"],
                                           f"widget '{w.get('key', '?')}'")

        return len(self.errors) == 0

    def validate_cross_module(self, manifests, active_modules=None):
        """Check for duplicate state keys and validate inputs across modules.

        Args:
            manifests: list of loaded module manifests
            active_modules: list of module names from project.json (for inputs validation)
        """
        # Збираємо всі state keys та їх типи з усіх модулів
        seen_keys = {}
        # module_name → {key → type} — для inputs cross-validation
        module_state_map = {}
        for m in manifests:
            name = m.get("module", "?")
            state = m.get("state", {})
            module_state_map[name] = {
                k: v.get("type", "") for k, v in state.items()
            }
            for key in state:
                if key in seen_keys:
                    self.errors.append(
                        f"Duplicate state key '{key}' in modules "
                        f"'{seen_keys[key]}' and '{name}'")
                else:
                    seen_keys[key] = name

        # V15: features.requires_roles must exist in SOME role-provider's
        # requires[].role (будь-який модуль із top-level requires — не лише
        # 'equipment').
        provider_roles = set()
        role_owner = {}   # role -> module (для перевірки унікальності)
        for prov_mod, reqs in role_providers(manifests):
            for req in reqs:
                r = req.get("role", "")
                if not r:
                    continue  # validate_manifest вже зарепортив malformed entry
                provider_roles.add(r)
                # Ролі мусять бути глобально унікальні (в т.ч. всередині одного
                # модуля): bindings.json і WebUI-редактор ключаться на сам
                # рядок ролі, без module.
                if r in role_owner:
                    self.errors.append(
                        f"Role '{r}' declared more than once "
                        f"('{role_owner[r]}' / '{prov_mod}') — role names must "
                        f"be unique")
                else:
                    role_owner[r] = prov_mod
        if provider_roles:
            for m in manifests:
                mod_name = m.get("module", "?")
                for feat_name, feat in m.get("features", {}).items():
                    for role in feat.get("requires_roles", []):
                        if role not in provider_roles:
                            self.errors.append(
                                f"[{mod_name}] Feature '{feat_name}' "
                                f"requires_role '{role}' not found in any "
                                f"module's requires")

        # V20: visible_when must reference a declared state key or a known runtime
        # namespace — catches dangling refs (e.g. a leftover 'equipment.evap_temp'
        # after the hardware it gated was removed).
        # Dynamic per-role presence flags: КОЖЕН role-provider публікує
        # <module>.has_<role> у рантаймі (EquipmentBase pattern) — не лише
        # 'equipment'.
        RUNTIME_KEY_PREFIXES = tuple(
            f"{prov_mod}.has_" for prov_mod, _ in role_providers(manifests)
        ) + (
            "wifi.", "mqtt.", "system.", "_ota.",  # framework-populated (player-fork: no scenario engine)
        )

        def _vw_key(vw):
            """Extract the state key a visible_when refers to (both schema forms)."""
            if not isinstance(vw, dict):
                return None
            if isinstance(vw.get("key"), str):          # canonical {"key": "...", op: ...}
                return vw["key"]
            if len(vw) == 1:                            # short form {"<state.key>": [...]}
                only = next(iter(vw))
                if isinstance(only, str):
                    return only
            return None

        def _check_vw(vw, context, mod):
            k = _vw_key(vw)
            if k is None or k in seen_keys:
                return
            if any(k.startswith(p) for p in RUNTIME_KEY_PREFIXES):
                return
            self.errors.append(
                f"[{mod}] {context}: visible_when key '{k}' is not a declared "
                f"state key or known runtime namespace")

        for m in manifests:
            mod = m.get("module", "?")
            for card in m.get("ui", {}).get("cards", []):
                if "visible_when" in card:
                    _check_vw(card["visible_when"], f"card '{card.get('title', '?')}'", mod)
                for w in card.get("widgets", []):
                    if "visible_when" in w:
                        _check_vw(w["visible_when"], f"widget '{w.get('key', '?')}'", mod)

        # Валідація inputs секцій
        if active_modules is None:
            active_modules = [m.get("module", "?") for m in manifests]
        for m in manifests:
            self._validate_inputs(m, module_state_map, active_modules)

    def _validate_inputs(self, manifest, module_state_map, active_modules):
        """Validate inputs section of a single manifest (cross-module).

        Rules from documentation/en/02-module-author-guide/manifest.md (inputs section):
        1. Key in inputs CANNOT also be in state of same module
        2. source_module must exist in project.json OR optional=true
        3. source_module must have this key in its state
        4. type must match the type in source module's state
        5. optional=false + source missing → ERROR
        6. optional=true + source missing → WARNING
        """
        inputs = manifest.get("inputs")
        if not inputs:
            return

        name = manifest.get("module", "?")
        own_state = manifest.get("state", {})

        for key, info in inputs.items():
            source_module = info.get("source_module", "")
            expected_type = info.get("type", "")
            optional = info.get("optional", True)

            # Правило 1: ключ не може бути одночасно в state і inputs
            if key in own_state:
                self.errors.append(
                    f"[{name}] Input key '{key}' conflicts with own state "
                    f"(key cannot be in both 'state' and 'inputs')")
                continue

            # Правила 2, 5, 6: source_module повинен існувати
            if source_module not in active_modules:
                if optional:
                    self.warnings.append(
                        f"[{name}] Input '{key}': source_module '{source_module}' "
                        f"not in project.json (optional, skipping)")
                else:
                    self.errors.append(
                        f"[{name}] Input '{key}': source_module '{source_module}' "
                        f"not in project.json (required)")
                continue

            # Правило 3: source_module повинен мати цей ключ в state
            source_state = module_state_map.get(source_module, {})
            if key not in source_state:
                self.errors.append(
                    f"[{name}] Input '{key}': not found in state of "
                    f"source_module '{source_module}'")
                continue

            # Правило 4: тип повинен збігатися
            actual_type = source_state[key]
            if expected_type and actual_type and expected_type != actual_type:
                self.errors.append(
                    f"[{name}] Input '{key}': type mismatch — "
                    f"expected '{expected_type}', source has '{actual_type}'")

    def report(self):
        """Print validation results. Returns True if no errors."""
        for w in self.warnings:
            print(f"  WARNING: {w}")
        for e in self.errors:
            print(f"  ERROR: {e}")
        return len(self.errors) == 0


# ═══════════════════════════════════════════════════════════════
#  Manifest Loader
# ═══════════════════════════════════════════════════════════════

class ManifestLoader:
    """Loads and validates module manifests."""

    def __init__(self, modules_dir, validator):
        self.modules_dir = Path(modules_dir)
        self.validator = validator

    def load_module(self, name):
        """Load manifest for a single module."""
        path = self.modules_dir / name / "manifest.json"
        if not path.exists():
            self.validator.errors.append(
                f"No manifest for module '{name}' at {path}")
            return None

        with open(path, "r", encoding="utf-8") as f:
            try:
                manifest = json.load(f)
            except json.JSONDecodeError as e:
                self.validator.errors.append(
                    f"Invalid JSON in {path}: {e}")
                return None

        # Структурний шар ПЕРЕД усім: схемно-битий маніфест далі не годуємо —
        # cross-validate хард-індексує поля, які схема щойно відхилила, і впав
        # би сирим трейсбеком, загубивши чисте повідомлення.
        sch = schema_errors(manifest, "module", manifest.get("module", name))
        if sch:
            self.validator.errors.extend(sch)
            return None

        # Folder name and manifest 'module' must match — the generated registry,
        # CMake component name and include paths are all derived from the folder,
        # so a mismatch produces confusing link/registration errors downstream.
        declared = manifest.get("module")
        if declared and declared != name:
            self.validator.errors.append(
                f"Module folder 'modules/{name}/' declares module '{declared}' — "
                f"folder name and manifest 'module' field must match")

        self.validator.validate_manifest(manifest, path)
        return manifest

    def load_all(self, module_names):
        """Load manifests for all specified modules."""
        manifests = []
        for name in module_names:
            m = self.load_module(name)
            if m:
                manifests.append(m)
                n_keys = len(m.get("state", {}))
                n_widgets = sum(
                    len(c.get("widgets", []))
                    for c in m.get("ui", {}).get("cards", []))
                print(f"  + {name}: {n_keys} state keys, {n_widgets} widgets")

        self.validator.validate_cross_module(manifests, module_names)
        return manifests


# ═══════════════════════════════════════════════════════════════
#  Driver Manifest Validator
# ═══════════════════════════════════════════════════════════════

# Допустимі категорії драйверів. Допустимі hardware types — НЕ хардкод: вони
# похідні від єдиної таблиці BOARD_SECTION_TO_HW_TYPE (див. VALID_HARDWARE_TYPES
# нижче, одразу після неї). Додати новий hardware type = один рядок у ту таблицю.
# player-fork: lcd = module-bound LCD-family backend (panel/touch); storage = a mountable
# volume of files (drivers/sdmmc_card); link = a byte transport to a co-processor
# (drivers/uart_link). The last three are module-bound like display/audio: their factories
# come from DriverRegistry and the OWNING module creates them in on_bind().
VALID_DRIVER_CATEGORIES = {"sensor", "actuator", "io", "display", "audio", "lcd",
                           "storage", "link"}
# Категорії з компільованим driver-компонентом у drivers/ (Kconfig toggle +
# register-all). Display/audio — теж повноцінні драйвери: їхні фабрики модулі
# беруть із DriverRegistry (create_display/create_audio) у своєму on_bind().
DRIVER_COMPONENT_CATEGORIES = {"sensor", "actuator", "io", "display", "audio", "lcd",
                               "storage", "link"}   # player-fork
VALID_KEY_RE = re.compile(r'^[a-z0-9_]+$')


class DriverManifestValidator:
    """Validates driver manifests."""

    def __init__(self):
        self.errors = []
        self.warnings = []

    def validate(self, manifest, path):
        """Validate a single driver manifest. Returns True if valid."""
        name = manifest.get("driver", "<unknown>")

        # Структурний шар: JSON Schema — далі доменні перевірки не женемо.
        sch = schema_errors(manifest, "driver", f"driver:{name}")
        if sch:
            self.errors.extend(sch)
            return False

        # manifest_version
        mv = manifest.get("manifest_version")
        if mv is None:
            self.errors.append(f"[driver:{name}] Missing 'manifest_version' in {path}")
        elif mv != 1:
            self.errors.append(f"[driver:{name}] Unsupported manifest_version={mv} in {path}")

        # Обов'язкові поля
        for field in ("driver", "category", "hardware_type", "provides"):
            if field not in manifest:
                self.errors.append(f"[driver:{name}] Missing required field '{field}' in {path}")

        # driver key format
        driver_key = manifest.get("driver", "")
        if driver_key and not VALID_KEY_RE.match(driver_key):
            self.errors.append(
                f"[driver:{name}] Invalid driver key '{driver_key}' — must be [a-z0-9_]")

        # category
        cat = manifest.get("category")
        if cat and cat not in VALID_DRIVER_CATEGORIES:
            self.errors.append(
                f"[driver:{name}] Invalid category '{cat}' — must be one of {VALID_DRIVER_CATEGORIES}")

        # hardware_type
        hw_type = manifest.get("hardware_type")
        if hw_type and hw_type not in VALID_HARDWARE_TYPES:
            self.warnings.append(
                f"[driver:{name}] Unknown hardware_type '{hw_type}'")

        # settings validation
        for setting in manifest.get("settings", []):
            self._validate_setting(name, setting, path)

        return len(self.errors) == 0

    def _validate_setting(self, driver_name, setting, path):
        """Validate a single driver setting."""
        key = setting.get("key", "<no_key>")

        if "key" not in setting:
            self.errors.append(f"[driver:{driver_name}] Setting missing 'key' in {path}")
            return
        if "type" not in setting:
            self.errors.append(f"[driver:{driver_name}] Setting '{key}' missing 'type' in {path}")
            return
        if "default" not in setting:
            self.errors.append(f"[driver:{driver_name}] Setting '{key}' missing 'default' in {path}")

        # key format
        if not VALID_KEY_RE.match(key):
            self.errors.append(
                f"[driver:{driver_name}] Setting key '{key}' must be [a-z0-9_]")

        # float/int settings MUST have min, max, step
        stype = setting.get("type")
        if stype in ("float", "int"):
            for prop in ("min", "max", "step"):
                if prop not in setting:
                    self.errors.append(
                        f"[driver:{driver_name}] Setting '{key}' (type={stype}) missing '{prop}'")

    def report(self):
        """Print validation results. Returns True if no errors."""
        for w in self.warnings:
            print(f"  WARNING: {w}")
        for e in self.errors:
            print(f"  ERROR: {e}")
        return len(self.errors) == 0


# ═══════════════════════════════════════════════════════════════
#  Driver Manifest Loader
# ═══════════════════════════════════════════════════════════════

class DriverManifestLoader:
    """Loads and validates driver manifests referenced by modules."""

    def __init__(self, drivers_dir, validator):
        self.drivers_dir = Path(drivers_dir)
        self.validator = validator

    def load_driver(self, name):
        """Load manifest for a single driver."""
        path = self.drivers_dir / name / "manifest.json"
        if not path.exists():
            # Не помилка: module requires — це whitelist того, що МОЖНА
            # прив'язати; невідвантажений у цьому білді драйвер просто
            # недоступний. Жорсткий інваріант (bindings → існуючий драйвер)
            # перевіряє validate_bindings.
            self.validator.warnings.append(
                f"Driver '{name}' not present in drivers/ — roles listing it "
                f"cannot bind it in this build")
            return None

        with open(path, "r", encoding="utf-8") as f:
            try:
                manifest = json.load(f)
            except json.JSONDecodeError as e:
                self.validator.errors.append(
                    f"Invalid JSON in {path}: {e}")
                return None

        sch = schema_errors(manifest, "driver",
                            f"driver:{manifest.get('driver', name)}")
        if sch:
            self.validator.errors.extend(sch)
            return None

        # Folder name == manifest 'driver' field (mirrors the module guard). The P3 cap_index
        # light-scan keys by the 'driver' field while this loader resolves by folder — if they
        # diverged a driver could silently drop out of a role's eligible set.
        declared = manifest.get("driver")
        if declared and declared != name:
            self.validator.errors.append(
                f"Driver folder 'drivers/{name}/' declares driver '{declared}' — "
                f"folder name and manifest 'driver' field must match")
            return None

        self.validator.validate(manifest, path)
        return manifest

    def load_required(self, module_manifests):
        """Load the drivers a role can bind and return (loaded, cap_index).

        A role binds by CAPABILITY: `needed` = every driver that DECLARES the role's
        capability (a light scan of drivers/ builds capability→drivers), plus any explicitly
        listed req.driver (back-compat). `cap_index` (capability→sorted driver names) is
        reused by the generator for capability matching. requires stays a WHITELIST — a
        driver missing from drivers/ just means the role can't bind it in this build."""
        cap_index = {}
        for path in sorted(self.drivers_dir.glob("*/manifest.json")):
            try:
                with open(path, "r", encoding="utf-8") as f:
                    dm = json.load(f)
            except (json.JSONDecodeError, OSError):
                continue
            dn = dm.get("driver", path.parent.name)
            for cap in set(_driver_declared_capabilities(dm)):
                cap_index.setdefault(cap, set()).add(dn)
        cap_index = {c: sorted(v) for c, v in cap_index.items()}

        needed = set()
        for m in module_manifests:
            for req in m.get("requires", []):
                cap = req.get("capability")
                if cap:
                    needed.update(cap_index.get(cap, ()))
                drivers = req.get("driver", [])
                if isinstance(drivers, str):
                    drivers = [drivers]
                needed.update(drivers)

        loaded = {}
        for name in sorted(needed):
            dm = self.load_driver(name)
            if dm:
                loaded[name] = dm
                n_settings = len(dm.get("settings", []))
                print(f"  + driver/{name}: category={dm.get('category')}, "
                      f"{n_settings} settings")
        return loaded, cap_index


# ═══════════════════════════════════════════════════════════════
#  Cross-Validation (module <-> driver)
# ═══════════════════════════════════════════════════════════════

# SINGLE SOURCE OF TRUTH for hardware types: board.json section → hardware_type.
# Add a new hardware type by adding ONE row here — VALID_HARDWARE_TYPES (driver
# manifest validation) is derived from this, and the board-section validator +
# hw-id map + bindings page iterate it. (Runtime resolution — a HAL find_<type>()
# — is added by the driver author alongside the driver, same as any new bus.)
BOARD_SECTION_TO_HW_TYPE = {
    "gpio_outputs": "gpio_output",
    "gpio_inputs": "gpio_input",
    "onewire_buses": "onewire_bus",
    "adc_channels": "adc_channel",
    "encoders": "encoder",
    "i2c_buses": "i2c_bus",
    "expander_outputs": "i2c_expander_output",
    "expander_inputs": "i2c_expander_input",
    "i2c_displays": "i2c_display",
    "dsi_displays": "dsi_display",   # player-fork: MIPI-DSI panel (drivers/dsi_st7701)
    "i2c_touch": "i2c_touch",        # player-fork: I2C touch controller (drivers/gt911)
    "sdmmc_slots": "sdmmc_slot",     # player-fork: microSD/TF slot (drivers/sdmmc_card)
    "spi_buses": "spi_bus",
    "spi_displays": "spi_display",
    "pwm_outputs": "pwm_output",
    "uart_buses": "uart_bus",
    "i2s_buses": "i2s_bus",
    "ble_devices": "ble",       # legacy key alias for remote_devices (BLE-only seed)
    "remote_devices": "ble",    # transport-generic key; per-entry transport→hw_type is a P6 refinement (only "ble" wired today)
}

# Hardware types a driver manifest may declare = every type a board section maps to
# (derived, so the two never drift). A driver whose hardware_type is not here fails
# validation. EXTRA_HW_TYPES is for any future type with no dedicated board section.
EXTRA_HW_TYPES: set = set()
VALID_HARDWARE_TYPES = set(BOARD_SECTION_TO_HW_TYPE.values()) | EXTRA_HW_TYPES

# board.json top-level keys that are metadata, not hardware sections.
BOARD_META_KEYS = {"manifest_version", "board", "display_name", "version", "description"}

# board.json sections consumed by the device (HAL/config) but not bindable
# hardware entries — known, just outside BOARD_SECTION_TO_HW_TYPE.
# i2c_expanders defines the expander CHIPS; expander_outputs/inputs map their pins.
BOARD_AUX_SECTIONS = {"i2c_expanders"}


# A driver's category maps to a capability KIND (sensor reads, actuator drives).
# io/display/audio are coarse categories that resolve to one of the two kinds.
_CATEGORY_KIND = {"sensor": "sensor", "io": "sensor",
                  "actuator": "actuator", "display": "actuator", "audio": "actuator",
                  # player-fork: every module-bound backend is "actuator" kind — the module
                  # drives it, and nothing polls it as a reading. That is already why `lcd`
                  # covers the touch controller, which is an input.
                  "lcd": "actuator", "storage": "actuator", "link": "actuator"}

def _driver_declared_capabilities(drv):
    """Capabilities EXPLICITLY declared by a driver manifest (not inferred) — for the drift
    guard. Returns a list of capability names from provides.capability / provides.channels[] /
    address_channels[]."""
    out = []
    p = drv.get("provides") or {}
    if p.get("capability"):
        out.append(p["capability"])
    for ch in p.get("channels") or []:
        if ch.get("capability"):
            out.append(ch["capability"])
    for ch in drv.get("address_channels") or []:
        if ch.get("capability"):
            out.append(ch["capability"])
    return out


def role_channels_for(drv, cap):
    """Channels a driver offers for a role's capability. WITH a capability, filter to the
    matching channels (provides.channels[] preferred, else address_channels[].capability) so a
    temperature role on a multi-channel device offers only its temperature channel. WITHOUT a
    capability, fall back to all address_channels (legacy). Returns a channels list or None."""
    if not cap:
        return drv.get("address_channels") or None
    matched = []
    for ch in ((drv.get("provides") or {}).get("channels") or []):
        if ch.get("capability") == cap and ch.get("channel"):
            matched.append({"value": ch["channel"],
                            "label": ch.get("label", ch["channel"]), "capability": cap})
    for ch in (drv.get("address_channels") or []):
        if ch.get("capability") == cap:
            matched.append(ch)
    return matched or None


def cross_validate(module_manifests, driver_manifests, errors, warnings, capabilities=None,
                   cap_index=None):
    """Cross-validate module requires vs driver manifests.

    Checks:
    1. Each requires[].driver has a loaded driver manifest
    2. driver.category matches requires[].type (sensor↔sensor, actuator↔actuator)
    3. (drift guard) any capability a driver DECLARES must exist in tools/capabilities.json
       and its kind must match the driver's category. Dormant until drivers declare
       capability (P2), so it never fires — and cannot change output — before then.
    """
    for m in module_manifests:
        mod_name = m.get("module", "?")
        for req in m.get("requires", []):
            role = req.get("role", "?")
            req_type = req.get("type", "")  # sensor / actuator
            req_cap = req.get("capability")

            drivers = req.get("driver", [])
            if isinstance(drivers, str):
                drivers = [drivers]

            if req_cap:
                # Capability-typed role — the CAPABILITY is the matcher, not a driver.
                caps = capabilities or {}
                if caps and req_cap not in caps:
                    errors.append(
                        f"[{mod_name}] Require '{role}' capability '{req_cap}' "
                        f"not in tools/capabilities.json")
                else:
                    want_kind = _CATEGORY_KIND.get(req_type, req_type)
                    cap_kind = caps.get(req_cap, {}).get("kind")
                    if cap_kind and want_kind and cap_kind != want_kind:
                        errors.append(
                            f"[{mod_name}] Require '{role}' type='{req_type}' (kind "
                            f"'{want_kind}') but capability '{req_cap}' is kind '{cap_kind}'")
                    if cap_index is not None and not cap_index.get(req_cap):
                        warnings.append(
                            f"[{mod_name}] Require '{role}' capability '{req_cap}' — "
                            f"no driver provides it in this build")
                if drivers:
                    # A capability role must not also pin a driver (that re-couples it).
                    warnings.append(
                        f"[{mod_name}] Require '{role}' lists driver {drivers} alongside "
                        f"capability '{req_cap}' — capability is the matcher; drop the driver list")
                    for dn in drivers:
                        if dn in driver_manifests and \
                                req_cap not in _driver_declared_capabilities(driver_manifests[dn]):
                            errors.append(
                                f"[{mod_name}] Require '{role}' lists driver '{dn}' that does "
                                f"not provide capability '{req_cap}'")
                continue

            for drv_name in drivers:
                if drv_name not in driver_manifests:
                    # Whitelist-запис на невідвантажений драйвер — не помилка:
                    # роль просто не зможе прив'язати його в цьому білді.
                    # validate_bindings ловить справжню проблему (binding на
                    # неіснуючий драйвер) як ERROR.
                    warnings.append(
                        f"[{mod_name}] Require '{role}' lists driver "
                        f"'{drv_name}' which is not present in drivers/ — "
                        f"unavailable in this build")
                    continue

                drv = driver_manifests[drv_name]
                drv_cat = drv.get("category", "")

                # Category mismatch check
                if req_type and drv_cat and req_type != drv_cat:
                    errors.append(
                        f"[{mod_name}] Require '{role}' type='{req_type}' but "
                        f"driver '{drv_name}' category='{drv_cat}'")

    # Drift guard — a driver's DECLARED capability must be in the vocabulary and its kind must
    # agree with the driver's category. Only runs when the vocabulary was passed (the real
    # build always passes it); with no vocabulary there is nothing to check against, so callers
    # that don't load capabilities (e.g. focused unit tests) are unaffected.
    caps = capabilities or {}
    if caps:
        for drv_name, drv in driver_manifests.items():
            cat_kind = _CATEGORY_KIND.get(drv.get("category", ""))
            for cap in _driver_declared_capabilities(drv):
                if cap not in caps:
                    errors.append(
                        f"[driver {drv_name}] declares capability '{cap}' not in tools/capabilities.json")
                elif cat_kind and caps[cap].get("kind") != cat_kind:
                    errors.append(
                        f"[driver {drv_name}] capability '{cap}' is kind '{caps[cap].get('kind')}' "
                        f"but the driver's category '{drv.get('category')}' is kind '{cat_kind}'")


def validate_loggable(manifests, errors):
    """Validate DataLogger event ids in the pre-write gate (before any file lands).

    Mirrors the runtime contract enforced by datalogger_module.cpp:
      - ids are uint8_t (0-255, integer, not bool);
      - system ids 7 (EVENT_ALARM_CLEAR) and 10 (EVENT_POWER_ON) are reserved;
      - BOTH-edge events consume id (rising) AND id+1 (falling).
    Runs as part of the existing ERROR gate so a bad manifest fails the build
    before partial datalogger_*.h headers are written to generated/.
    """
    event_ids_seen = {7, 10}
    for m in manifests:
        mod = m.get("module", "?")
        for key, cfg in m.get("loggable", {}).get("events", {}).items():
            eid = cfg.get("id")
            if eid is None:
                errors.append(f"[{mod}] loggable event '{key}' missing explicit 'id'")
                continue
            if not isinstance(eid, int) or isinstance(eid, bool):
                errors.append(f"[{mod}] loggable event '{key}' id must be an integer, got {eid!r}")
                continue
            if not (0 <= eid <= 255):
                errors.append(f"[{mod}] loggable event '{key}' id={eid} out of range 0-255 (uint8_t)")
                continue
            used = {eid}
            if cfg.get("edge", "rising") == "both":
                if eid + 1 > 255:
                    errors.append(f"[{mod}] BOTH event '{key}' id={eid} leaves no room for id+1")
                    continue
                used.add(eid + 1)
            clash = used & event_ids_seen
            if clash:
                errors.append(f"[{mod}] event '{key}' id(s) {sorted(used)} collide with "
                              f"{sorted(clash)} (system ids 7/10, a BOTH id+1, or another event)")
                continue
            event_ids_seen |= used


def load_all_driver_manifests(drivers_dir, warnings):
    """Load EVERY drivers/*/manifest.json → {driver_name: manifest}.

    Unlike DriverManifestLoader.load_required (which loads only drivers a module
    requires), this is the full set — needed for binding validation, since a
    binding may reference a driver no module requires. Malformed manifest →
    warning + skip (non-fatal), mirroring the driver-glue block.
    """
    out = {}
    for path in sorted(Path(drivers_dir).glob("*/manifest.json")):
        try:
            dm = json.loads(path.read_text(encoding="utf-8"))
        except Exception as e:
            warnings.append(f"[drivers] skipping '{path.parent.name}': {e}")
            continue
        out[dm.get("driver", path.parent.name)] = dm
    return out


def validate_bindings(board, bindings, all_driver_manifests, errors, warnings,
                      project_modules=None):
    """Cross-validate bindings.json against board.json + driver manifests.

    Appends to mutable errors/warnings (same idiom as cross_validate). All checks
    are ERROR-level (fail the build): a mis-wired binding must not reach firmware
    as a silent runtime skip. board/bindings may be None (boardless build) → skip.
    project_modules (set of names from project.json) — if given, a binding that
    targets a module absent from the project is an ERROR (orphaned hardware
    config would be silently ignored at runtime otherwise).
    """
    # Unknown top-level board.json sections are silently ignored by both this
    # generator and the on-device parser — a typo ('gpio_output' for
    # 'gpio_outputs') would silently drop the hardware. Warn loudly.
    if board:
        for section in board:
            if (section not in BOARD_SECTION_TO_HW_TYPE
                    and section not in BOARD_META_KEYS
                    and section not in BOARD_AUX_SECTIONS
                    and not section.startswith("_")):   # schema allows _-prefixed comment/annotation keys
                warnings.append(
                    f"[board] unknown top-level section '{section}' — ignored. "
                    f"Known sections: {', '.join(sorted(set(BOARD_SECTION_TO_HW_TYPE) | BOARD_AUX_SECTIONS))}")

    # (a) No board or no bindings → nothing to cross-check.
    if not board or not bindings:
        return

    # (b) hw_id -> hardware_type map; duplicate id within board is a board bug.
    hw_type = {}
    hw_section = {}
    for section, htype in BOARD_SECTION_TO_HW_TYPE.items():
        for entry in board.get(section, []):
            hw_id = entry.get("id")
            if not hw_id:
                errors.append(f"[board] section '{section}' has an entry with no 'id'")
                continue
            if hw_id in hw_type:
                errors.append(
                    f"[board] duplicate hardware id '{hw_id}' "
                    f"(in '{hw_section[hw_id]}' and '{section}')")
                continue
            hw_type[hw_id] = htype
            hw_section[hw_id] = section
            # Remote devices: an observer identity ("identity"/legacy "mac") OR a connect 'name'.
            # For the BLE transport the identity is a MAC (format-checked); other transports opaque.
            if htype == "ble":
                identity = entry.get("identity") or entry.get("mac", "")   # "mac" = legacy field alias
                name = entry.get("name", "")
                if identity and not re.match(r"^([0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}$", identity):
                    errors.append(
                        f"[board] {section} '{hw_id}' has invalid MAC identity: '{identity}'")
                if not identity and not name:
                    errors.append(
                        f"[board] {section} '{hw_id}' needs 'identity'/'mac' (observer) or 'name' (connect device)")

    valid_ids = ", ".join(sorted(hw_type)) or "(none)"
    hw_usage = {}             # hw_id -> [(role, driver, address)]
    roles_by_module = {}      # module -> set(role)

    for idx, b in enumerate(bindings.get("bindings", [])):
        hw_id = b.get("hardware")
        drv_name = b.get("driver")
        role = b.get("role")
        module = b.get("module")
        addr = b.get("address", "") or ""
        tag = role or hw_id or f"#{idx}"

        # player-fork: 'driver' is optional, a binding without one is a hardware assignment.
        # (i) required fields present. NOT 'driver': a binding with no driver is a plain
        # hardware ASSIGNMENT — it says which board resource plays a role for a module, and
        # the module uses the HAL resource directly. The driver checks below are skipped for
        # those, and DriverManager creates nothing.
        missing = [name for name, val in
                   (("hardware", hw_id), ("role", role), ("module", module)) if not val]
        if missing:
            errors.append(
                f"[bindings] binding '{tag}' missing required field(s): {', '.join(missing)}")
            continue

        # (i2) binding must target a module that is actually in the project —
        # otherwise the hardware would be wired to nobody and silently ignored.
        if project_modules is not None and module not in project_modules:
            errors.append(
                f"[bindings] binding '{tag}' targets module '{module}' which is "
                f"not in project.json — remove the binding or add the module")
            continue

        # (c) hardware exists on board
        if hw_id not in hw_type:
            errors.append(
                f"[bindings] role '{role}' references hardware '{hw_id}' not in board "
                f"— valid ids: {valid_ids}")
            continue

        # (d) driver manifest exists — only when there is a driver to check.
        # player-fork: a driverless binding records its hardware use and role, then stops.
        if not drv_name:
            hw_usage.setdefault(hw_id, []).append((role, "", addr))
            seen = roles_by_module.setdefault(module, set())
            if role in seen:
                errors.append(f"[{module}] duplicate role '{role}' bound more than once")
            seen.add(role)
            continue

        drv = all_driver_manifests.get(drv_name)
        if drv is None:
            errors.append(
                f"[bindings] role '{role}' references driver '{drv_name}' which has no manifest")
            continue

        # (e) driver.hardware_type matches the board section's type
        drv_htype = drv.get("hardware_type", "")
        if drv_htype and drv_htype != hw_type[hw_id]:
            errors.append(
                f"[bindings] role '{role}': driver '{drv_name}' expects hardware_type "
                f"'{drv_htype}' but hardware '{hw_id}' is '{hw_type[hw_id]}'")

        # (f) requires_address — opt-in only (ds18b20 SKIP_ROM stays valid)
        if drv.get("requires_address", False) and not addr:
            errors.append(
                f"[bindings] role '{role}': driver '{drv_name}' requires an 'address' "
                f"but none was given for hardware '{hw_id}'")

        # accumulate for (g)/(h)
        hw_usage.setdefault(hw_id, []).append((role, drv_name, addr))
        seen = roles_by_module.setdefault(module, set())
        # (h) duplicate role within a module
        if role in seen:
            errors.append(f"[{module}] duplicate role '{role}' bound more than once")
        else:
            seen.add(role)

    # (g) same hardware reused across bindings
    for hw_id, uses in hw_usage.items():
        if len(uses) < 2:
            continue
        roles = ", ".join(u[0] for u in uses)
        multi = all(
            all_driver_manifests.get(dn, {}).get("multiple_per_bus", False)
            for dn in {u[1] for u in uses})
        if not multi:
            errors.append(
                f"[bindings] hardware '{hw_id}' bound {len(uses)} times (roles: {roles}) "
                f"but its driver does not allow multiple_per_bus")
            continue
        addrs = [u[2] for u in uses]
        if any(not a for a in addrs):
            errors.append(
                f"[bindings] hardware '{hw_id}' has {len(uses)} bindings (roles: {roles}); "
                f"each needs a distinct 'address', but one or more are missing")
        elif len(set(addrs)) != len(addrs):
            errors.append(
                f"[bindings] hardware '{hw_id}' has duplicate addresses across its "
                f"{len(uses)} bindings (roles: {roles}) — addresses must be unique")


def unused_drivers(bindings, all_driver_manifests):
    """Drivers with a manifest that the active board's bindings do NOT use.

    Excludes drivers that are USED before any binding exists: discovery-capable ones
    (e.g. ds18b20, scanned per-bus) and RUNTIME-PROVISIONED BLE devices — both observer
    sensors (hardware_type "ble" + category "sensor") and connect devices (a
    connect_name_prefix, e.g. the iPixel panel). All are added at runtime via the
    Devices page, never bound at build, so they must not be advertised as prunable —
    pruning would drop the driver's boot registration (adv-decoder / connect-matcher)
    and make the device invisible in the unified scan. Pure helper (advisory hint).
    """
    if not bindings:
        return []
    used = {b.get("driver") for b in bindings.get("bindings", [])}
    out = []
    for name, dm in sorted(all_driver_manifests.items()):
        if name in used:
            continue
        if dm.get("discovery", {}).get("supported", False):
            continue
        if dm.get("hardware_type") == "ble" and (
                dm.get("category") == "sensor" or dm.get("connect_name_prefix")):
            continue   # runtime-provisioned BLE: observer sensor OR connect device — not "unused"
        out.append(name)
    return out


# ═══════════════════════════════════════════════════════════════
#  Feature Resolver
# ═══════════════════════════════════════════════════════════════

class FeatureResolver:
    """Визначає які features активні на основі bindings."""

    def __init__(self, bindings_data, provider_manifests):
        """provider_manifests: список маніфестів модулів із top-level requires
        (role-провайдери) — не лише 'equipment'. Одиночний маніфест-dict теж
        приймається (зручність для тестів/скриптів)."""
        if isinstance(provider_manifests, dict):
            provider_manifests = [provider_manifests]
        self.bound_roles = set()
        for b in bindings_data.get("bindings", []):
            self.bound_roles.add(b["role"])
        self.all_provider_roles = set()
        for m in provider_manifests:
            for r in m.get("requires", []):
                self.all_provider_roles.add(r["role"])

    def resolve_module(self, module_manifest):
        """Повертає dict {feature_name: bool} для одного модуля."""
        features = module_manifest.get("features", {})
        result = {}
        for name, feat in features.items():
            if feat.get("always_active", False):
                result[name] = True
            else:
                required = set(feat.get("requires_roles", []))
                result[name] = required.issubset(self.bound_roles)
        return result

    def resolve_all(self, manifests):
        """Повертає dict {module_name: {feature_name: bool}}."""
        result = {}
        for m in manifests:
            result[m.get("module", "?")] = self.resolve_module(m)
        return result

    def resolve_constraints(self, module_manifest, active_features):
        """Повертає ВСІ options + requires_state/disabled_hint для runtime перевірки.
        Повертає dict {setting_key: [options_with_requires_state]}."""
        constraints = module_manifest.get("constraints", {})
        state = module_manifest.get("state", {})
        result = {}
        for key, constraint in constraints.items():
            if constraint.get("type") != "enum_filter":
                continue
            # Беремо оригінальні options зі state definition
            original_options = state.get(key, {}).get("options", [])
            if not original_options:
                continue
            # Додаємо ВСІ options + requires_state для runtime disabled
            filtered = []
            for opt in original_options:
                val_str = str(opt["value"])
                rule = constraint.get("values", {}).get(val_str, {})
                new_opt = dict(opt)
                # Domain-neutral: the constraint rule itself declares which state
                # key gates this option (e.g. "requires_state": "equipment.has_x").
                # The generator no longer hardcodes product-specific feature maps.
                req_state = rule.get("requires_state")
                if req_state:
                    new_opt["requires_state"] = req_state
                    new_opt["disabled_hint"] = rule.get("disabled_hint", "Недоступно")
                filtered.append(new_opt)
            result[key] = filtered
        return result

    def get_disabled_info(self, key, module_manifest, active_features):
        """Для setting key → (disabled: bool, reason: str|None)."""
        for feat_name, feat in module_manifest.get("features", {}).items():
            if key in feat.get("controls_settings", []):
                if not active_features.get(feat_name, True):
                    missing = set(feat.get("requires_roles", [])) - self.bound_roles
                    reason = f"Потрібно: {', '.join(sorted(missing))}" if missing else feat.get("description", "")
                    return True, reason
                return False, None
        return False, None


# ═══════════════════════════════════════════════════════════════
#  UI JSON Generator  (data/ui.json)
# ═══════════════════════════════════════════════════════════════

class UIJsonGenerator:
    """Generates merged ui.json for runtime serving via GET /api/ui."""

    def generate(self, project, manifests, driver_manifests=None,
                 board=None, bindings=None, resolver=None,
                 capabilities=None, cap_index=None):
        # Глобальна карта всіх state keys з усіх модулів (для cross-module widget keys)
        self._all_state = {}
        for m in manifests:
            for key, info in m.get("state", {}).items():
                self._all_state[key] = info

        # Зберігаємо resolver для використання у _build_widget
        self._resolver = resolver

        # DataLogger канали/події — для інжекту в chart-widget config (щоб WebUI
        # мав id→state_key і таблицю подій, не хардкодячи їх).
        self._log_channels = collect_log_channels(manifests)
        self._log_events = collect_log_events(manifests)

        # Для кожного модуля обчислюємо active features та constraints
        self._module_features = {}
        self._module_constraints = {}
        if resolver:
            for m in manifests:
                mod_name = m.get("module", "?")
                active = resolver.resolve_module(m)
                self._module_features[mod_name] = active
                self._module_constraints[mod_name] = resolver.resolve_constraints(m, active)

        pages = []

        # System pages
        sys_cfg = project.get("system", {})
        sys_pages = sys_cfg.get("pages", {})
        if sys_pages.get("dashboard", True):
            pages.append(self._dashboard_page(manifests))
        # Module pages (inserted between dashboard and system pages)
        for m in manifests:
            ui = m.get("ui")
            if not ui:
                continue
            page = self._module_page(m, ui)
            pages.append(page)
        # Bindings page (hardware overview)
        if bindings and board:
            # Агрегуємо requires з УСІХ role-провайдерів (модулі з top-level
            # requires) — ролі display/player/... видимі нарівні з equipment.
            provider_requires = []   # [(module_name, req_dict)]
            for prov_mod, reqs in role_providers(manifests):
                for req in reqs:
                    provider_requires.append((prov_mod, req))
            pages.append(self._bindings_page(
                bindings, board, driver_manifests or {},
                provider_requires, cap_index, capabilities))
        # Devices page (subscribe remote BLE devices) — only when a BLE observer driver is
        # present (hardware_type=="ble" + category=="sensor"). Absent otherwise, so BLE-less
        # builds show no page. Nearby devices are found by the unified GET /api/ble/scan.
        dev_page = self._devices_page(driver_manifests or {})
        if dev_page:
            pages.append(dev_page)
        if sys_pages.get("network", True):
            cloud_provider = sys_cfg.get("cloud_provider", "mqtt")
            pages.append(self._network_page(cloud_provider))
        # firmware cards merged into _system_page
        if sys_pages.get("system", True):
            pages.append(self._system_page())

        # Sort by order
        pages.sort(key=lambda p: p.get("order", 50))

        # Build state_meta for frontend
        state_meta = {}
        for m in manifests:
            for key, info in m.get("state", {}).items():
                entry = {
                    "type": info.get("type", "string"),
                    "access": info.get("access", "read"),
                }
                if info.get("unit"):
                    entry["unit"] = info["unit"]
                if info.get("access") == "readwrite":
                    if "options" in info:
                        entry["options"] = info["options"]
                    else:
                        for prop in ("min", "max", "step"):
                            if prop in info:
                                entry[prop] = info[prop]
                state_meta[key] = entry

        return {
            "device_name": sys_cfg.get("device_name", "ModESP"),
            "project": project.get("project", "unknown"),
            "version": project.get("version", "0.0.0"),
            "generated": datetime.now().isoformat(timespec="seconds"),
            "pages": pages,
            "state_meta": state_meta,
        }

    def _module_page(self, manifest, ui):
        """Build page from module UI section."""
        page = {
            "id": ui.get("page_id", manifest["module"]),
            "title": ui.get("page", manifest["module"]),
            "icon": ui.get("icon", "cube"),
            "order": ui.get("order", 50),
            "module": manifest["module"],
            "cards": [],
        }
        for card in ui.get("cards", []):
            page_card = {"title": card["title"], "widgets": []}
            if "group" in card:
                page_card["group"] = card["group"]
            if card.get("collapsible"):
                page_card["collapsible"] = True
            if "visible_when" in card:
                page_card["visible_when"] = card["visible_when"]
            for opt_field in ("icon", "icon_color", "subtitle", "summary_keys", "wide"):
                if opt_field in card:
                    page_card[opt_field] = card[opt_field]

            for w in card.get("widgets", []):
                widget = self._build_widget(w, manifest)
                page_card["widgets"].append(widget)
            page["cards"].append(page_card)
        return page

    def _build_widget(self, w, manifest):
        """Build a widget dict with state metadata merged in."""
        key = w["key"]
        widget = {"key": key, "widget": w["widget"]}
        # Шукаємо state info спочатку в поточному модулі, потім в глобальній карті
        state_info = manifest.get("state", {}).get(key, {})
        if not state_info and hasattr(self, '_all_state'):
            state_info = self._all_state.get(key, {})

        mod_name = manifest.get("module", "?")

        # Select: якщо є options в state definition
        if "options" in state_info:
            widget["widget"] = "select"
            constraints_map = self._module_constraints.get(mod_name, {})
            if key in constraints_map:
                widget["options"] = constraints_map[key]
            else:
                widget["options"] = state_info["options"]

        # Pull metadata from state definition
        if state_info.get("unit"):
            widget["unit"] = state_info["unit"]
        if state_info.get("description"):
            widget["description"] = state_info["description"]
        # i18n key for language pack lookup
        widget["i18n_key"] = f"state.{key}"
        if state_info.get("access") == "readwrite":
            widget["editable"] = True
            if "options" not in state_info:
                for prop in ("min", "max", "step"):
                    if prop in state_info:
                        widget[prop] = state_info[prop]
        if state_info.get("enum"):
            widget["enum"] = state_info["enum"]

        # Feature disabled check
        if self._resolver:
            active_features = self._module_features.get(mod_name, {})
            disabled, reason = self._resolver.get_disabled_info(key, manifest, active_features)
            if disabled:
                widget["disabled"] = True
                widget["disabled_reason"] = reason

        # Widget-specific props from manifest ui section
        for prop in ("size", "color_zones", "on_label", "off_label",
                     "on_color", "off_color", "format", "label",
                     "api_endpoint", "confirm", "maxlength",
                     "data_source", "default_hours"):
            if prop in w:
                widget[prop] = w[prop]
        # visible_when passthrough
        if "visible_when" in w:
            widget["visible_when"] = w["visible_when"]

        # Chart: інжектуємо мапу каналів (id→state_key для live-оновлень з
        # SharedState) і таблицю подій (id/edge/label/label_off для зон і
        # лейблів) — замість хардкоду у ChartWidget.svelte.
        if w.get("widget") == "chart":
            widget["channels"] = [
                {"id": ch["id"], "state_key": ch["state_key"],
                 "label": ch["label"]}
                for ch in getattr(self, "_log_channels", [])
            ]
            widget["events"] = [
                {"id": ev["id"], "edge": ev["edge"],
                 "label": ev.get("label_on") or ev["label"],
                 "label_off": ev.get("label_off")}
                for ev in getattr(self, "_log_events", [])
            ]
        return widget

    def _dashboard_page(self, manifests):
        """Dashboard: first card from each module + system info."""
        cards = []
        for m in manifests:
            ui = m.get("ui", {})
            if ui.get("cards"):
                first_card = ui["cards"][0]
                card = {"title": ui.get("page", m["module"]), "widgets": []}
                for w in first_card.get("widgets", []):
                    widget = self._build_widget(w, m)
                    card["widgets"].append(widget)
                cards.append(card)
        return {
            "id": "dashboard",
            "title": "Dashboard",
            "icon": "home",
            "order": 0,
            "system": True,
            "cards": cards,
        }

    def _system_pages(self):
        """Framework Network/System page STRUCTURE — loaded from tools/system_pages.json
        (data, not hardcoded here), so the built-in pages are editable without touching
        the generator. Cached per generator instance."""
        if not hasattr(self, "_sys_pages_cache"):
            p = Path(__file__).resolve().parent / "system_pages.json"
            with open(p, encoding="utf-8") as f:
                self._sys_pages_cache = json.load(f)
        return self._sys_pages_cache

    def _network_page(self, cloud_provider="mqtt"):
        """Network page: status + WiFi/AP + cloud (MQTT or AWS). Structure comes from
        system_pages.json; only the cloud variant is selected here (project.system.
        cloud_provider). deepcopy → downstream mutation can't poison the cache."""
        d = copy.deepcopy(self._system_pages()["network"])
        cloud = d["cloud"].get(cloud_provider) or d["cloud"]["mqtt"]
        sc = d["status_card"]
        status_card = {
            "title": sc["title"],
            "icon": sc["icon"],
            "subtitle": cloud["subtitle"],
            "wide": sc["wide"],
            "widgets": d["wifi_status_widgets"] + cloud["status_widgets"],
        }
        return {
            "id": d["id"],
            "title": d["title"],
            "icon": d["icon"],
            "order": d["order"],
            "system": d["system"],
            "cards": [status_card, d["wifi_card"], d["ap_card"], cloud["card"]],
        }

    def _system_page(self):
        """System page: system info, time, security, service actions. Structure from
        system_pages.json (data, not hardcoded). deepcopy so downstream sorting/mutation
        never poisons the cache."""
        return copy.deepcopy(self._system_pages()["system"])

    def _devices_page(self, driver_manifests):
        """Devices page: subscribe remote BLE devices into the runtime registry
        (/data/devices.json). Nearby devices are found by ONE unified GET /api/ble/scan —
        the transport identifies each device's driver TYPE by which registered decoder
        claims its advertisement (manufacturer or service data), so the manual scan lists
        only SUPPORTED devices with their live readings. `ble_types` is the type→label map
        the page uses to show a friendly name and to set the subscribed device's driver.
        Returns None when no BLE observer driver is present (page + nav entry disappear)."""
        ble_types = []
        for name, d in driver_manifests.items():
            if d.get("hardware_type") != "ble":
                continue
            # Subscribable BLE devices: broadcast observer SENSORS, and CONNECT devices that
            # declare a name-prefix (e.g. the iPixel panel) — both found by the unified scan.
            is_observer = d.get("category") == "sensor"
            is_connect  = bool(d.get("connect_name_prefix"))
            if not (is_observer or is_connect):
                continue
            ble_types.append({
                "driver": name,
                "label": d.get("description", name),
                "connect": is_connect,   # connect device → subscribe stores adv-name, not MAC
            })
        if not ble_types:
            return None
        return {
            "id": "devices",
            "title": "Пристрої",
            "icon": "radio",
            "order": 78,   # just before Bindings (80)
            "system": True,
            "access_level": "service",
            "scan_endpoint": "/api/ble/scan",
            "ble_types": ble_types,
        }

    def _bindings_page(self, bindings, board, driver_manifests,
                        provider_requires=None, cap_index=None, capabilities=None):
        """Bindings page: current bindings + free hardware + role metadata.

        provider_requires: [(module_name, req_dict)] — агрегація з усіх
        role-провайдерів; кожна роль несе 'module' для WebUI-редактора."""
        binding_list = bindings.get("bindings", [])

        # Збираємо всі hardware ids з board.json
        all_hw_ids = set()
        for section in BOARD_SECTION_TO_HW_TYPE:
            for item in board.get(section, []):
                all_hw_ids.add(item.get("id", ""))

        # Визначаємо зайняті hardware ids
        used_hw_ids = {b.get("hardware") for b in binding_list}

        # Картка "Призначене обладнання"
        bound_widgets = []
        for b in binding_list:
            hw = b.get("hardware", "?")
            role = b.get("role", "?")
            driver = b.get("driver", "?")
            module = b.get("module", "?")
            addr = b.get("address")
            label = f"{hw} \u2192 {role} ({driver} \u2192 {module})"
            if addr:
                label += f" [{addr}]"
            bound_widgets.append({
                "key": f"_binding.{hw}",
                "widget": "value",
                "description": label,
            })

        # Картка "Вільне обладнання"
        free_hw = sorted(all_hw_ids - used_hw_ids)
        free_widgets = []
        for hw_id in free_hw:
            # Визначаємо тип hardware
            hw_type = "?"
            for section, htype in BOARD_SECTION_TO_HW_TYPE.items():
                for item in board.get(section, []):
                    if item.get("id") == hw_id:
                        hw_type = htype
                        break
            free_widgets.append({
                "key": f"_hw.{hw_id}",
                "widget": "value",
                "description": f"{hw_id} ({hw_type}) \u2014 не призначено",
            })

        cards = []
        if bound_widgets:
            cards.append({
                "title": "Призначене обладнання",
                "widgets": bound_widgets,
            })
        if free_widgets:
            cards.append({
                "title": "Вільне обладнання",
                "widgets": free_widgets,
            })
        if not cards:
            cards.append({
                "title": "Обладнання",
                "widgets": [{
                    "key": "_binding.empty",
                    "widget": "value",
                    "description": "Немає налаштованих підключень",
                }],
            })

        # driver -> its ROLE-LEVEL scan config. Only drivers whose discovery assigns an
        # "address" (a per-role channel on a shared bus, e.g. a OneWire ROM) get a picker
        # inside the role card. Drivers that assign a "mac" identify a whole DEVICE, not a
        # role channel — those are subscribed on the Devices page (see _devices_page), and
        # the role then binds to the resulting device id transport-agnostically. Keyed by
        # DRIVER, not hw_type: one bus type (e.g. "ble") can host both a discovery driver
        # and a non-discovery one. Manifest-driven — discovery.supported opts in.
        discovery_by_driver = {}
        for name, d in driver_manifests.items():
            disc = d.get("discovery", {})
            if disc.get("supported", False) and disc.get("assigns", "address") == "address":
                discovery_by_driver[name] = {
                    "endpoint": disc.get("scan_endpoint", ""),
                    "assigns": "address",
                    # The picker only makes sense on the discovery driver's own bus type;
                    # BindingCard gates on selectedHw.hw_type == scan.hw_type so a multi-
                    # driver role (e.g. ds18b20+ntc) doesn't show the OneWire scanner when
                    # the user picks the ADC/NTC hardware.
                    "hw_type": d.get("hardware_type", ""),
                }

        # Roles metadata для BindingsEditor
        roles = []
        for prov_mod, req in (provider_requires or []):
            # role = CAPABILITY: the eligible drivers are EVERY driver that provides the
            # capability (no hand list). A role without a capability falls back to its
            # explicit driver list (legacy, still supported).
            req_cap = req.get("capability")
            if req_cap:
                drivers = list((cap_index or {}).get(req_cap, []))
            else:
                drivers = req.get("driver", [])
                if isinstance(drivers, str):
                    drivers = [drivers]

            # Збираємо hw_types та requires_address з усіх допустимих драйверів
            hw_types = []
            requires_address = False
            scan_cfg = None
            channels_by_driver = {}
            cap_meta = (capabilities or {}).get(req_cap, {}) if req_cap else {}
            _cap_lbl = cap_meta.get("label"); _cap_unit = cap_meta.get("unit")
            cap_default_lbl = (f"{_cap_lbl}, {_cap_unit}" if _cap_lbl and _cap_unit
                               else _cap_lbl)   # capability's canonical channel label (or None)
            for drv_name in drivers:
                drv = driver_manifests.get(drv_name, {})
                hw_t = drv.get("hardware_type", "")
                if hw_t and hw_t not in hw_types:
                    hw_types.append(hw_t)
                if drv.get("requires_address", False):
                    requires_address = True
                # First address-assigning discovery driver defines the role's channel
                # picker (mac-assigning drivers are handled on the Devices page instead).
                if scan_cfg is None and drv_name in discovery_by_driver:
                    scan_cfg = discovery_by_driver[drv_name]
                # Fixed channel enum, PER DRIVER, filtered to the role's capability. The editor
                # shows a channel <select> ONLY when the BOUND device's driver exposes 2+ channels
                # of the capability; a device with a single matching channel is auto-bound (no
                # picker). A channel without an explicit driver label shows the capability's
                # canonical label — a temperature channel reads the same whatever driver provides it.
                chs = role_channels_for(drv, req_cap)
                if chs:
                    channels_by_driver[drv_name] = [
                        {"value": c["value"],
                         "capability": c.get("capability", req_cap),
                         "label": c.get("label") or cap_default_lbl or c["value"]}
                        for c in chs
                    ]

            # requires_address / channels / scan are PER-DRIVER properties. A capability role
            # spans HETEROGENEOUS drivers (e.g. a wired direct-read ds18b20 that needs no
            # address AND a BLE channel-select that does), so the WebUI must key these off the
            # BOUND driver, not the role. addr_drivers = the eligible drivers that need an
            # address; the editor requires one ONLY when the chosen hardware uses such a driver.
            addr_drivers = [n for n in drivers
                            if driver_manifests.get(n, {}).get("requires_address", False)]
            role_entry = {
                "role": req["role"],
                "type": req["type"],
                "module": prov_mod,
                "drivers": drivers,
                "hw_types": hw_types,
                "requires_address": requires_address,   # role-wide OR (legacy consumers)
                "addr_drivers": addr_drivers,            # per-driver truth (WebUI keys on this)
                "label": req.get("label", req["role"]),
                "optional": req.get("optional", False),
            }
            # Capability + its direction (in/out) — the matcher the WebUI keys on.
            if req_cap:
                role_entry["capability"] = req_cap
                direction = (capabilities or {}).get(req_cap, {}).get("direction")
                if direction:
                    role_entry["direction"] = direction
            # Role's driver can enumerate devices → BindingsEditor shows the scan picker.
            if scan_cfg is not None:
                role_entry["scan"] = scan_cfg
            # Per-driver channel enums → editor shows a channel <select> ONLY when the bound
            # device's driver has 2+ channels of the capability (else the sole one is auto-bound).
            if channels_by_driver:
                role_entry["channels_by_driver"] = channels_by_driver
            # Legacy driver-typed role with a single driver → emit driver/hw_type. NOT for a
            # capability role (its single-eligible-driver count is incidental, not declared, so
            # a later 2nd provider must not silently flip the shape).
            if not req_cap and len(drivers) == 1:
                role_entry["driver"] = drivers[0]
                role_entry["hw_type"] = hw_types[0] if hw_types else ""
            roles.append(role_entry)

        # hw_types that support multiple roles on one port (a driver with
        # multiple_per_bus). Manifest-driven — the BindingsEditor uses hw.shareable
        # instead of a hardcoded set, so a new shareable bus needs no webui edit.
        shareable_hw_types = {
            d.get("hardware_type", "")
            for d in driver_manifests.values()
            if d.get("multiple_per_bus", False)
        }
        # Hardware inventory з board.json (для select options у BindingsEditor)
        hardware = []
        for section, hw_type in BOARD_SECTION_TO_HW_TYPE.items():
            for item in board.get(section, []):
                hw_entry = {
                    "id": item.get("id", ""),
                    "hw_type": hw_type,
                    "label": item.get("label", item.get("id", "")),
                    "shareable": hw_type in shareable_hw_types,
                }
                if "gpio" in item:
                    hw_entry["gpio"] = item["gpio"]
                hardware.append(hw_entry)

        return {
            "id": "bindings",
            "title": "Обладнання",
            "icon": "link",
            "order": 80,
            "system": True,
            "access_level": "service",
            "cards": cards,
            "roles": roles,
            "hardware": hardware,
        }


# ═══════════════════════════════════════════════════════════════
#  C++ Header Generators
# ═══════════════════════════════════════════════════════════════

def _meta_float(v, key, field, fallback=0.0):
    """Coerce a numeric meta field to float; non-numeric (e.g. string-typed state
    with a non-numeric/empty default) → fallback. The float slots are meaningless
    for string/bool keys, so this is safe — and keeps one bad entry from crashing
    the whole generator."""
    try:
        return float(v)
    except (TypeError, ValueError):
        import sys as _sys
        print(f"  WARN: state '{key}' has non-numeric {field}={v!r} → using {fallback}",
              file=_sys.stderr)
        return fallback


class StateMetaGenerator:
    """Generates generated/state_meta.h — constexpr metadata for API validation."""

    def generate(self, manifests):
        # Підрахунок ВСІХ state keys з маніфестів для auto-capacity
        total_manifest_keys = sum(len(m.get("state", {})) for m in manifests)
        # Runtime margin breakdown:
        #   48 — base: _ota.* (7), wifi.* (5), mqtt.* (3), system.* (5),
        #         equipment.has_* (up to 12 dynamic roles), equipment.* actuator states
        #   48 — sequence engine reservation: scenario.resource.<hash>.owner mirrors
        #         (up to 32 resources x MAX_SEQUENCES instances), engine internal counters,
        #         future expansion. See ADR-0005 (resource arbitration), plan Q3.
        SEQUENCE_RUNTIME_MARGIN = 96
        capacity = total_manifest_keys + SEQUENCE_RUNTIME_MARGIN

        lines = [
            "#pragma once",
            "// Auto-generated by generate_ui.py \u2014 DO NOT EDIT",
            "",
            "#include <cstdint>",
            "#include <cstddef>",
            "",
            f"// SharedState auto-capacity: {total_manifest_keys} manifest keys + {SEQUENCE_RUNTIME_MARGIN} runtime margin",
            f"#define MODESP_MAX_STATE_ENTRIES {capacity}",
            "",
            "namespace modesp::gen {",
            "",
            "struct StateMeta {",
            "    const char* key;",
            '    const char* type;     // "float", "int", "bool", "string"',
            "    bool writable;",
            "    bool persist;",
            "    float min_val;",
            "    float max_val;",
            "    float step;",
            "    float default_val;",
            "};",
            "",
        ]

        # Include readwrite keys AND read-only keys with persist=true
        rw_entries = []
        for m in manifests:
            for key, info in m.get("state", {}).items():
                if info.get("access") == "readwrite" or info.get("persist", False):
                    rw_entries.append((key, info))

        lines.append("static constexpr StateMeta STATE_META[] = {")
        if rw_entries:
            for key, info in rw_entries:
                stype = info.get("type", "float")
                writable = "true" if info.get("access") == "readwrite" else "false"
                persist = "true" if info.get("persist", False) else "false"
                # Для keys з options — min/max обчислюються з values
                options = info.get("options")
                if options:
                    values = [opt["value"] for opt in options]
                    min_v = float(min(values))
                    max_v = float(max(values))
                    step_v = 1.0
                else:
                    min_v = _meta_float(info.get("min", 0.0), key, "min")
                    max_v = _meta_float(info.get("max", 0.0), key, "max")
                    step_v = _meta_float(info.get("step", 1.0), key, "step", 1.0)
                default_v = _meta_float(info.get("default", 0.0), key, "default")
                lines.append(
                    f'    {{"{key}", "{stype}", {writable}, {persist}, '
                    f'{min_v}f, {max_v}f, {step_v}f, {default_v}f}},')
        else:
            # Empty array fallback — avoid zero-size array in C++
            lines.append('    {"", "", false, false, 0.0f, 0.0f, 0.0f, 0.0f},  // placeholder')
        lines.append("};")
        lines.append(
            f"static constexpr size_t STATE_META_COUNT = "
            f"{max(len(rw_entries), 1) if not rw_entries else len(rw_entries)};")
        lines.append("")

        # Lookup helper
        lines.extend([
            "// Lookup helper (linear search \u2014 small array, compile-time size)",
            "inline const StateMeta* find_state_meta(const char* key) {",
            "    for (size_t i = 0; i < STATE_META_COUNT; i++) {",
            "        const char* a = STATE_META[i].key;",
            "        const char* b = key;",
            "        while (*a && *a == *b) { a++; b++; }",
            "        if (*a == *b) return &STATE_META[i];",
            "    }",
            "    return nullptr;",
            "}",
            "",
        ])

        # —— Command keys ——
        # player-fork: STATE_COMMANDS, for the fork's `command` flag.
        # `"command": true` in a manifest means the key is an EVENT for that module, not a
        # value in the store. The writers (HTTP, MQTT, BLE, the screen) look the key up here
        # and post it to the owning module's queue; nothing is written to SharedState, so a
        # button push is neither coalesced with the next one nor broadcast twice.
        commands = []
        for m in manifests:
            mod_name = m.get("module", "")
            for key, info in m.get("state", {}).items():
                if info.get("command"):
                    commands.append((key, mod_name))
        lines.extend([
            "// Keys that are COMMANDS — manifest `command: true`. A write to one of these is",
            "// posted to the named module's queue (BaseModule::post_command) instead of being",
            "// stored. They stay in STATE_META so the range check still runs before the post.",
            "struct StateCommand {",
            "    const char* key;",
            "    const char* module;",
            "};",
            f"static constexpr size_t STATE_COMMAND_COUNT = {len(commands)};",
            "static constexpr StateCommand STATE_COMMANDS[] = {",
        ])
        if commands:
            for key, mod_name in commands:
                lines.append(f'    {{"{key}", "{mod_name}"}},')
        else:
            lines.append('    {"", ""},  // placeholder — C++ has no zero-size array')
        lines.extend([
            "};",
            "",
            "inline const StateCommand* find_state_command(const char* key) {",
            "    for (size_t i = 0; i < STATE_COMMAND_COUNT; i++) {",
            "        const char* a = STATE_COMMANDS[i].key;",
            "        const char* b = key;",
            "        while (*a && *a == *b) { a++; b++; }",
            "        if (*a == *b) return &STATE_COMMANDS[i];",
            "    }",
            "    return nullptr;",
            "}",
            "",
        ])

        # —— Long string keys ——
        # player-fork: STATE_LONG_KEYS, for the fork's `long` flag.
        # A path or a URI does not fit the inline string in StateValue, and that inline
        # string is what every one of the map slots pays for. Keys flagged `"long": true`
        # keep their value in a side table instead. The list is generated so the manifests
        # and SharedState cannot drift apart on which keys those are.
        long_keys = []
        for m in manifests:
            for key, info in m.get("state", {}).items():
                if info.get("long"):
                    long_keys.append(key)
        lines.extend([
            "// Keys whose string value lives in SharedState's side table — manifest `long: true`.",
            "// These are the keys where a cut value is a broken feature (a file that cannot be",
            "// opened, a URI that no longer compares equal), not a shortened label.",
            f"static constexpr size_t STATE_LONG_KEY_COUNT = {len(long_keys)};",
            f"#define MODESP_LONG_STATE_ENTRIES {max(len(long_keys), 1)}",
            "static constexpr const char* STATE_LONG_KEYS[] = {",
        ])
        if long_keys:
            for key in long_keys:
                lines.append(f'    "{key}",')
        else:
            lines.append('    "",  // placeholder — C++ has no zero-size array')
        lines.extend([
            "};",
            "",
            "inline bool is_long_state_key(const char* key) {",
            "    for (size_t i = 0; i < STATE_LONG_KEY_COUNT; i++) {",
            "        const char* a = STATE_LONG_KEYS[i];",
            "        const char* b = key;",
            "        while (*a && *a == *b) { a++; b++; }",
            "        if (*a == *b) return true;",
            "    }",
            "    return false;",
            "}",
            "",
            "} // namespace modesp::gen",
            "",
        ])

        return "\n".join(lines)


# player-fork: generated/state_enums.h, the option values as named constants that modules
# include instead of keeping their own copies of the numbers.
class StateEnumsGenerator:
    """Generates generated/state_enums.h — the values behind a state key with `options`.

    A key with options is a small closed set of numbers with a meaning: audio.source is
    0..5 and 5 means "the network stream". Those numbers were written out by hand in four
    places — audio_module.h had the enum, and player, radio and dlna each had their own
    `constexpr int32_t AUDIO_SRC_* = N` with a comment explaining that they could not include
    the audio module's header because a build may carry either module without the other.

    That reasoning was right and the copies were still copies. The manifest is what the two
    modules genuinely share, so the constant comes from there.

    Each option needs an `id` — a C identifier for the value, next to the human label, which
    is Ukrainian and cannot be one. A key whose options have no ids emits nothing, so this is
    opt-in per key; ids on SOME options of a key is an error, because the resulting header
    would be missing values without saying so.
    """

    @staticmethod
    def _const(key, opt_id):
        # audio.source + "radio" -> AUDIO_SOURCE_RADIO
        stem = key.replace(".", "_").replace("-", "_").upper()
        return f"{stem}_{opt_id.upper()}"

    def generate(self, manifests):
        lines = [
            "#pragma once",
            "// Auto-generated by generate_ui.py — DO NOT EDIT",
            "",
            "// The values behind the state keys that carry `options` in their manifest.",
            "//",
            "// These are the numbers modules compare and assign: `audio.source == 5` is what",
            "// \"the network stream is playing\" means on the wire between them. Including this",
            "// header is not a dependency on another MODULE — it is the manifest set, which is",
            "// the contract the modules already share; a build carrying the radio without the",
            "// audio module still gets the same numbers.",
            "",
            "#include <cstdint>",
            "",
            "namespace modesp::gen {",
            "",
        ]

        emitted = 0
        for m in manifests:
            for key, info in m.get("state", {}).items():
                options = info.get("options")
                if not options:
                    continue
                ids = [o.get("id") for o in options]
                if not any(ids):
                    continue
                if not all(ids):
                    print(f"  ERROR: state key '{key}': some options have an 'id' and some "
                          f"do not — state_enums.h would be missing values silently",
                          file=sys.stderr)
                    sys.exit(1)
                desc = info.get("description", "")
                lines.append(f"// {key}" + (f" — {desc}" if desc else ""))
                for o in options:
                    label = o.get("label", "")
                    name = self._const(key, o["id"])
                    comment = f"  // {label}" if label else ""
                    lines.append(
                        f"constexpr int32_t {name} = {int(o['value'])};{comment}")
                values = [int(o["value"]) for o in options]
                stem = key.replace(".", "_").replace("-", "_").upper()
                lines.append(f"constexpr int32_t {stem}_MIN = {min(values)};")
                lines.append(f"constexpr int32_t {stem}_MAX = {max(values)};")
                lines.append("")
                emitted += 1

        if not emitted:
            lines.append("// (no state key in this build declares options with ids)")
            lines.append("")
        lines.append("} // namespace modesp::gen")
        lines.append("")
        return "\n".join(lines)


class MqttTopicsGenerator:
    """Generates generated/mqtt_topics.h.

    Inputs (module manifests, 'mqtt' section):
      publish/subscribe — key lists (як і раніше);
      alarm             — підмножина publish: QoS1 + retain (надійна доставка);
      ha                — {state_key: {name, component, device_class?,
                          state_class?, unit?}} — Home Assistant discovery;
                          unit за замовчуванням береться зі state-ключа.
    Plus project.json system.mqtt_topic_root → gen::MQTT_TOPIC_ROOT.
    """

    def generate(self, manifests, project=None):
        pub_keys = []
        sub_keys = []
        alarm_keys = set()
        ha_entities = []   # (state_key, name, component, device_class, unit, state_class)
        for m in manifests:
            mqtt = m.get("mqtt", {})
            pub_keys.extend(mqtt.get("publish", []))
            sub_keys.extend(mqtt.get("subscribe", []))
            alarm_keys.update(mqtt.get("alarm", []))
            state = m.get("state", {})
            for key, ha in mqtt.get("ha", {}).items():
                unit = ha.get("unit", state.get(key, {}).get("unit", ""))
                ha_entities.append((
                    key,
                    ha.get("name", key),
                    ha.get("component", "sensor"),
                    ha.get("device_class", ""),
                    unit,
                    ha.get("state_class", ""),
                ))
        topic_root = ((project or {}).get("system", {}) or {}).get(
            "mqtt_topic_root", "modesp")

        lines = [
            "#pragma once",
            "// Auto-generated by generate_ui.py \u2014 DO NOT EDIT",
            "",
            "namespace modesp::gen {",
            "",
        ]

        # Publish
        lines.append("static constexpr const char* MQTT_PUBLISH[] = {")
        if pub_keys:
            for key in pub_keys:
                lines.append(f'    "{key}",')
        else:
            lines.append('    "",  // empty')
        lines.append("};")
        lines.append(f"static constexpr size_t MQTT_PUBLISH_COUNT = {len(pub_keys)};")
        lines.append("")

        # Subscribe
        lines.append("static constexpr const char* MQTT_SUBSCRIBE[] = {")
        if sub_keys:
            for key in sub_keys:
                lines.append(f'    "{key}",')
        else:
            lines.append('    "",  // empty')
        lines.append("};")
        lines.append(f"static constexpr size_t MQTT_SUBSCRIBE_COUNT = {len(sub_keys)};")
        lines.append("")

        # Alarm-class publish keys (manifest mqtt.alarm) — паралельний до
        # MQTT_PUBLISH масив прапорців: true → QoS1 + retain.
        lines.append("// QoS1+retain для alarm-класу ключів (manifest mqtt.alarm)")
        lines.append("static constexpr bool MQTT_PUBLISH_ALARM[] = {")
        if pub_keys:
            for key in pub_keys:
                lines.append(f"    {'true' if key in alarm_keys else 'false'},  // {key}")
        else:
            lines.append("    false,  // empty")
        lines.append("};")
        lines.append("")

        # Topic root (project.json system.mqtt_topic_root; default "modesp")
        lines.append("// Коренева назва топіків/ідентифікаторів (project.json)")
        lines.append(f'static constexpr const char* MQTT_TOPIC_ROOT = "{topic_root}";')
        lines.append("")

        # Home Assistant discovery entities (manifest mqtt.ha)
        lines.append("// HA discovery entities (manifest mqtt.ha) — MqttService")
        lines.append("struct HaEntity {")
        lines.append("    const char* state_key;")
        lines.append("    const char* name;")
        lines.append("    const char* component;     // sensor | binary_sensor")
        lines.append('    const char* device_class;  // "" = none')
        lines.append('    const char* unit;          // "" = none')
        lines.append('    const char* state_class;   // "" = none')
        lines.append("};")
        lines.append("static constexpr HaEntity HA_ENTITIES[] = {")
        if ha_entities:
            for key, name, comp, dclass, unit, sclass in ha_entities:
                lines.append(
                    f'    {{"{key}", "{name}", "{comp}", "{dclass}", '
                    f'"{unit}", "{sclass}"}},')
        else:
            lines.append('    {"", "", "", "", "", ""},  // empty')
        lines.append("};")
        lines.append(f"static constexpr size_t HA_ENTITIES_COUNT = {len(ha_entities)};")
        lines.append("")
        lines.append("} // namespace modesp::gen")
        lines.append("")

        return "\n".join(lines)


class DisplayScreensGenerator:
    """Generates generated/display_screens.h — hierarchical LCD/OLED menu tree.

    Layout of MENU_NODES (flattened tree):
      [0 .. MENU_ROOT_COUNT-1]  — module submenus (root children)
      [MENU_ROOT_COUNT .. N-1]  — items, contiguous per submenu

    Edit constraints (min/max/step/unit/options) are merged from the
    state definition — menu_items only need label + key.
    """

    DEFAULT_FORMATS = {"float": "%.1f", "int": "%d", "bool": "%s", "string": "%s"}

    @staticmethod
    def _cstr(s):
        """Escape a Python string into a C string literal (without quotes)."""
        return str(s).replace("\\", "\\\\").replace('"', '\\"')

    def _item_type(self, info):
        """Map a state definition to a DisplayItemType."""
        stype = info.get("type", "string")
        if info.get("access") != "readwrite":
            return "VALUE"
        if "options" in info:
            return "EDIT_ENUM"
        if stype == "float":
            return "EDIT_FLOAT"
        if stype == "int":
            return "EDIT_INT"
        if stype == "bool":
            return "EDIT_BOOL"
        return "VALUE"  # readwrite string — view-only on device

    def _options_for(self, key, info):
        """Options table for enum keys; on/off labels for bool keys."""
        if "options" in info:
            return [(opt.get("label", str(opt["value"])), opt["value"])
                    for opt in info["options"]]
        if info.get("type") == "bool":
            return [(info.get("off_label", "OFF"), 0),
                    (info.get("on_label", "ON"), 1)]
        return None

    def generate(self, manifests):
        main_values = []
        submenus = []  # [{label, items: [node dict]}]

        for m in manifests:
            display = m.get("display", {})
            state = m.get("state", {})
            menu_label = (display.get("menu_label")
                          or m.get("ui", {}).get("page")
                          or m["module"])

            mv = display.get("main_value")
            if mv:
                main_values.append({
                    "module": m["module"],
                    "label": menu_label,
                    "key": mv["key"],
                    "format": mv.get("format", "%s"),
                })

            items = []
            for item in display.get("menu_items", []):
                info = state.get(item["key"], {})
                stype = info.get("type", "string")
                items.append({
                    "label": item["label"],
                    "key": item["key"],
                    "format": item.get("format")
                              or self.DEFAULT_FORMATS.get(stype, "%s"),
                    "unit": info.get("unit", ""),
                    "type": self._item_type(info),
                    "min": float(info.get("min", 0)),
                    "max": float(info.get("max", 0)),
                    "step": float(info.get("step", 0)),
                    "options": self._options_for(item["key"], info),
                    "cap": item.get("cap", ""),   # capability-gate (DisplayCaps); "" = завжди
                })
            if items:
                submenus.append({"label": menu_label, "items": items})

        total_nodes = len(submenus) + sum(len(s["items"]) for s in submenus)
        if total_nodes > 255:
            raise SystemExit(
                f"display: menu tree has {total_nodes} nodes (max 255 — "
                f"first_child/child_count are uint8_t)")

        # menu-1: MenuView усікає список одного рівня до MAX_MENU_ITEMS (display_port.h),
        # +1 — віртуальний "Назад". Перевищення → "мертвий" курсор/недосяжний back.
        MAX_MENU_ITEMS = 16  # дзеркалить modesp::display::MAX_MENU_ITEMS
        if len(submenus) > MAX_MENU_ITEMS - 1:
            raise SystemExit(
                f"display: {len(submenus)} root submenus (max {MAX_MENU_ITEMS - 1} "
                f"— MenuView caps one level at {MAX_MENU_ITEMS} incl. back item)")
        for s in submenus:
            if len(s["items"]) > MAX_MENU_ITEMS - 1:
                raise SystemExit(
                    f"display: submenu '{s['label']}' has {len(s['items'])} items "
                    f"(max {MAX_MENU_ITEMS - 1} — MenuView caps one level at "
                    f"{MAX_MENU_ITEMS} incl. back item)")

        lines = [
            "#pragma once",
            "// Auto-generated by generate_ui.py — DO NOT EDIT",
            "",
            "#include <cstddef>",
            "#include <cstdint>",
            "",
            "namespace modesp::gen {",
            "",
            "// ── Idle screen: primary values cycled on the main screen ──",
            "struct DisplayMainValue {",
            "    const char* module;",
            "    const char* label;",
            "    const char* key;",
            "    const char* format;",
            "};",
            "",
            "// ── Menu tree ──",
            "enum class DisplayItemType : uint8_t {",
            "    SUBMENU,     // node with children",
            "    VALUE,       // read-only value",
            "    EDIT_FLOAT,  // editable float (min/max/step)",
            "    EDIT_INT,    // editable int (min/max/step)",
            "    EDIT_BOOL,   // editable bool (toggle)",
            "    EDIT_ENUM,   // editable int with named options",
            "};",
            "",
            "// Capability-gate per menu item (mirrors modesp::display::DisplayCaps).",
            "// MENU_NODE_CAPS[i] != NONE => show node i only if the backend has that cap.",
            "enum class DisplayCap : uint8_t {",
            "    NONE = 0, COLOR, BACKLIGHT, VIDEO_PARAMS, INPUTS, BACKDROP, POWER,",
            "};",
            "",
            "struct DisplayEnumOption {",
            "    const char* label;",
            "    int32_t     value;",
            "};",
            "",
            "struct DisplayMenuNode {",
            "    const char*              label;",
            "    const char*              key;          // empty for SUBMENU",
            "    const char*              format;       // printf format for the value",
            "    const char*              unit;         // empty if none",
            "    DisplayItemType          type;",
            "    float                    min;          // EDIT_FLOAT / EDIT_INT",
            "    float                    max;",
            "    float                    step;",
            "    const DisplayEnumOption* options;      // EDIT_ENUM / bool labels",
            "    uint8_t                  option_count;",
            "    uint8_t                  first_child;  // SUBMENU: index into MENU_NODES",
            "    uint8_t                  child_count;",
            "};",
            "",
        ]

        # Option tables (one constexpr array per key that has options)
        option_arrays = {}  # key -> array name
        for s in submenus:
            for it in s["items"]:
                if it["options"] and it["key"] not in option_arrays:
                    arr_name = "OPTIONS_" + re.sub(r"[^a-zA-Z0-9]", "_", it["key"])
                    option_arrays[it["key"]] = arr_name
                    lines.append(
                        f"static constexpr DisplayEnumOption {arr_name}[] = {{")
                    for label, value in it["options"]:
                        lines.append(
                            f'    {{"{self._cstr(label)}", {value}}},')
                    lines.append("};")
                    lines.append("")

        def fmt_float(v):
            s = f"{v:g}"
            if "." not in s and "e" not in s:
                s += ".0"
            return s + "f"

        # Menu tree: submenus first (root children), then items per submenu
        lines.append("static constexpr DisplayMenuNode MENU_NODES[] = {")
        if submenus:
            next_child = len(submenus)
            for s in submenus:
                lines.append(
                    f'    {{"{self._cstr(s["label"])}", "", "", "", '
                    f"DisplayItemType::SUBMENU, 0, 0, 0, nullptr, 0, "
                    f'{next_child}, {len(s["items"])}}},')
                next_child += len(s["items"])
            for s in submenus:
                for it in s["items"]:
                    if it["options"]:
                        opts = option_arrays[it["key"]]
                        opt_count = len(it["options"])
                    else:
                        opts = "nullptr"
                        opt_count = 0
                    lines.append(
                        f'    {{"{self._cstr(it["label"])}", "{it["key"]}", '
                        f'"{self._cstr(it["format"])}", "{self._cstr(it["unit"])}", '
                        f'DisplayItemType::{it["type"]}, '
                        f'{fmt_float(it["min"])}, {fmt_float(it["max"])}, '
                        f'{fmt_float(it["step"])}, '
                        f"{opts}, {opt_count}, 0, 0}},")
        else:
            lines.append('    {"", "", "", "", DisplayItemType::SUBMENU, '
                         "0, 0, 0, nullptr, 0, 0, 0},  // empty")
        lines.append("};")
        lines.append(f"static constexpr size_t MENU_NODES_COUNT = {total_nodes};")
        lines.append("")

        # Parallel capability-gate array (same order as MENU_NODES): submenus first (NONE),
        # then items per submenu (their cap). Struct DisplayMenuNode stays unchanged → no
        # blast radius on test fixtures; MenuEngine reads this when DisplayCaps are provided.
        CAP_VALUES = {"": "NONE", "color": "COLOR", "backlight": "BACKLIGHT",
                      "video_params": "VIDEO_PARAMS", "inputs": "INPUTS",
                      "backdrop": "BACKDROP", "power": "POWER"}
        cap_seq = []
        for s in submenus:
            cap_seq.append("NONE")                       # submenu node itself — always shown
        for s in submenus:
            for it in s["items"]:
                cap = it.get("cap", "")
                if cap not in CAP_VALUES:
                    raise SystemExit(
                        f"display: menu item '{it['label']}' has unknown cap '{cap}' "
                        f"(allowed: {', '.join(k for k in CAP_VALUES if k)})")
                cap_seq.append(CAP_VALUES[cap])
        if not cap_seq:
            cap_seq = ["NONE"]
        lines.append("static constexpr DisplayCap MENU_NODE_CAPS[] = {")
        lines.append("    " + ", ".join(f"DisplayCap::{c}" for c in cap_seq) + ",")
        lines.append("};")
        lines.append("")
        lines.append("// Root children: MENU_NODES[MENU_ROOT_FIRST .. +MENU_ROOT_COUNT)")
        lines.append("static constexpr uint8_t MENU_ROOT_FIRST = 0;")
        lines.append(f"static constexpr uint8_t MENU_ROOT_COUNT = {len(submenus)};")
        lines.append("")

        # Main values
        lines.append("static constexpr DisplayMainValue MAIN_VALUES[] = {")
        if main_values:
            for v in main_values:
                lines.append(
                    f'    {{"{v["module"]}", "{self._cstr(v["label"])}", '
                    f'"{v["key"]}", "{self._cstr(v["format"])}"}},')
        else:
            lines.append('    {"", "", "", ""},  // empty')
        lines.append("};")
        lines.append(f"static constexpr size_t MAIN_VALUES_COUNT = {len(main_values)};")
        lines.append("")
        lines.append("} // namespace modesp::gen")
        lines.append("")

        return "\n".join(lines)


class FeaturesConfigGenerator:
    """Generates generated/features_config.h — constexpr feature flags."""

    def generate(self, manifests, resolver):
        """Generate features_config.h with active feature flags."""
        all_features = resolver.resolve_all(manifests)

        lines = [
            "#pragma once",
            "// Auto-generated by generate_ui.py — DO NOT EDIT",
            "",
            "#include <cstddef>",
            "",
            "namespace modesp::gen {",
            "",
            "struct FeatureConfig {",
            "    const char* module;",
            "    const char* feature;",
            "    bool active;",
            "};",
            "",
        ]

        entries = []
        for mod_name in sorted(all_features.keys()):
            features = all_features[mod_name]
            for feat_name in sorted(features.keys()):
                active = "true" if features[feat_name] else "false"
                entries.append(
                    f'    {{"{mod_name}", "{feat_name}", {active}}}')

        lines.append("static constexpr FeatureConfig FEATURES[] = {")
        if entries:
            lines.append(",\n".join(entries) + ",")
        else:
            lines.append('    {"", "", false},  // placeholder')
        lines.append("};")
        lines.append(f"static constexpr size_t FEATURES_COUNT = {max(len(entries), 1) if not entries else len(entries)};")
        lines.append("")

        # Lookup helper
        lines.extend([
            "// Lookup helper — is feature active for module?",
            "inline bool is_feature_active(const char* module, const char* feature) {",
            "    for (size_t i = 0; i < FEATURES_COUNT; i++) {",
            "        // strcmp module",
            "        const char* a = FEATURES[i].module;",
            "        const char* b = module;",
            "        while (*a && *a == *b) { a++; b++; }",
            "        if (*a != *b) continue;",
            "        // strcmp feature",
            "        a = FEATURES[i].feature;",
            "        b = feature;",
            "        while (*a && *a == *b) { a++; b++; }",
            "        if (*a == *b) return FEATURES[i].active;",
            "    }",
            "    return false;  // unknown feature → inactive",
            "}",
            "",
            "} // namespace modesp::gen",
            "",
        ])

        return "\n".join(lines)


# ═══════════════════════════════════════════════════════════════
#  Capability vocabulary (P0) — role = capability SSOT + derivation
#
#  A role declares a CAPABILITY (temperature/relay_out/…), never a driver; a driver
#  channel declares the capability it provides; matching is capability EQUALITY.
#  P0 only LOADS + VALIDATES the vocabulary and can REPORT the derived mapping — it
#  does NOT change any generated output (matching still uses category==type until P3).
# ═══════════════════════════════════════════════════════════════

_CAPABILITIES_PATH = Path(__file__).resolve().parent / "capabilities.json"

def load_capabilities(path=_CAPABILITIES_PATH):
    """Load + validate tools/capabilities.json (SSOT). Returns {name: entry}.
    Missing file → {} (capability layer absent; matching falls back to category==type).
    Raises ValueError on an invalid/inconsistent entry (fails the build)."""
    if not path.exists():
        return {}
    with open(path, "r", encoding="utf-8") as f:
        data = json.load(f)
    caps = {}
    VK = {"sensor", "actuator"}; VD = {"in", "out"}; VT = {"float", "bool", "enum", "blob"}
    for e in data.get("capabilities", []):
        name = e.get("name"); kind = e.get("kind"); direction = e.get("direction"); vt = e.get("value_type")
        if not name or not isinstance(name, str):
            raise ValueError(f"[capabilities] entry missing string 'name': {e}")
        if name in caps:
            raise ValueError(f"[capabilities] duplicate capability '{name}'")
        if kind not in VK:
            raise ValueError(f"[capabilities] '{name}': kind must be sensor|actuator, got {kind!r}")
        if direction not in VD:
            raise ValueError(f"[capabilities] '{name}': direction must be in|out, got {direction!r}")
        if vt not in VT:
            raise ValueError(f"[capabilities] '{name}': value_type must be float|bool|enum|blob, got {vt!r}")
        # kind<->direction consistency: a sensor reads (in), an actuator drives (out).
        if (kind == "sensor") != (direction == "in"):
            raise ValueError(f"[capabilities] '{name}': kind '{kind}' inconsistent with direction "
                             f"'{direction}' (sensor⇒in, actuator⇒out)")
        caps[name] = e
    return caps


# Best-effort inference of a capability from a per-CHANNEL name (address_channels value
# or a channel key). Same table the P2 migrate script will seed from; observe-only in P0.
_CHANNEL_CAP = {
    "temperature": "temperature", "temp": "temperature",
    "humidity": "humidity", "hum": "humidity",
    "battery": "battery", "batt": "battery",
    "angle": "angle", "tilted": "binary_in",
    "ax": "accel", "ay": "accel", "az": "accel",
    "presence": "presence",
    "moving": "distance", "static": "distance", "detect": "distance", "distance": "distance",
    "moving_energy": "energy", "static_energy": "energy", "energy": "energy",
    "pressure": "pressure", "voltage": "voltage", "current": "current",
    "co2": "co2", "luminance": "luminance",
}

def _infer_single_capability(drv):
    """Infer the single-value capability of a driver that has no per-channel enum."""
    cat = drv.get("category"); ht = drv.get("hardware_type")
    unit = ((drv.get("provides") or {}).get("unit") or "").strip()
    if cat == "display":
        return "display"
    if cat == "audio":
        return "audio"
    if cat == "lcd":   # player-fork: single-value fallback; gt911 declares provides.capability=touch explicitly
        return "lcd"
    if cat == "actuator":
        if drv.get("connect_name_prefix") and ht == "ble":
            return "panel"
        if ht in ("gpio_output", "i2c_expander_output"):
            return "relay_out"
        return None
    if cat == "sensor":
        if unit in ("°C", "C", "degC"):
            return "temperature"
        if ht == "onewire_bus":
            return "temperature"
        if ht in ("gpio_input", "i2c_expander_input"):
            return "binary_in"
        if drv.get("driver") == "ld2410b":
            return "presence"    # default (address="") channel; extra channels migrate in P2
        return None
    return None

def derive_driver_capabilities(drv):
    """Best-effort {channel_or_None: capability} for a driver, from EXISTING signals only
    (explicit capability fields win; else inference). None key = single-value driver.
    Observe-only: used by --report-capabilities and the P2 migrate seed; never affects output."""
    out = {}
    provides = drv.get("provides") or {}
    if provides.get("capability"):
        out[None] = provides["capability"]
    for ch in provides.get("channels") or []:
        if ch.get("channel") and ch.get("capability"):
            out[ch["channel"]] = ch["capability"]
    for ch in drv.get("address_channels") or []:
        val = ch.get("value")
        if val is None:
            continue
        cap = ch.get("capability")
        if cap:
            out[val] = cap
        elif val not in out:
            g = _CHANNEL_CAP.get(val.lower())
            if g:
                out[val] = g
    if not out:
        c = _infer_single_capability(drv)
        if c:
            out[None] = c
    return out


def report_capabilities(caps, driver_manifests, module_manifests):
    """Observe-only: print the DERIVED capability + transport for every driver channel and
    every role, so the author reviews the full P2/P3 migration up front. Writes no output."""
    print("\n" + "=" * 60)
    print("  CAPABILITY REPORT (observe-only) — derived, nothing written")
    print("=" * 60)
    print(f"\ncapabilities.json: {len(caps)} declared → {', '.join(sorted(caps)) or '(none)'}")
    unknown = set(); unresolved = []
    print("\nDRIVERS  (channel → capability [transport]):")
    for name in sorted(driver_manifests):
        drv = driver_manifests[name]
        ht = drv.get("hardware_type", "?")
        transport = "ble" if ht == "ble" else "wired"   # P0 heuristic; P2 makes it explicit
        derived = derive_driver_capabilities(drv)
        if not derived:
            unresolved.append(name)
            print(f"  {name:15} cat={drv.get('category')} hw={ht}  → (UNRESOLVED — needs manual capability)")
            continue
        for ch, cap in derived.items():
            if cap not in caps:
                unknown.add(cap)
            tag = "" if cap in caps else "   ← UNKNOWN (not in capabilities.json)"
            print(f"  {name:15} {('* (single)' if ch is None else ch):12} → {cap} [{transport}]{tag}")
    print("\nROLES  (module.role → capability):")
    for mod, requires in role_providers(module_manifests):
        for r in requires:
            cap = r.get("capability")   # declared (post-P3) wins
            if not cap:                 # legacy: derive from the first listed driver
                drivers = r.get("driver") or []
                if isinstance(drivers, str):
                    drivers = [drivers]
                for dn in drivers:
                    dm = driver_manifests.get(dn)
                    if dm:
                        dd = derive_driver_capabilities(dm)
                        if dd:
                            cap = dd.get(None) or next(iter(dd.values()))
                            break
            print(f"  {mod}.{r.get('role'):14} type={r.get('type'):9} → {cap or '(UNRESOLVED)'}")
    if unknown:
        print(f"\nWARNING: {len(unknown)} inferred capabilities NOT in capabilities.json: {sorted(unknown)}")
    if unresolved:
        print(f"WARNING: {len(unresolved)} drivers need a manual capability in P2: {unresolved}")
    print("\n(observe-only: a normal build produces byte-identical ui.json — capability is not used in matching yet)")


# ═══════════════════════════════════════════════════════════════
#  Main
# ═══════════════════════════════════════════════════════════════

def main():
    parser = argparse.ArgumentParser(description="ModESP v4 UI & Code Generator")
    parser.add_argument("--project", type=Path,
                        default=PROJECT_ROOT / "project.json",
                        help="Path to project.json")
    parser.add_argument("--modules-dir", type=Path,
                        default=PROJECT_ROOT / "modules",
                        help="Path to modules directory")
    parser.add_argument("--drivers-dir", type=Path,
                        default=PROJECT_ROOT / "drivers",
                        help="Path to drivers directory")
    parser.add_argument("--output-data", type=Path,
                        default=PROJECT_ROOT / "data",
                        help="Output path for data files (ui.json)")
    parser.add_argument("--output-gen", type=Path,
                        default=PROJECT_ROOT / "generated",
                        help="Output path for generated C++ headers")
    parser.add_argument("--minify", action="store_true",
                        help="Minify ui.json output")
    parser.add_argument("--report-capabilities", action="store_true",
                        help="Observe-only: print the derived capability/transport for every "
                             "driver + role, then exit (writes no output)")
    args = parser.parse_args()

    print("=" * 60)
    print("  ModESP v4 \u2014 UI & Code Generator")
    print("=" * 60)

    # Load project config
    if not args.project.exists():
        print(f"ERROR: {args.project} not found")
        sys.exit(1)

    with open(args.project, "r", encoding="utf-8") as f:
        project = json.load(f)

    print(f"\nProject: {project.get('project', '?')} v{project.get('version', '?')}")
    print(f"Modules: {', '.join(project.get('modules', []))}")

    # Validate module names (must be valid C++ identifiers)
    import re
    _MODULE_NAME_RE = re.compile(r'^[a-z][a-z0-9_]*$')
    for mod_name in project.get("modules", []):
        if not _MODULE_NAME_RE.match(mod_name):
            print(f"ERROR: Invalid module name '{mod_name}'")
            print(f"  Must match ^[a-z][a-z0-9_]*$ (lowercase, start with letter, only a-z 0-9 _)")
            print(f"  Examples: thermostat, heat_pump, data_logger")
            sys.exit(1)

    # Структурний шар: project.json проти схеми (типи, невідомі ключі).
    _proj_sch = schema_errors(project, "project", "project.json")
    if _proj_sch:
        for e in _proj_sch:
            print(f"ERROR: {e}")
        sys.exit(1)

    # system.mqtt_topic_root: потрапляє в C-літерали, MQTT-топіки та HA
    # node_id — жорсткий charset + ліміт довжини, щоб fixed-size буфери
    # (prefix_[80], disc_topic[96], unique_id[64]) ніколи не обрізались.
    _topic_root = (project.get("system", {}) or {}).get("mqtt_topic_root", "modesp")
    if (not re.match(r"^[a-z][a-z0-9_-]*$", _topic_root)) or len(_topic_root) > 24:
        print(f"ERROR: system.mqtt_topic_root '{_topic_root}' is invalid — "
              f"must match ^[a-z][a-z0-9_-]*$ and be at most 24 chars "
              f"(lands in MQTT topics, HA node ids and C string literals)")
        sys.exit(1)

    # Load and validate module manifests
    print("\nLoading module manifests...")
    validator = ManifestValidator()
    loader = ManifestLoader(args.modules_dir, validator)
    manifests = loader.load_all(project.get("modules", []))

    # Load and validate driver manifests
    print("\nLoading driver manifests...")
    drv_validator = DriverManifestValidator()
    drv_loader = DriverManifestLoader(args.drivers_dir, drv_validator)
    driver_manifests, cap_index = drv_loader.load_required(manifests)

    # Capability vocabulary (P0): load + validate the SSOT — an invalid/inconsistent entry
    # FAILS the build. It is NOT used in matching yet (P3), so output stays byte-identical.
    # --report-capabilities prints the derived mapping and exits before any file is written.
    try:
        capabilities = load_capabilities()
    except (ValueError, json.JSONDecodeError, OSError) as e:
        print(f"ERROR: {e}")
        sys.exit(1)
    if args.report_capabilities:
        report_capabilities(capabilities, load_all_driver_manifests(args.drivers_dir, []), manifests)
        sys.exit(0)

    # Cross-validate module <-> driver
    cross_errors = []
    cross_warnings = []
    cross_validate(manifests, driver_manifests, cross_errors, cross_warnings, capabilities, cap_index)

    # Load board.json and bindings.json for bindings page (+ cross-validate them)
    board = None
    bindings = None
    board_path = args.output_data / "board.json"
    bindings_path = args.output_data / "bindings.json"
    binding_errors = []
    binding_warnings = []
    if board_path.exists():
        try:
            with open(board_path, "r", encoding="utf-8") as f:
                board = json.load(f)
        except (json.JSONDecodeError, OSError) as e:
            binding_errors.append(f"[board] invalid JSON in {board_path.name}: {e}")
        if board is not None:
            binding_errors.extend(schema_errors(board, "board", "board"))
    if bindings_path.exists():
        try:
            with open(bindings_path, "r", encoding="utf-8") as f:
                bindings = json.load(f)
        except (json.JSONDecodeError, OSError) as e:
            binding_errors.append(f"[bindings] invalid JSON in {bindings_path.name}: {e}")
        if bindings is not None:
            binding_errors.extend(schema_errors(bindings, "bindings", "bindings"))

    # Full driver set (not just module-required) for binding validation.
    all_driver_manifests = load_all_driver_manifests(args.drivers_dir, binding_warnings)
    validate_bindings(board, bindings, all_driver_manifests, binding_errors, binding_warnings,
                      project_modules=set(project.get("modules", [])))

    # DataLogger event-id validation (before any generated/ file is written)
    loggable_errors = []
    validate_loggable(manifests, loggable_errors)

    # Print validation results
    print("\nValidating...")
    ok = validator.report()

    if not drv_validator.report():
        ok = False

    for w in cross_warnings:
        print(f"  WARNING: {w}")
    for e in cross_errors:
        print(f"  ERROR: {e}")
        ok = False
    for w in binding_warnings:
        print(f"  WARNING: {w}")
    for e in binding_errors:
        print(f"  ERROR: {e}")
        ok = False
    for e in loggable_errors:
        print(f"  ERROR: {e}")
        ok = False

    if not ok:
        print("\nERROR: Validation failed. Fix errors above.")
        sys.exit(1)
    print("  All checks passed.")

    # Advisory: drivers compiled (default y) but unused by this board's bindings.
    _unused = unused_drivers(bindings, all_driver_manifests)
    if _unused:
        print(f"\n  INFO: drivers enabled but unused by this board — run "
              f"'python tools/drivers_sync.py --prune' to shrink the binary: "
              f"{', '.join(_unused)}")

    # Create FeatureResolver if bindings available
    resolver = None
    if bindings:
        providers = [m for m in manifests if m.get("requires")]
        if not providers and bindings.get("bindings"):
            # Прив'язки є, але жоден модуль проекту не декларує requires —
            # залізо не має споживача. Fail fast замість тихого ігнорування.
            print("  ERROR: bindings.json contains bindings, but no module in "
                  "project.json declares a top-level 'requires' (role provider) "
                  "— the hardware would be silently ignored at runtime")
            sys.exit(1)
        if providers:
            resolver = FeatureResolver(bindings, providers)
            all_features = resolver.resolve_all(manifests)
            print("\nFeatures:")
            for mod_name, features in sorted(all_features.items()):
                active = [f for f, v in features.items() if v]
                inactive = [f for f, v in features.items() if not v]
                if active:
                    print(f"  {mod_name}: {', '.join(active)}")
                if inactive:
                    print(f"  {mod_name} (disabled): {', '.join(inactive)}")

    # Generate ui.json
    print("\nGenerating files...")
    ui_gen = UIJsonGenerator()
    ui_schema = ui_gen.generate(project, manifests, driver_manifests,
                                board, bindings, resolver, capabilities, cap_index)

    data_dir = args.output_data
    gen_dir = args.output_gen
    os.makedirs(data_dir, exist_ok=True)
    os.makedirs(gen_dir, exist_ok=True)

    files_written = 0

    # 1. ui.json
    ui_json_path = data_dir / "ui.json"
    with open(ui_json_path, "w", encoding="utf-8") as f:
        if args.minify:
            json.dump(ui_schema, f, ensure_ascii=False, separators=(',', ':'))
        else:
            json.dump(ui_schema, f, ensure_ascii=False, indent=2)
    ui_size = os.path.getsize(ui_json_path)
    print(f"  + {ui_json_path} ({len(ui_schema['pages'])} pages, {ui_size} bytes)")
    files_written += 1

    # 2. state_meta.h
    meta_gen = StateMetaGenerator()
    meta_h = meta_gen.generate(manifests)
    meta_path = gen_dir / "state_meta.h"
    with open(meta_path, "w", encoding="utf-8") as f:
        f.write(meta_h)
    print(f"  + {meta_path}")
    files_written += 1

    # 2b. state_enums.h (player-fork: StateEnumsGenerator)
    enums_gen = StateEnumsGenerator()
    enums_h = enums_gen.generate(manifests)
    enums_path = gen_dir / "state_enums.h"
    with open(enums_path, "w", encoding="utf-8") as f:
        f.write(enums_h)
    print(f"  + {enums_path}")
    files_written += 1

    # 3. mqtt_topics.h
    mqtt_gen = MqttTopicsGenerator()
    mqtt_h = mqtt_gen.generate(manifests, project)
    mqtt_path = gen_dir / "mqtt_topics.h"
    with open(mqtt_path, "w", encoding="utf-8") as f:
        f.write(mqtt_h)
    print(f"  + {mqtt_path}")
    files_written += 1

    # 4. display_screens.h
    display_gen = DisplayScreensGenerator()
    display_h = display_gen.generate(manifests)
    display_path = gen_dir / "display_screens.h"
    with open(display_path, "w", encoding="utf-8") as f:
        f.write(display_h)
    print(f"  + {display_path}")
    files_written += 1

    # 5. features_config.h (only if resolver available)
    if resolver:
        feat_gen = FeaturesConfigGenerator()
        feat_h = feat_gen.generate(manifests, resolver)
        feat_path = gen_dir / "features_config.h"
        with open(feat_path, "w", encoding="utf-8") as f:
            f.write(feat_h)
        print(f"  + {feat_path}")
        files_written += 1
    else:
        # Generate empty fallback so C++ compiles without bindings
        feat_path = gen_dir / "features_config.h"
        with open(feat_path, "w", encoding="utf-8") as f:
            f.write("#pragma once\n"
                    "// Auto-generated by generate_ui.py — DO NOT EDIT\n"
                    "// No bindings.json found — all features default to false\n\n"
                    "#include <cstddef>\n\n"
                    "namespace modesp::gen {\n\n"
                    "struct FeatureConfig {\n"
                    "    const char* module;\n"
                    "    const char* feature;\n"
                    "    bool active;\n"
                    "};\n\n"
                    "static constexpr FeatureConfig FEATURES[] = {\n"
                    '    {"", "", false},  // placeholder\n'
                    "};\n"
                    "static constexpr size_t FEATURES_COUNT = 0;\n\n"
                    "inline bool is_feature_active(const char*, const char*) {\n"
                    "    return false;\n"
                    "}\n\n"
                    "} // namespace modesp::gen\n")
        print(f"  + {feat_path} (fallback — no bindings)")
        files_written += 1

    # ── 6. Module registry (auto-registration) ─────────────
    # Convention: module "thermostat" → ThermostatModule in thermostat_module.h
    module_names = project.get("modules", [])

    # Step 4 (Q-discussion): Filter recipes for C++ binding generation.
    # Recipe modules (`module_type: "recipe"`) — manifest-only, no C++ class.
    # State keys / UI widgets / MQTT topics still processed via existing pipeline
    # (their manifests are у `manifests` list and contribute normally).
    # But module_includes.h, module_instances.h, module_register.h, modules.cmake
    # MUST exclude recipes (no _module.h файлу, no class to instantiate).
    recipe_module_names: set[str] = {
        m["module"] for m in manifests
        if m.get("module_type") == "recipe"
    }
    cpp_module_names = [m for m in module_names if m not in recipe_module_names]
    if recipe_module_names:
        print(f"  (recipes excluded from C++ binding: {sorted(recipe_module_names)})")

    # Class name overrides for modules that don't follow simple convention
    # (e.g. "datalogger" → "DataLoggerModule", not "DataloggerModule")
    _class_overrides = {}
    for m in manifests:
        mod = m.get("module", "")
        if "class_name" in m:
            _class_overrides[mod] = m["class_name"]

    def _to_class_name(mod_name):
        """thermostat → ThermostatModule, datalogger → DataLoggerModule"""
        if mod_name in _class_overrides:
            return _class_overrides[mod_name]
        return ''.join(w.capitalize() for w in mod_name.split('_')) + 'Module'

    def _to_header(mod_name):
        """thermostat → thermostat_module.h"""
        return f"{mod_name}_module.h"

    # 6a. module_includes.h (recipes excluded — no .h file)
    inc_lines = [
        "#pragma once",
        "// Auto-generated from project.json — DO NOT EDIT",
        f"// C++ modules: {', '.join(cpp_module_names)}",
        "",
    ]
    for mod in cpp_module_names:
        inc_lines.append(f'#include "{_to_header(mod)}"')

    inc_path = gen_dir / "module_includes.h"
    with open(inc_path, "w", encoding="utf-8") as f:
        f.write('\n'.join(inc_lines) + '\n')
    print(f"  + {inc_path}")
    files_written += 1

    # 6b. module_instances.h (recipes excluded — no class)
    inst_lines = [
        "#pragma once",
        "// Auto-generated from project.json — DO NOT EDIT",
        "",
    ]
    for mod in cpp_module_names:
        inst_lines.append(f"static {_to_class_name(mod)}  {mod};")

    inst_path = gen_dir / "module_instances.h"
    with open(inst_path, "w", encoding="utf-8") as f:
        f.write('\n'.join(inst_lines) + '\n')
    print(f"  + {inst_path}")
    files_written += 1

    # 6c. module_register.h (recipes excluded — no register call)
    # Sort by priority from manifest (lower = earlier)
    mod_priorities = []
    for mod in cpp_module_names:
        prio = 2  # default
        for m in manifests:
            if m.get("module") == mod:
                prio = m.get("priority", 2)
                break
        mod_priorities.append((prio, mod))
    mod_priorities.sort(key=lambda x: x[0])

    reg_lines = [
        "#pragma once",
        "// Auto-generated from project.json — DO NOT EDIT",
        "// Sorted by manifest priority (lower = earlier)",
        "",
        "#include \"modesp/app.h\"",
        "",
        "inline void modesp_register_modules(modesp::App& app) {",
    ]
    for prio, mod in mod_priorities:
        reg_lines.append(f"    app.modules().register_module({mod});  // priority={prio}")
    reg_lines.append("}")

    reg_path = gen_dir / "module_register.h"
    with open(reg_path, "w", encoding="utf-8") as f:
        f.write('\n'.join(reg_lines) + '\n')
    print(f"  + {reg_path}")
    files_written += 1

    # 6d. modules.cmake — PRIV_REQUIRES list for main/CMakeLists.txt
    # Recipes excluded — they're not ESP-IDF components (no CMakeLists.txt)
    cmake_lines = [
        "# Auto-generated from project.json — DO NOT EDIT",
        f"set(PRODUCT_MODULES {' '.join(cpp_module_names)})",
    ]
    cmake_path = gen_dir / "modules.cmake"
    with open(cmake_path, "w", encoding="utf-8") as f:
        f.write('\n'.join(cmake_lines) + '\n')
    print(f"  + {cmake_path}")
    files_written += 1

    # ── 6e. Driver glue — optional-driver support ──────────
    # Scan ALL drivers/ (not only those referenced by modules) so every driver
    # gets a menuconfig toggle + auto-wiring with ZERO manual edits:
    #   generated/Kconfig.drivers      → config MODESP_DRIVER_<NAME> per driver
    #   generated/drivers.cmake        → REQUIRES list for modesp_hal
    #   generated/driver_register_all.h→ guarded register-all (skips disabled)
    driver_dirs = sorted(p.parent for p in args.drivers_dir.glob("*/manifest.json"))
    drivers_meta = []  # (name, category, description, hardware_type)
    for ddir in driver_dirs:
        try:
            dm = json.loads((ddir / "manifest.json").read_text(encoding="utf-8"))
        except Exception as e:
            print(f"  WARNING: skipping driver dir '{ddir.name}': {e}")
            continue
        name = dm.get("driver", ddir.name)
        if not re.match(r"^[a-z][a-z0-9_]*$", name):
            print(f"  WARNING: driver '{name}' has invalid name — skipped")
            continue
        category = dm.get("category", "sensor")
        if category not in DRIVER_COMPONENT_CATEGORIES:
            # Невідома категорія — валідатор уже зарепортив; без компонента.
            continue
        drivers_meta.append((name, category, dm.get("description", name),
                             dm.get("hardware_type", "")))

    # Driver Kconfig — one toggle per driver (default y → existing behaviour).
    # Written to components/modesp_hal/Kconfig so ESP-IDF auto-discovers it as a
    # component Kconfig (reliable; no fragile orsource path resolution).
    #
    # Transport dependencies, derived from hardware_type in the driver manifest
    # (manifest-driven, no per-driver hardcode): a BLE driver consumes types and
    # callbacks that live behind the BLE *central* role (observer scan + connect —
    # BleCentral / adv_decoder / BlePanel are all under CONFIG_MODESP_BLE_CENTRAL),
    # so it must not compile when that role is off. 'depends on' auto-disables the
    # driver toggle, and the bound-driver gate in modesp_hal/CMakeLists.txt then
    # reports boards that actually need it. (CENTRAL implies ENABLE in Kconfig.)
    HW_TYPE_KCONFIG_DEPS = {"ble": "MODESP_BLE_CENTRAL",
                            "dsi_display": "SOC_MIPI_DSI_SUPPORTED",   # player-fork: MIPI-DSI panels only exist on the P4
                            "encoder": "SOC_PCNT_SUPPORTED"}          # player-fork: PCNT-based encoder
    kcfg = [
        "# Auto-generated from drivers/*/manifest.json by tools/generate_ui.py",
        "# DO NOT EDIT — regenerated on every build.",
        'menu "ModESP Drivers"',
        "",
    ]
    for name, category, desc, hw_type in drivers_meta:
        kcfg += [
            f"    config MODESP_DRIVER_{name.upper()}",
            f'        bool "{name} driver"',
            "        default y",
        ]
        dep = HW_TYPE_KCONFIG_DEPS.get(hw_type)
        if dep:
            kcfg.append(f"        depends on {dep}")
        kcfg += [
            "        help",
            f"            Compile and register the '{name}' {category} driver.",
            "            Disable to exclude it from the firmware (smaller binary).",
            "",
        ]
    kcfg.append("endmenu")
    kcfg_path = args.drivers_dir.parent / "components" / "modesp_hal" / "Kconfig"
    with open(kcfg_path, "w", encoding="utf-8") as f:
        f.write('\n'.join(kcfg) + '\n')
    print(f"  + {kcfg_path}")
    files_written += 1

    # ── 6e-2. Board Kconfig — generated from boards/*/board.json ──────────
    # One choice entry per board folder, so adding boards/my_board/ requires ZERO
    # framework edits (same drop-in contract as drivers). Written to
    # main/Kconfig.boards; main/Kconfig.projbuild pulls it in via rsource.
    # Symbol names derive from the folder: boards/dev/ → MODESP_BOARD_DEV.
    boards_root = args.drivers_dir.parent / "boards"
    board_entries = []  # (dirname, prompt, description)
    for bjson in sorted(boards_root.glob("*/board.json")):
        bdir = bjson.parent.name
        if not re.match(r"^[a-z][a-z0-9_]*$", bdir):
            print(f"  WARNING: board dir '{bdir}' has invalid name "
                  f"(must match ^[a-z][a-z0-9_]*$) — skipped")
            continue
        if bdir == "dir":
            # MODESP_BOARD_DIR is the string symbol carrying the folder name —
            # a board literally named 'dir' would collide with it.
            print("  WARNING: board dir 'dir' is reserved "
                  "(collides with MODESP_BOARD_DIR) — skipped")
            continue
        try:
            bdata = json.loads(bjson.read_text(encoding="utf-8"))
        except Exception as e:
            print(f"  WARNING: skipping board '{bdir}': {e}")
            continue
        # Kconfig prompt is a quoted single-line string; help is one indented
        # line — sanitize both (escape, collapse to a single line).
        prompt = (bdata.get("display_name") or bdata.get("board") or bdir)
        prompt = " ".join(prompt.replace("\\", "/").replace('"', "'").split())
        desc_lines = bdata.get("description", "").splitlines()
        board_entries.append((bdir, prompt,
                              desc_lines[0].strip() if desc_lines else ""))
    if not board_entries:
        print(f"  ERROR: no valid boards found in {boards_root}/*/board.json")
        sys.exit(1)
    default_board = ("dev" if any(d == "dev" for d, _, _ in board_entries)
                     else board_entries[0][0])
    bkcfg = [
        "# Auto-generated from boards/*/board.json by tools/generate_ui.py",
        "# DO NOT EDIT — regenerated on every build.",
        "choice MODESP_BOARD",
        '    prompt "Target board"',
        f"    default MODESP_BOARD_{default_board.upper()}",
        "    help",
        "        Select the target hardware board. Determines which board.json",
        "        and bindings.json are copied to data/ during build.",
        "",
    ]
    for bdir, prompt, desc in board_entries:
        bkcfg += [
            f"    config MODESP_BOARD_{bdir.upper()}",
            f'        bool "{prompt} ({bdir})"',
        ]
        if desc:
            bkcfg += ["        help", f"            {desc}"]
        bkcfg.append("")
    bkcfg += ["endchoice", "", "config MODESP_BOARD_DIR", "    string"]
    for bdir, _, _ in board_entries:
        bkcfg.append(f'    default "{bdir}" if MODESP_BOARD_{bdir.upper()}')
    board_kcfg_path = args.drivers_dir.parent / "main" / "Kconfig.boards"
    with open(board_kcfg_path, "w", encoding="utf-8") as f:
        f.write('\n'.join(bkcfg) + '\n')
    print(f"  + {board_kcfg_path}")
    files_written += 1

    # drivers.cmake — full driver list for modesp_hal PRIV_REQUIRES
    dcmake_path = gen_dir / "drivers.cmake"
    with open(dcmake_path, "w", encoding="utf-8") as f:
        f.write("# Auto-generated from drivers/*/manifest.json — DO NOT EDIT\n")
        f.write(f"set(MODESP_ALL_DRIVERS {' '.join(n for n, _, _, _ in drivers_meta)})\n")
    print(f"  + {dcmake_path}")
    files_written += 1

    # required_drivers.cmake — distinct driver set the ACTIVE board's bindings use.
    # modesp_hal/CMakeLists.txt FATAL_ERRORs if any of these is disabled in menuconfig.
    driver_names = {n for n, _, _, _ in drivers_meta}
    bound_drivers = []
    if bindings:
        for b in bindings.get("bindings", []):
            d = b.get("driver")
            if d in driver_names and d not in bound_drivers:
                bound_drivers.append(d)
    req_path = gen_dir / "required_drivers.cmake"
    with open(req_path, "w", encoding="utf-8") as f:
        f.write("# Auto-generated from the active board's bindings.json — DO NOT EDIT\n")
        f.write(f"set(MODESP_BOUND_DRIVERS {' '.join(sorted(bound_drivers))})\n")
    print(f"  + {req_path}")
    files_written += 1

    # driver_register_all.h — guarded extern decls + register-all body.
    # Uses extern "C" symbol modesp_register_driver_<name>() defined by each
    # driver via MODESP_REGISTER_SENSOR/ACTUATOR — no class names needed here.
    reg = [
        "#pragma once",
        "// Auto-generated from drivers/*/manifest.json — DO NOT EDIT",
        "",
        '#include "sdkconfig.h"',
        '#include "modesp/hal/driver_registry.h"',
        "",
        "// Compile-time guard: turn a silent registry overflow (the 17th driver",
        "// type) into a build error instead of a runtime 'unknown driver' warning.",
        f"static_assert({len(drivers_meta)} <= modesp::DriverRegistry::MAX_DRIVER_TYPES,",
        '              "Too many driver types — raise MAX_DRIVER_TYPES in driver_registry.h");',
        "",
        'extern "C" {',
    ]
    for name, _, _, _ in drivers_meta:
        reg += [
            f"#ifdef CONFIG_MODESP_DRIVER_{name.upper()}",
            f"void modesp_register_driver_{name}(void);",
            "#endif",
        ]
    reg += ["}", "", "inline void modesp_register_all_drivers(void) {"]
    for name, _, _, _ in drivers_meta:
        reg += [
            f"#ifdef CONFIG_MODESP_DRIVER_{name.upper()}",
            f"    modesp_register_driver_{name}();",
            "#endif",
        ]
    reg.append("}")
    reg_drv_path = gen_dir / "driver_register_all.h"
    with open(reg_drv_path, "w", encoding="utf-8") as f:
        f.write('\n'.join(reg) + '\n')
    print(f"  + {reg_drv_path}")
    files_written += 1

    # ── 7. DataLogger channels (manifest-driven) ───────────
    log_channels = collect_log_channels(manifests)  # same source as chart-widget config

    # Always (re)write the header — even with zero channels — so removing the last
    # loggable channel can't leave a stale datalogger_channels.h that the
    # unconditional #include in datalogger_module.h would still compile against.
    MAX_CHANNELS = 6  # Fixed for binary compatibility
    if len(log_channels) > MAX_CHANNELS:
        # Channels beyond runtime capacity would silently never log — in an
        # industrial logger that is silent data loss, so fail the build instead.
        print(f"  ERROR: {len(log_channels)} datalogger channels declared, but "
              f"MAX_LOG_CHANNELS={MAX_CHANNELS} — channels beyond capacity would "
              f"silently never be logged. Remove channels or raise the capacity.")
        sys.exit(1)

    ch_lines = [
        "#pragma once",
        "// Auto-generated from module manifests — DO NOT EDIT",
        "",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        "namespace modesp::gen {",
        "",
        "struct LogChannel {",
        "    const char* id;",
        "    const char* state_key;",
        "    const char* enable_key;   // nullptr = always enabled",
        "    const char* requires_key; // nullptr = no hardware condition",
        "    bool default_enabled;",
        "};",
        "",
        f"static constexpr size_t MAX_LOG_CHANNELS = {MAX_CHANNELS};",
        f"static constexpr size_t LOG_CHANNELS_COUNT = {len(log_channels)};",
        "",
        "static constexpr LogChannel LOG_CHANNELS[] = {",
    ]
    for ch in log_channels:
        ek = f'"{ch["enable_key"]}"' if ch["enable_key"] else "nullptr"
        rk = f'"{ch["requires"]}"' if ch["requires"] else "nullptr"
        de = "true" if ch["default"] else "false"
        ch_lines.append(f'    {{"{ch["id"]}", "{ch["state_key"]}", {ek}, {rk}, {de}}},')
    if not log_channels:
        # Zero-length C++ arrays are non-standard; emit one never-indexed sentinel
        # (LOG_CHANNELS_COUNT == 0 guards every access).
        ch_lines.append('    {"", "", nullptr, nullptr, false},  // sentinel (COUNT==0)')
    ch_lines.extend([
        "};",
        "",
        "} // namespace modesp::gen",
    ])

    ch_path = gen_dir / "datalogger_channels.h"
    with open(ch_path, "w", encoding="utf-8") as f:
        f.write('\n'.join(ch_lines) + '\n')
    print(f"  + {ch_path} ({len(log_channels)} channels)")
    files_written += 1

    # ── 8. DataLogger events (manifest-driven) ─────────────
    # Event ids were already validated in the pre-write gate (validate_loggable),
    # so this only collects + emits — no sys.exit mid-generation.
    log_events = collect_log_events(manifests)  # same source as chart-widget config

    # Always (re)write the header — same stale-header reasoning as channels above.
    ev_lines = [
        "#pragma once",
        "// Auto-generated from module manifests — DO NOT EDIT",
        "",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        "namespace modesp::gen {",
        "",
        "enum class EdgeType : uint8_t { RISING = 0, FALLING = 1, BOTH = 2 };",
        "",
        "struct LogEvent {",
        "    uint8_t     id;          // unique event ID (stable across builds)",
        "    const char* state_key;   // SharedState key to watch",
        "    EdgeType    edge;        // which edge triggers the event",
        "    const char* label;       // default label (or label_on for BOTH)",
        "    const char* label_off;   // label for falling edge (BOTH only, nullptr otherwise)",
        "};",
        "",
        "// System events (not edge-detect, logged explicitly in code)",
        "static constexpr uint8_t EVENT_POWER_ON    = 10;",
        "static constexpr uint8_t EVENT_ALARM_CLEAR = 7;",
        "",
        f"static constexpr size_t LOG_EVENTS_COUNT = {len(log_events)};",
        "",
        "static constexpr LogEvent LOG_EVENTS[] = {",
    ]
    for ev in log_events:
        edge_str = {"rising": "EdgeType::RISING", "falling": "EdgeType::FALLING", "both": "EdgeType::BOTH"}[ev["edge"]]
        if ev["edge"] == "both" and ev["label_on"]:
            label = ev["label_on"]
            label_off = f'"{ev["label_off"]}"' if ev["label_off"] else "nullptr"
        else:
            label = ev["label"]
            label_off = "nullptr"
        ev_lines.append(f'    {{{ev["id"]}, "{ev["state_key"]}", {edge_str}, "{label}", {label_off}}},')
    if not log_events:
        # Zero-length C++ arrays are non-standard; emit one never-indexed sentinel
        # (LOG_EVENTS_COUNT == 0 guards every access).
        ev_lines.append('    {0, "", EdgeType::RISING, "", nullptr},  // sentinel (COUNT==0)')
    ev_lines.extend([
        "};",
        "",
        "} // namespace modesp::gen",
    ])

    ev_path = gen_dir / "datalogger_events.h"
    with open(ev_path, "w", encoding="utf-8") as f:
        f.write('\n'.join(ev_lines) + '\n')
    print(f"  + {ev_path} ({len(log_events)} events)")
    files_written += 1

    # ── i18n: build language packs ───────────────────────────
    i18n_out = args.output_data / "www" / "i18n"
    i18n_out.mkdir(parents=True, exist_ok=True)

    # Discover available languages from module i18n files
    available_langs = set()
    for mod_name in project.get("modules", []):
        i18n_dir = args.modules_dir / mod_name / "i18n"
        if i18n_dir.is_dir():
            for f in i18n_dir.glob("*.json"):
                available_langs.add(f.stem)
    # player-fork: system translation packs (data/i18n/system_<lang>.json) also define a
    # language, so the built-in network/system pages ship translated even with zero modules.
    for f in (args.output_data / "i18n").glob("system_*.json"):
        available_langs.add(f.stem[len("system_"):])

    for lang in sorted(available_langs):
        merged = {}
        # Merge per-module i18n files
        for mod_name in project.get("modules", []):
            i18n_file = args.modules_dir / mod_name / "i18n" / f"{lang}.json"
            if i18n_file.is_file():
                with open(i18n_file, "r", encoding="utf-8") as f:
                    mod_i18n = json.load(f)
                merged.update(mod_i18n)

        # Merge system strings (per-language)
        sys_i18n_path = args.output_data / "i18n" / f"system_{lang}.json"
        sys_i18n = {}
        if sys_i18n_path.is_file():
            with open(sys_i18n_path, "r", encoding="utf-8") as f:
                sys_i18n = json.load(f)
            # System strings use UA key → EN value format
            for ua_key, en_val in sys_i18n.items():
                merged[f"sys.{ua_key}"] = en_val

        # Also include chrome strings from webui/src/i18n/{lang}.js if exists
        chrome_path = PROJECT_ROOT / "webui" / "src" / "i18n" / f"{lang}.js"
        # Chrome strings stay in JS files (imported by Svelte), not merged here

        # Build flat reverse map: every EN value keyed by its UA original
        reverse = {}

        # Simple approach: scan ALL values in merged dict
        # and create reverse entries for any value found in ui.json
        all_ua_texts = set()
        for page in ui_schema.get("pages", []):
            if page.get("title"): all_ua_texts.add(page["title"])
            for card in page.get("cards", []):
                for f in ("title", "subtitle"):
                    if card.get(f): all_ua_texts.add(card[f])
                for w in card.get("widgets", []):
                    for f in ("description", "unit", "on_label", "off_label", "label", "confirm", "disabled_hint"):
                        if w.get(f): all_ua_texts.add(w[f])
                    for opt in w.get("options", []):
                        if opt.get("label"): all_ua_texts.add(opt["label"])
                        if opt.get("disabled_hint"): all_ua_texts.add(opt["disabled_hint"])
                    if w.get("actions"):
                        for a in w["actions"]:
                            if a.get("label"): all_ua_texts.add(a["label"])
                            if a.get("confirm"): all_ua_texts.add(a["confirm"])

        # For each UA text in ui.json, find ANY value in merged that matches
        # Build inverted index: EN value → structured key
        en_to_ua = {}  # structured_key → EN value
        for k, v in merged.items():
            en_to_ua[k] = v

        # Now: for each UA text, search module i18n files for a match
        # Module i18n files map structured_key → EN, and we know the UA text
        # We need: UA text → EN text
        # Strategy: for each module, load its i18n and its manifest to build UA→EN
        for mod_name in project.get("modules", []):
            mf_path = args.modules_dir / mod_name / "manifest.json"
            i18n_file = args.modules_dir / mod_name / "i18n" / f"{lang}.json"
            if not mf_path.is_file() or not i18n_file.is_file():
                continue
            with open(mf_path, "r", encoding="utf-8") as f:
                mf = json.load(f)
            with open(i18n_file, "r", encoding="utf-8") as f:
                mod_en = json.load(f)

            # State descriptions
            for key, info in mf.get("state", {}).items():
                for field in ("description", "unit", "on_label", "off_label"):
                    ua = info.get(field, "")
                    ek = f"state.{key}.{field}"
                    if ua and ek in mod_en:
                        reverse[ua] = mod_en[ek]
                # Options
                for opt in info.get("options", []):
                    ua = opt.get("label", "")
                    ek = f"state.{key}.options.{opt.get('value','')}"
                    if ua and ek in mod_en:
                        reverse[ua] = mod_en[ek]

            # UI page/card titles
            ui = mf.get("ui", {})
            pk = f"page.{mod_name}.title"
            if ui.get("page") and pk in mod_en:
                reverse[ui["page"]] = mod_en[pk]
            for ci, card in enumerate(ui.get("cards", [])):
                cid = card.get("id", f"card{ci}")
                for field in ("title", "subtitle"):
                    ua = card.get(field, "")
                    ek = f"card.{mod_name}.{cid}.{field}"
                    if ua and ek in mod_en:
                        reverse[ua] = mod_en[ek]
                # Widget labels
                for w in card.get("widgets", []):
                    for field in ("label", "confirm"):
                        ua = w.get(field, "")
                        ek = f"widget.{w.get('key','')}.{field}"
                        if ua and ek in mod_en:
                            reverse[ua] = mod_en[ek]

            # Constraints disabled_hint
            for key, c in mf.get("constraints", {}).items():
                for val_str, rule in c.get("values", {}).items():
                    ua = rule.get("disabled_hint", "")
                    ek = f"constraint.{key}.{val_str}.disabled_hint"
                    if ua and ek in mod_en:
                        reverse[ua] = mod_en[ek]

        # System strings (UA key → translated value)
        for ua_key, trans_val in sys_i18n.items():
            reverse[ua_key] = trans_val
        # Merge reverse into strings (frontend uses this for card/page titles)
        merged.update(reverse)

        # Write merged language pack
        lang_pack = {
            "lang": lang,
            "version": 1,
            "keys": len(merged),
            "strings": merged,
        }
        out_path = i18n_out / f"{lang}.json"
        with open(out_path, "w", encoding="utf-8") as f:
            json.dump(lang_pack, f, ensure_ascii=False, separators=(",", ":"))
        print(f"  + {out_path} ({len(merged)} keys, {out_path.stat().st_size} bytes)")
        files_written += 1

    # Write i18n manifest. The SOURCE language (project.system.source_lang, default
    # "uk") is the language the manifests are authored in — its strings live in ui.json
    # directly; the other languages ship translation packs. Source goes first and is the
    # default; it is never duplicated even if it also has a translation file.
    source_lang = project.get("system", {}).get("source_lang", "uk")
    other_langs = sorted(l for l in available_langs if l != source_lang)
    i18n_manifest = {"languages": [source_lang] + other_langs, "default": source_lang}
    manifest_path = i18n_out / "manifest.json"
    # player-fork: newline="\n" — the file is tracked and .gitattributes says LF; on Windows
    # the default text mode wrote CRLF, and every build then showed a modified file with an
    # empty diff.
    with open(manifest_path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(i18n_manifest, f, ensure_ascii=False, indent=2)
    print(f"  + {manifest_path} (languages: {i18n_manifest['languages']})")
    files_written += 1

    # Validate: check that module i18n covers all translatable fields
    _translatable = set()
    for page in ui_schema.get("pages", []):
        for card in page.get("cards", []):
            for w in card.get("widgets", []):
                i18n_key = w.get("i18n_key")
                if i18n_key:
                    _translatable.add(i18n_key + ".description")
    # Simple coverage report
    for lang in sorted(available_langs):
        lang_file = i18n_out / f"{lang}.json"
        if lang_file.is_file():
            with open(lang_file, "r", encoding="utf-8") as f:
                pack = json.load(f)
            keys = set(pack.get("strings", {}).keys())
            # Count state description keys
            desc_keys = {k for k in keys if k.startswith("state.") and k.endswith(".description")}
            print(f"  ✓ {lang}: {len(keys)} keys ({len(desc_keys)} descriptions)")

    # Summary
    total_keys = sum(len(m.get("state", {})) for m in manifests)
    total_widgets = sum(
        sum(len(c.get("widgets", []))
            for c in m.get("ui", {}).get("cards", []))
        for m in manifests
    )
    print(f"\n{'=' * 60}")
    print(f"  {len(manifests)} modules, {total_keys} state keys, "
          f"{len(ui_schema['pages'])} pages, {files_written} files generated")
    print(f"{'=' * 60}")


if __name__ == "__main__":
    main()
