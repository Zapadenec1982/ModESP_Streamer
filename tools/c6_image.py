#!/usr/bin/env python3
"""Check an ESP32-C6 co-processor image before it is allowed anywhere near the panel.

The Wi-Fi on this panel lives on a C6 that only the P4 can reach. Its firmware is flashed
into the P4's `slave_fw` partition and pushed over SDIO by tools/c6_updater. If a bad image
gets that far and is activated, the C6 comes up with no working firmware, ESP-Hosted never
connects, and the panel has no network — which is also the only way to fix it remotely. The
recovery is a soldering-iron job on JP1 (docs/C6_UPDATE.md, "Recovery: flashing the C6
directly over UART (JP1)").

So the image is checked twice, and this is the first of the two:

  * here, on the PC, before `idf.py flash` writes it into `slave_fw` — a truncated download,
    a half-written copy, the wrong file;
  * on the device, in ota_partition.c, before a single byte is sent to the C6 and long
    before anything is activated.

Both check the same three things, which is what an ESP application image can be checked
against with no external information:

  1. the header magic (0xE9) and a segment table that adds up inside the file;
  2. the one-byte checksum esptool appends: 0xEF xor every segment DATA byte;
  3. the SHA-256 esptool appends when `hash_appended` is set, over everything up to and
     including that checksum byte.

The layout, from esptool's own writer (`ESP32FirmwareImage.save` / `append_checksum`):

    header (24 B) | [segment header (8 B) + data] * n | pad | checksum (1 B) | [sha256]
                                                        ^-- pad so the checksum byte lands
                                                            on the last byte of a 16-byte
                                                            block; the image length up to
                                                            and including it is a multiple
                                                            of 16. Nothing is padded before
                                                            the hash.

`record` writes tools/c6_updater/known_good.json — the fingerprint of the image the panel is
known to work with (docs/C6_UPDATE.md, "Image verification — twice, before it reaches the
C6"). The image
itself is ~1.1 MB and does not belong in git; the fingerprint does, so "is this the file we
verified?" has an answer that does not depend on anyone's memory.

    python tools/c6_image.py verify  <image.bin>
    python tools/c6_image.py record  <image.bin> --tag v3.0.7   # write the fingerprint
    python tools/c6_image.py check   <image.bin>                # against the fingerprint
"""

import argparse
import hashlib
import json
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RECORD = os.path.join(ROOT, "tools", "c6_updater", "known_good.json")

MAGIC = 0xE9
HEADER_LEN = 24
SEGMENT_HEADER_LEN = 8
CHECKSUM_SEED = 0xEF
SHA_LEN = 32

# A segment longer than this is not a segment, it is a misread length field. The C6's whole
# flash is 4 MB, so nothing legitimate comes close.
MAX_SEGMENT_LEN = 4 * 1024 * 1024
MAX_SEGMENTS = 16


class BadImage(Exception):
    """The bytes are not a usable ESP application image, with the reason attached."""


def describe(data):
    """Walk `data` as an ESP application image and return what it is.

    Returns a dict with: length (bytes the image actually occupies, which may be less than
    len(data) when the file has a tail), segments, checksum_ok, sha_appended, sha_ok,
    sha256 (of the image, not of the file), version, project.

    Raises BadImage when the walk cannot be completed — a truncated file reaches this, and
    reaching it is the point.
    """
    if len(data) < HEADER_LEN:
        raise BadImage("shorter than an image header (%d bytes)" % len(data))
    magic, segment_count = data[0], data[1]
    if magic != MAGIC:
        raise BadImage("magic is 0x%02X, expected 0x%02X — not an ESP image" % (magic, MAGIC))
    if not 1 <= segment_count <= MAX_SEGMENTS:
        raise BadImage("segment count %d is not plausible" % segment_count)
    hash_appended = data[23] == 1

    pos = HEADER_LEN
    checksum = CHECKSUM_SEED
    segments = []
    for i in range(segment_count):
        if pos + SEGMENT_HEADER_LEN > len(data):
            raise BadImage("file ends inside the header of segment %d" % i)
        load_addr, data_len = struct.unpack("<II", data[pos:pos + SEGMENT_HEADER_LEN])
        if data_len > MAX_SEGMENT_LEN:
            raise BadImage("segment %d claims %d bytes — the field is not a length" % (i, data_len))
        pos += SEGMENT_HEADER_LEN
        if pos + data_len > len(data):
            raise BadImage("file ends inside segment %d (need %d more bytes)"
                           % (i, pos + data_len - len(data)))
        for b in data[pos:pos + data_len]:
            checksum ^= b
        segments.append({"load_addr": load_addr, "len": data_len})
        pos += data_len

    # Pad so the checksum byte is the last byte of a 16-byte block.
    pos += 15 - (pos % 16)
    if pos >= len(data):
        raise BadImage("file ends before the checksum byte")
    checksum_ok = data[pos] == checksum
    pos += 1                      # now a multiple of 16

    sha_ok = None
    if hash_appended:
        if pos + SHA_LEN > len(data):
            raise BadImage("header says a SHA-256 is appended and the file ends before it")
        want = data[pos:pos + SHA_LEN]
        got = hashlib.sha256(data[:pos]).digest()
        sha_ok = (want == got)
        pos += SHA_LEN

    return {
        "length": pos,
        "segments": segments,
        "checksum_ok": checksum_ok,
        "sha_appended": hash_appended,
        "sha_ok": sha_ok,
        "sha256": hashlib.sha256(data[:pos]).hexdigest(),
        "version": _app_desc_field(data, APP_DESC_VERSION),
        "project": _app_desc_field(data, APP_DESC_PROJECT),
    }


