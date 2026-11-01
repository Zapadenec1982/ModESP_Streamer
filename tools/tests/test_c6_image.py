"""
test_c6_image.py — the C6 co-processor image checker (tools/c6_image.py).

Why this is worth testing on the host rather than on the bench: the arithmetic is the part
that goes wrong, and it goes wrong silently. tools/c6_updater's own parse_image_header()
computed the image length as

    align_up(len, 16) + 1 + align_up_pad + 32

which is 16 bytes MORE than the real length whenever the segment data does not happen to
end on a 16-byte boundary — and that never showed up, because the slave's esp_ota_write()
accepts the extra bytes and esp_ota_end() finds the real length in the header anyway. The
moment anything actually verifies the image, those 16 bytes make every SHA-256 fail.

So the images here are built the way esptool builds them, and then broken one way at a time.
"""

import hashlib
import importlib.util
import json
import os
import struct

import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

_spec = importlib.util.spec_from_file_location(
    "c6_image", os.path.join(ROOT, "tools", "c6_image.py"))
c6 = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(c6)


APP_DESC_MAGIC = 0xABCD5432


def app_desc(version="v3.0.7", project="coprocessor_ota"):
    """The first 256 bytes of an esp_app_desc_t, enough for version and project_name."""
    d = bytearray(256)
    struct.pack_into("<I", d, 0, APP_DESC_MAGIC)
    d[0x10:0x10 + len(version)] = version.encode()
    d[0x30:0x30 + len(project)] = project.encode()
    return bytes(d)


def build_image(segments=None, hash_appended=True, segment_count=None):
    """An ESP application image, laid out exactly as esptool writes one.

    segments: list of (load_addr, data). Defaults to one segment that starts with a
    plausible esp_app_desc_t, because that is what the updater reads the version out of.
    """
    if segments is None:
        # 1337 bytes on purpose: not a multiple of 16, so the padding before the checksum
        # byte is exercised. A round length would let the off-by-16 bug pass.
        body = app_desc() + bytes((i * 7) & 0xFF for i in range(1337 - 256))
        segments = [(0x42000020, body)]

    n = segment_count if segment_count is not None else len(segments)
    header = bytearray(24)
    header[0] = 0xE9
    header[1] = n
    header[23] = 1 if hash_appended else 0

    out = bytearray(header)
    checksum = 0xEF
    for load_addr, data in segments:
        out += struct.pack("<II", load_addr, len(data))
        out += data
        for b in data:
            checksum ^= b

    out += b"\x00" * (15 - (len(out) % 16))
    out.append(checksum)
    assert len(out) % 16 == 0

    if hash_appended:
        out += hashlib.sha256(bytes(out)).digest()
    return bytes(out)


# ── the happy path ───────────────────────────────────────────────────────────────────────

def test_a_well_formed_image_verifies_and_reports_its_version():
    info = c6.verify(build_image())
    assert info["checksum_ok"]
    assert info["sha_appended"] and info["sha_ok"]
    assert info["version"] == "v3.0.7"
    assert info["project"] == "coprocessor_ota"
    assert len(info["segments"]) == 1


def test_the_reported_length_is_the_image_length_esptool_wrote():
    """The number the updater streams to the C6. 16 bytes too many is the historical bug."""
    data = build_image()
    info = c6.verify(data)
    assert info["length"] == len(data)
    # And the same image with a tail on it still reports the image length, not the file's:
    # a partition read does not know where the image ends, only the header does.
    info2 = c6.verify(data + b"\xFF" * 4096)
    assert info2["length"] == len(data)
    assert info2["sha256"] == info["sha256"]


def test_an_image_without_an_appended_hash_is_still_usable():
    info = c6.verify(build_image(hash_appended=False))
    assert info["checksum_ok"]
    assert info["sha_appended"] is False
    assert info["sha_ok"] is None


def test_several_segments_are_walked_in_order():
    segs = [(0x42000020, app_desc() + b"a" * 100), (0x4FF00000, b"b" * 33), (0x4FF10000, b"c" * 7)]
    info = c6.verify(build_image(segs))
    assert [s["len"] for s in info["segments"]] == [356, 33, 7]
    assert info["segments"][1]["load_addr"] == 0x4FF00000


@pytest.mark.parametrize("payload_len", [1, 15, 16, 17, 255, 256, 1000, 1024])
def test_the_padding_is_right_for_every_alignment_of_the_segment_data(payload_len):
    """The one thing that must not be approximately right."""
    data = build_image([(0x42000020, app_desc()[:min(payload_len, 256)] +
                         b"z" * max(0, payload_len - 256))])
    info = c6.verify(data)
    assert info["length"] == len(data)
    assert info["length"] % 16 == (0 if not info["sha_appended"] else 32 % 16)


