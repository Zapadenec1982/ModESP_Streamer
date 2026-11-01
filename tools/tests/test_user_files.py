"""
test_user_files.py — the firmware and the backup tool must agree on what a user file is.

There are two lists of "files a person makes": UserStore::FILES in the firmware, which the
first boot after the partition split migrates into /userdata, and USER_FILES in
tools/user_data.py, which is what a backup carries off the panel. A name in one and not the
other is a file that is silently not saved, or one that is migrated and never written.

Both mistakes were already in the tree when this test was written:

  * "last_dir" was in the tool, and nothing has ever written it. The player
    writes /data/last_track through a constant still called LAST_DIR_PATH.
  * "devices.json" is written by POST /api/devices and was in neither list, so a backup left
    every subscribed remote device behind.
"""

import importlib.util
import os
import re

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

_spec = importlib.util.spec_from_file_location(
    "user_data", os.path.join(ROOT, "tools", "user_data.py"))
user_data = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(user_data)

STORE_CPP = os.path.join(ROOT, "components", "user_store", "src", "user_store.cpp")


def firmware_files():
    """The names in UserStore::FILES, read out of the source rather than guessed."""
    with open(STORE_CPP, encoding="utf-8") as f:
        src = f.read()
    body = src[src.index("const char* const UserStore::FILES[]"):]
    body = body[body.index("{"):body.index("};")]
    return re.findall(r'"([^"]+)"', body)


def test_the_two_lists_are_the_same_set():
    assert sorted(firmware_files()) == sorted(user_data.USER_FILES), (
        "UserStore::FILES and tools/user_data.py USER_FILES disagree. A name in one and not "
        "the other is either a file no backup saves, or one the migration copies and nothing "
        "writes.")


def test_no_build_output_is_treated_as_a_user_file():
    """A flash is supposed to replace these; carrying one forward outlives its board."""
    for name in set(firmware_files()) | set(user_data.USER_FILES):
        assert name not in ("board.json", "bindings.json", "ui.json", "index.html")


def test_every_user_file_is_actually_written_by_the_firmware():
    """A name nothing writes is a name someone will keep maintaining for no reason."""
    writers = {
        # name: (file that writes it, the token that proves it)
        "eq.json":       ("components/audio_dsp/src/eq_config.cpp", "safe_file::save"),
        "stations.json": ("modules/radio/src/radio_module.cpp", "s_stations_path"),
        "last_track":    ("modules/player/src/player_module.cpp", "s_last_track_path"),
        "devices.json":  ("components/modesp_net/src/http_service.cpp", "devices.json"),
        "eq_custom.json": ("modules/audio/src/audio_module.cpp", "eq_custom.json"),
    }
    for name in firmware_files():
        assert name in writers, "%s is migrated but this test does not know who writes it" % name
        path, token = writers[name]
        with open(os.path.join(ROOT, path), encoding="utf-8", errors="replace") as f:
            src = f.read()
        assert token in src, "%s no longer writes %s the way this test expects" % (path, name)


def test_the_resume_point_is_a_track_and_the_phantom_last_dir_is_gone():
    assert "last_dir" not in user_data.USER_FILES
    assert "last_dir" not in firmware_files()
    with open(os.path.join(ROOT, "modules", "player", "src", "player_module.cpp"),
              encoding="utf-8", errors="replace") as f:
        src = f.read()
    assert '"/data/last_track"' in src, "the default resume path changed shape"
