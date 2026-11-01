"""
test_user_data.py — what a backup of the panel's own files saves, and what `seed` carries.

tools/user_data.py reads the panel with esptool and the LittleFS image with littlefs-python.
Neither runs here: a stand-in plays the panel, and what is tested is what the tool does
around them — which files end up in build/user_data, and from there in data/.
"""

import argparse
import importlib.util
import os
import shutil
import subprocess

import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


@pytest.fixture
def tool(tmp_path, monkeypatch):
    """The tool with its working tree moved into tmp_path."""
    spec = importlib.util.spec_from_file_location(
        "user_data", os.path.join(ROOT, "tools", "user_data.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    monkeypatch.setattr(mod, "ROOT", str(tmp_path))
    monkeypatch.setattr(mod, "DATA", str(tmp_path / "data"))
    monkeypatch.setattr(mod, "SAVE", str(tmp_path / "build" / "user_data"))
    monkeypatch.setattr(mod, "IMAGE", str(tmp_path / "build" / "device_data.bin"))
    (tmp_path / "data").mkdir()
    shutil.copy(os.path.join(ROOT, "partitions_p4_16mb.csv"), tmp_path)
    return mod


class Panel:
    """esptool and the LittleFS reader in one: a panel that has `files`, and a read of them
    that fails after writing them when `fail` is set."""

    def __init__(self, files, fail=False):
        self.files, self.fail = files, fail

    def __call__(self, argv, **_):
        if "read_flash" in argv:
            open(argv[-1], "wb").close()
            return 0
        assert argv[1] == "-c", argv                  # the reader: code, image, out dir, names
        for name, data in self.files.items():
            with open(os.path.join(argv[4], name), "wb") as f:
                f.write(data)
        if self.fail:
            raise subprocess.CalledProcessError(1, argv[:2])
        return 0


def backup(tool, monkeypatch, panel):
    monkeypatch.setattr(subprocess, "check_call", panel)
    tool.cmd_backup(argparse.Namespace(partition="data", port="COM12", baud=921600))


def saved(tool):
    out = {}
    for name in sorted(os.listdir(tool.SAVE)):
        with open(os.path.join(tool.SAVE, name), "rb") as f:
            out[name] = f.read()
    return out


def test_a_backup_replaces_the_saved_set_whole(tool, monkeypatch, tmp_path):
    backup(tool, monkeypatch, Panel({"eq.json": b"{1}", "stations.json": b"[1]",
                                     "devices.json": b"{}"}))
    # the next panel has no station list and no devices: none may be carried as its own
    backup(tool, monkeypatch, Panel({"eq.json": b"{2}"}))
    assert saved(tool) == {"eq.json": b"{2}"}

    tool.cmd_seed(argparse.Namespace())
    assert sorted(os.listdir(tmp_path / "data")) == ["eq.json"]
    assert (tmp_path / "data" / "eq.json").read_bytes() == b"{2}"


def test_a_failed_backup_leaves_the_last_good_set(tool, monkeypatch, tmp_path):
    good = {"eq.json": b"{1}", "stations.json": b"[1]"}
    backup(tool, monkeypatch, Panel(good))
    with pytest.raises(subprocess.CalledProcessError):
        backup(tool, monkeypatch, Panel({"stations.json": b"[half]"}, fail=True))
    assert saved(tool) == good
    assert sorted(os.listdir(tmp_path / "build")) == ["device_data.bin", "user_data"]