# ── the ways a bad image arrives ─────────────────────────────────────────────────────────

def test_a_file_that_is_not_an_esp_image_is_refused_by_its_magic():
    with pytest.raises(c6.BadImage) as e:
        c6.verify(b"\x7fELF" + b"\x00" * 4096)
    assert "magic" in str(e.value)


def test_an_empty_or_tiny_file_is_refused_rather_than_indexed_past_its_end():
    for data in (b"", b"\xE9", b"\xE9\x01" + b"\x00" * 10):
        with pytest.raises(c6.BadImage):
            c6.verify(data)


def test_a_truncated_image_is_refused_and_says_where_it_ran_out():
    """A download that stopped, or a partition that was only partly written."""
    data = build_image()
    with pytest.raises(c6.BadImage) as e:
        c6.verify(data[:len(data) // 2])
    assert "ends inside" in str(e.value)


def test_an_image_missing_only_its_appended_hash_is_refused():
    data = build_image()
    with pytest.raises(c6.BadImage) as e:
        c6.verify(data[:-32])
    assert "SHA-256" in str(e.value) or "checksum" in str(e.value)


def test_one_flipped_bit_in_the_payload_fails_the_checksum_and_the_hash():
    data = bytearray(build_image())
    data[500] ^= 0x01
    with pytest.raises(c6.BadImage) as e:
        c6.verify(bytes(data))
    assert "checksum" in str(e.value)


def test_a_corrupted_hash_alone_is_caught_even_when_the_checksum_still_matches():
    data = bytearray(build_image())
    data[-1] ^= 0xFF                      # last byte of the appended SHA-256
    info = c6.describe(bytes(data))
    assert info["checksum_ok"] is True    # the payload is untouched
    assert info["sha_ok"] is False
    with pytest.raises(c6.BadImage) as e:
        c6.verify(bytes(data))
    assert "SHA-256" in str(e.value)


def test_a_segment_length_that_is_really_noise_is_refused_not_acted_on():
    """An erased or mis-flashed partition reads as a huge length; it must not be trusted."""
    header = bytearray(24)
    header[0] = 0xE9
    header[1] = 1
    bad = bytes(header) + struct.pack("<II", 0x42000020, 0xFFFFFFFF) + b"\x00" * 64
    with pytest.raises(c6.BadImage) as e:
        c6.verify(bad)
    assert "not a length" in str(e.value)


def test_an_implausible_segment_count_is_refused():
    with pytest.raises(c6.BadImage):
        c6.verify(build_image(segment_count=0))
    with pytest.raises(c6.BadImage):
        c6.verify(build_image(segment_count=200))


# ── the known-good record ────────────────────────────────────────────────────────────────

def test_the_recorded_fingerprint_matches_a_freshly_computed_one(tmp_path, monkeypatch):
    image = build_image()
    path = tmp_path / "cp.bin"
    path.write_bytes(image)
    record = tmp_path / "known_good.json"
    monkeypatch.setattr(c6, "RECORD", str(record))

    class Args:
        pass
    a = Args(); a.image = str(path); a.tag = "v3.0.7"; a.hosted = "3.0.7"
    assert c6.cmd_record(a) == 0

    saved = json.loads(record.read_text())
    assert saved["sha256"] == c6.describe(image)["sha256"]
    assert saved["length"] == len(image)
    assert saved["tag"] == "v3.0.7"

    # The same file checks out...
    assert c6.cmd_check(a) == 0
    # ...a different but valid image is reported as different, not as broken...
    other = tmp_path / "other.bin"
    other.write_bytes(build_image([(0x42000020, app_desc(version="v3.1.0") + b"q" * 500)]))
    a.image = str(other)
    assert c6.cmd_check(a) == 2
    # ...and a broken one is refused outright.
    broken = tmp_path / "broken.bin"
    broken.write_bytes(image[:200])
    a.image = str(broken)
    assert c6.cmd_check(a) == 1


def test_recording_a_broken_image_is_refused(tmp_path, monkeypatch):
    """The known-good record is only worth anything if nothing broken can enter it."""
    record = tmp_path / "known_good.json"
    monkeypatch.setattr(c6, "RECORD", str(record))
    path = tmp_path / "half.bin"
    path.write_bytes(build_image()[:300])

    class Args:
        pass
    a = Args(); a.image = str(path); a.tag = None; a.hosted = "3.0.7"
    assert c6.cmd_record(a) == 1
    assert not record.exists()
