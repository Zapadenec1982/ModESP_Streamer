"""
test_user_data.py — what a backup of the panel's own files reads, saves, and `seed` carries.

tools/user_data.py reads the panel with esptool and the LittleFS image with littlefs-python.
Neither runs here: a stand-in plays the panel, and what is tested is what the tool does
around them — where on the flash it reads, which files end up in build/user_data, and from
there in data/.
"""

import argparse
import hashlib
import importlib.util
import os
import re
import struct
import subprocess

import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

# The table of a panel that has not been moved to the two-partition layout yet: no
# `userdata`, and `data` where the checkout has ota_1 and the end of the flash.
OLD_LAYOUT = [
    ("nvs", 0x11000, 0x8000), ("otadata", 0x19000, 0x2000), ("phy_init", 0x1B000, 0x1000),
    ("coredump", 0x1C000, 0x10000), ("ota_0", 0x30000, 0x300000),
    ("ota_1", 0x330000, 0x300000), ("slave_fw", 0x630000, 0x200000),
    ("data", 0x830000, 0x7D0000),
]


def checkout_rows():
    """(name, offset, size) of partitions_p4_16mb.csv, the layout an upgraded panel has."""
    rows = []
    with open(os.path.join(ROOT, "partitions_p4_16mb.csv"), encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith("#"):
                cols = [c.strip() for c in line.split(",")]
                rows.append((cols[0], int(cols[3], 0), int(cols[4], 0)))
    return rows


def table_image(rows):
    """A partition table as ESP-IDF writes one (PartitionTable.to_binary, gen_esp32part.py):
    a 32-byte entry per row, the MD5 entry, FF to 0xC00. Type and subtype play no part in
    finding a partition by name, so every row is data/spiffs here."""
    out = b"".join(struct.pack("<2sBBLL16sL", b"\xaa\x50", 0x01, 0x82, off, size,
                               name.encode(), 0) for name, off, size in rows)
    out += b"\xeb\xeb" + b"\xff" * 14 + hashlib.md5(out).digest()
    return out + b"\xff" * (0xC00 - len(out))


@pytest.fixture
def tool(tmp_path, monkeypatch):
    """The tool with its working tree moved into tmp_path, partitions_p4_16mb.csv left out."""
    spec = importlib.util.spec_from_file_location(
        "user_data", os.path.join(ROOT, "tools", "user_data.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    monkeypatch.setattr(mod, "ROOT", str(tmp_path))
    monkeypatch.setattr(mod, "DATA", str(tmp_path / "data"))
    monkeypatch.setattr(mod, "SAVE", str(tmp_path / "build" / "user_data"))
    monkeypatch.setattr(mod, "IMAGE", str(tmp_path / "build" / "device_data.bin"))
    monkeypatch.setattr(mod, "TABLE", str(tmp_path / "build" / "device_table.bin"))
    (tmp_path / "data").mkdir()
    return mod


UNREADABLE = object()       # a panel whose table esptool cannot read


class Panel:
    """esptool and the LittleFS reader in one: a panel whose flash at 0x10000 holds `table`
    (the checkout's layout by default) and whose partition holds `files`. With `fail`, the
    read of the files fails after writing them. `reads` lists every (offset, size) esptool
    was asked for."""

    def __init__(self, files, table=None, fail=False):
        self.files, self.fail, self.reads = files, fail, []
        self.table = table_image(checkout_rows()) if table is None else table

    def __call__(self, argv, **_):
        if "read_flash" in argv:
            offset, size = int(argv[-3], 16), int(argv[-2], 16)
            self.reads.append((offset, size))
            if offset == 0x10000 and self.table is UNREADABLE:
                raise subprocess.CalledProcessError(2, argv[:3])
            with open(argv[-1], "wb") as f:
                f.write(self.table if offset == 0x10000 else b"")
            return 0
        assert argv[1] == "-c", argv                  # the reader: code, image, out dir, names
        for name, data in self.files.items():
            with open(os.path.join(argv[4], name), "wb") as f:
                f.write(data)
        if self.fail:
            raise subprocess.CalledProcessError(1, argv[:2])
        return 0


def backup(tool, monkeypatch, panel, partition="data"):
    monkeypatch.setattr(subprocess, "check_call", panel)
    tool.cmd_backup(argparse.Namespace(partition=partition, port="COM12", baud=921600))


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
    assert sorted(os.listdir(tmp_path / "build")) == [
        "device_data.bin", "device_table.bin", "user_data"]


def test_the_table_is_read_where_the_board_puts_it(tool):
    with open(os.path.join(ROOT, "boards", "p4_player", "sdkconfig.board"),
              encoding="utf-8") as f:
        m = re.search(r"^CONFIG_PARTITION_TABLE_OFFSET=(\S+)$", f.read(), re.M)
    assert m and int(m.group(1), 0) == tool.TABLE_OFFSET == 0x10000


def test_the_parser_reads_a_table_as_esp_idf_writes_it(tool):
    for rows in (checkout_rows(), OLD_LAYOUT):
        parts = tool.parse_table(table_image(rows))
        assert list(parts.items()) == [(name, (off, size)) for name, off, size in rows]


def test_a_table_that_is_not_one_is_refused(tool):
    good = table_image(OLD_LAYOUT)
    assert tool.parse_table(b"\xff" * 0xC00) == {}             # blank flash: no partitions
    broken = bytearray(good)
    broken[4] ^= 0x01                                           # one bit of nvs's offset
    with pytest.raises(ValueError, match="MD5"):
        tool.parse_table(bytes(broken))
    with pytest.raises(ValueError, match="no partition entry"):
        tool.parse_table(b"\x00" * 0xC00)
    with pytest.raises(ValueError, match="end marker"):
        tool.parse_table(good[:32 * len(OLD_LAYOUT)])


def test_a_panel_on_the_old_layout_is_read_where_its_data_is(tool, monkeypatch):
    panel = Panel({"stations.json": b"[1]"}, table=table_image(OLD_LAYOUT))
    backup(tool, monkeypatch, panel)
    assert panel.reads == [(0x10000, 0xC00), (0x830000, 0x7D0000)]
    assert saved(tool) == {"stations.json": b"[1]"}


def test_an_upgraded_panel_is_read_where_its_table_says(tool, monkeypatch):
    panel = Panel({"eq.json": b"{1}"})
    backup(tool, monkeypatch, panel, partition="userdata")
    assert panel.reads == [(0x10000, 0xC00), (0xF80000, 0x80000)]


def test_userdata_on_a_panel_that_has_none_stops_before_reading(tool, monkeypatch):
    panel = Panel({"eq.json": b"{1}"}, table=table_image(OLD_LAYOUT))
    with pytest.raises(SystemExit, match="no 'userdata' partition.*old layout"):
        backup(tool, monkeypatch, panel, partition="userdata")
    assert panel.reads == [(0x10000, 0xC00)]
    assert not os.path.exists(tool.SAVE)


@pytest.mark.parametrize("table", [UNREADABLE, b"\xff" * 0xC00, b"\x00" * 0xC00],
                         ids=["unreadable", "blank", "not-a-table"])
def test_without_the_panels_own_table_nothing_is_read(tool, monkeypatch, table):
    panel = Panel({"eq.json": b"{1}"}, table=table)
    with pytest.raises(SystemExit):
        backup(tool, monkeypatch, panel)
    assert panel.reads == [(0x10000, 0xC00)]
    assert not os.path.exists(tool.SAVE)