# esp_app_desc_t lives at the start of the FIRST segment's data, so its fields are at
# HEADER_LEN + SEGMENT_HEADER_LEN + <offset in the struct>. From esp_app_desc.h:
# magic_word(4) secure_version(4) reserv1(8) version[32] project_name[32] ...
APP_DESC_AT = HEADER_LEN + SEGMENT_HEADER_LEN
APP_DESC_MAGIC = 0xABCD5432
APP_DESC_VERSION = 0x10
APP_DESC_PROJECT = 0x30
APP_DESC_FIELD_LEN = 32


def _app_desc_field(data, offset_in_desc, size=APP_DESC_FIELD_LEN):
    """A string out of esp_app_desc_t, or "" when there is no descriptor to read.

    The magic word is checked first: without it these offsets are just bytes, and printing
    them as a version would be worse than printing nothing.
    """
    if len(data) < APP_DESC_AT + 4:
        return ""
    if struct.unpack("<I", data[APP_DESC_AT:APP_DESC_AT + 4])[0] != APP_DESC_MAGIC:
        return ""
    start = APP_DESC_AT + offset_in_desc
    if start + size > len(data):
        return ""
    raw = data[start:start + size]
    end = raw.find(b"\x00")
    if end >= 0:
        raw = raw[:end]
    try:
        return raw.decode("utf-8")
    except UnicodeDecodeError:
        return ""


def verify(data):
    """describe() plus the verdict. Raises BadImage if the image must not be used."""
    info = describe(data)
    if not info["checksum_ok"]:
        raise BadImage("the appended checksum does not match the segment data")
    if info["sha_appended"] and not info["sha_ok"]:
        raise BadImage("the appended SHA-256 does not match the image")
    return info


def _read(path):
    with open(path, "rb") as f:
        return f.read()


def _print(path, info, data_len):
    print("  file        %s" % path)
    print("  image       %d bytes (%d in the file)" % (info["length"], data_len))
    print("  segments    %d" % len(info["segments"]))
    print("  checksum    %s" % ("ok" if info["checksum_ok"] else "MISMATCH"))
    print("  sha-256     %s" % ("ok" if info["sha_ok"] else
                                ("not appended" if not info["sha_appended"] else "MISMATCH")))
    print("  sha256      %s" % info["sha256"])
    print("  version     %s" % (info["version"] or "(none)"))
    print("  project     %s" % (info["project"] or "(none)"))


def cmd_verify(args):
    data = _read(args.image)
    try:
        info = verify(data)
    except BadImage as e:
        print("REFUSED: %s" % e)
        print("  Do not flash this into slave_fw. A bad image activated on the C6 leaves the")
        print("  panel with no network and no way to fix it over the air (docs/C6_UPDATE.md).")
        return 1
    _print(args.image, info, len(data))
    print("OK")
    return 0


def cmd_record(args):
    data = _read(args.image)
    try:
        info = verify(data)
    except BadImage as e:
        print("REFUSED: %s — a broken image must not become the known-good one" % e)
        return 1
    record = {
        "_comment": "Fingerprint of the C6 co-processor image this panel is known to work "
                    "with. The image itself is ~1.1 MB and lives next to the repo, not in "
                    "it; check a copy with: python tools/c6_image.py check <file>",
        "sha256": info["sha256"],
        "length": info["length"],
        "version": info["version"],
        "project": info["project"],
        "tag": args.tag or info["version"],
        "esp_hosted": args.hosted,
    }
    with open(RECORD, "w", encoding="utf-8") as f:
        json.dump(record, f, indent=2)
        f.write("\n")
    print("recorded in %s" % RECORD)
    _print(args.image, info, len(data))
    return 0


def cmd_check(args):
    if not os.path.exists(RECORD):
        print("no fingerprint yet — run: python tools/c6_image.py record <image.bin>")
        return 1
    with open(RECORD, encoding="utf-8") as f:
        record = json.load(f)
    data = _read(args.image)
    try:
        info = verify(data)
    except BadImage as e:
        print("REFUSED: %s" % e)
        return 1
    if info["sha256"] != record["sha256"]:
        print("DIFFERENT image: this file is internally consistent but is not the one")
        print("  recorded as known-good.")
        print("  recorded  %s  (%s, %d bytes)"
              % (record["sha256"], record.get("tag", "?"), record.get("length", 0)))
        print("  this file %s  (%s, %d bytes)" % (info["sha256"], info["version"], info["length"]))
        return 2
    _print(args.image, info, len(data))
    print("OK — matches the known-good record (%s)" % record.get("tag", "?"))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    v = sub.add_parser("verify", help="check an image is whole and internally consistent")
    v.add_argument("image")
    v.set_defaults(func=cmd_verify)

    r = sub.add_parser("record", help="write this image's fingerprint as the known-good one")
    r.add_argument("image")
    r.add_argument("--tag", help="the esp-hosted-mcu tag it was built from, e.g. v3.0.7")
    r.add_argument("--hosted", default="3.0.7",
                   help="the esp_hosted version the host is pinned to (idf_component.yml)")
    r.set_defaults(func=cmd_record)

    c = sub.add_parser("check", help="verify, and compare with the known-good fingerprint")
    c.add_argument("image")
    c.set_defaults(func=cmd_check)

    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
