#!/usr/bin/env python3
"""Move a panel's own files onto the partition a flash cannot reach, and back them up.

Until 22.09.2026 this tool was the workaround for a design fault it described itself:

    "This does not fix that; the fix is to stop storing user files next to build output."

That fix is now in (plan item 11.1). The flash has two LittleFS partitions:

    data      board.json, bindings.json, ui.json, the web bundle   REWRITTEN by every flash
    userdata  eq.json, stations.json, last_track, devices.json,    NO image, never written
              eq_custom.json

On its first boot after the change the firmware copies the user files from `data` into
`userdata` (components/user_store) and from then on reads and writes them there.

The tool is kept for two jobs it still has, not as a step in the normal build:

1. THE ONE-TIME MOVE. Installing the new layout means writing a new partition table, which
   means a full `idf.py flash`, which rewrites `data` — so a panel whose files are still in
   `data` loses them at exactly the moment it is being fixed. The only way through is to
   carry them across inside the image and let the first boot migrate them out:

       python tools/user_data.py backup            # read the panel BEFORE flashing it
       python tools/user_data.py seed              # put them in data/ so data.bin carries them
       idf.py build && idf.py -p COM12 flash       # new table; first boot migrates them
       python tools/user_data.py restore           # data/ back the way git has it

   Do the backup first. After the flash it is too late.

2. A BACKUP. `backup --partition userdata` reads the store off an upgraded panel, which is
   worth doing before anything experimental. `idf.py flash` no longer needs it.

`backup` needs esptool and reboots the board into the loader. Each backup replaces
build/user_data whole, and only once it has read the panel to the end, so `seed` carries
exactly what the last successful backup found: never a file left over from an earlier one,
never half of a backup that failed.

`seed` and `restore` touch only the working tree. Between the two, `idf.py build`
regenerates data.bin because the seeded files change - but data.bin is NOT rebuilt when
only eq.json changes (its DEPENDS list does not name it), so `seed` deletes build/data.bin
to force it.

Reading a LittleFS image needs littlefs-python. The build already provisions it in
build/littlefs_py_venv for writing images, and this reuses that interpreter rather than
installing anything.
"""

import argparse
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(ROOT, "data")
SAVE = os.path.join(ROOT, "build", "user_data")
IMAGE = os.path.join(ROOT, "build", "device_data.bin")

# What the firmware writes at runtime and git does not carry. A file listed here that is also
# tracked (eq.json is) gets restored from git by `restore`; the rest are simply removed.
#
# This list MUST match UserStore::FILES in components/user_store/src/user_store.cpp, and
# tools/tests/test_user_files.py fails if it does not. Two corrections came out of writing
# that test: "last_dir" was never written by anything (the player writes last_track, through
# a constant still named LAST_DIR_PATH), and devices.json - the remote devices someone
# subscribes over /api/devices - was missing, so a backup silently left them behind.
USER_FILES = ["eq.json", "stations.json", "last_track", "devices.json", "eq_custom.json"]

# The geometry the device mounts with: CONFIG_LITTLEFS_* in sdkconfig, block size 4096.
# No name_max: littlefs refuses to mount (LFS_ERR_INVAL) when it is below what the superblock
# records, and the firmware formats `userdata` with littlefs's own 255, not the 64 of
# CONFIG_LITTLEFS_OBJ_NAME_LEN — so `backup --partition userdata` failed with it set (seen
# 11.10.2026). Left out, it is taken from the superblock: 64 for data.bin, 255 for userdata.
GEOMETRY = dict(block_size=4096, read_size=128, prog_size=256, cache_size=512,
                lookahead_size=128, block_cycles=512)


def _partition(name="data"):
    """Offset and size of a partition, read from the table rather than assumed."""
    csv = os.path.join(ROOT, "partitions_p4_16mb.csv")
    with open(csv, encoding="utf-8") as f:
        for line in f:
            if line.strip().startswith("#") or not line.strip():
                continue
            cols = [c.strip() for c in line.split(",")]
            if cols and cols[0] == name and len(cols) >= 5:
                return int(cols[3], 0), int(cols[4], 0)
    raise SystemExit("could not find the %r partition in %s" % (name, csv))


def _lfs_python():
    """The interpreter that has littlefs-python: the build's venv, else this one."""
    venv = os.path.join(ROOT, "build", "littlefs_py_venv", "Scripts", "python.exe")
    if not os.path.exists(venv):
        venv = os.path.join(ROOT, "build", "littlefs_py_venv", "bin", "python")
    return venv if os.path.exists(venv) else sys.executable


def cmd_backup(args):
    # A panel that has not been upgraded yet has no 'userdata' partition, and reading the
    # offset the NEW table gives for it would read the middle of that panel's 'data'. So the
    # default is 'data', which is where the files are on every panel this is used on first.
    off, size = _partition(args.partition)
    os.makedirs(os.path.dirname(IMAGE), exist_ok=True)
    print("reading 0x%X..0x%X from %s" % (off, off + size, args.port))
    subprocess.check_call([sys.executable, "-m", "esptool", "-p", args.port,
                           "-b", str(args.baud), "read_flash",
                           hex(off), hex(size), IMAGE])

    # Read into a directory of its own and swap it in only when the read got to the end.
    # Written over the old set, a file the panel no longer has stayed from an earlier backup,
    # and a read that failed half way left half of each - and `seed` puts whatever is in
    # build/user_data into the image as the panel's own.
    new, old = SAVE + ".new", SAVE + ".old"
    for left in (new, old):                     # what a backup cut short left behind
        shutil.rmtree(left, ignore_errors=True)
    os.makedirs(new)
    code = r'''
import os, sys
from littlefs import LittleFS
img = open(sys.argv[1], "rb").read()
fs = LittleFS(block_count=len(img)//%(block_size)d, mount=False, **%(geom)r)
fs.context.buffer = bytearray(img)
fs.mount()
saved = []
for name in sys.argv[3:]:
    try:
        with fs.open("/" + name, "rb") as f:
            data = f.read()
    except Exception:
        continue
    open(os.path.join(sys.argv[2], name), "wb").write(data)
    saved.append("%%s (%%d bytes)" %% (name, len(data)))
print("\n".join("  saved " + s for s in saved) or "  the panel had none of these files")
''' % {"block_size": GEOMETRY["block_size"], "geom": GEOMETRY}
    try:
        subprocess.check_call([_lfs_python(), "-c", code, IMAGE, new] + USER_FILES)
    except BaseException:
        shutil.rmtree(new, ignore_errors=True)
        raise
    if os.path.isdir(SAVE):
        os.rename(SAVE, old)
    os.rename(new, SAVE)
    shutil.rmtree(old, ignore_errors=True)
    print("backup in %s" % SAVE)


def cmd_seed(args):
    if not os.path.isdir(SAVE):
        raise SystemExit("no backup yet — run 'backup' first")
    n = 0
    for name in USER_FILES:
        src = os.path.join(SAVE, name)
        if os.path.exists(src):
            shutil.copy2(src, os.path.join(DATA, name))
            print("  data/%s <- the panel's own" % name)
            n += 1
    # data.bin's DEPENDS list does not name eq.json, so a changed EQ alone would not rebuild
    # it and the flash would carry the old image.
    stale = os.path.join(ROOT, "build", "data.bin")
    if os.path.exists(stale):
        os.remove(stale)
        print("  removed build/data.bin so the image is rebuilt")
    print("seeded %d file(s); now run idf.py build && idf.py -p COM12 flash" % n)


def cmd_restore(args):
    for name in USER_FILES:
        path = os.path.join(DATA, name)
        if not os.path.exists(path):
            continue
        tracked = subprocess.run(["git", "ls-files", "--error-unmatch", "data/" + name],
                                 cwd=ROOT, capture_output=True).returncode == 0
        if tracked:
            subprocess.check_call(["git", "checkout", "--", "data/" + name], cwd=ROOT)
            print("  data/%s restored from git" % name)
        else:
            os.remove(path)
            print("  data/%s removed (not in git)" % name)
    stale = os.path.join(ROOT, "build", "data.bin")
    if os.path.exists(stale):
        os.remove(stale)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    b = sub.add_parser("backup", help="read a partition off the panel and save the user files in it")
    b.add_argument("--partition", default="data", choices=["data", "userdata"],
                   help="'data' for a panel still on the old layout (the default, and what "
                        "the one-time move needs); 'userdata' to back up an upgraded one")
    b.add_argument("--port", default="COM12")
    b.add_argument("--baud", type=int, default=921600)
    b.set_defaults(func=cmd_backup)

    s = sub.add_parser("seed", help="copy the saved files into data/ before a build")
    s.set_defaults(func=cmd_seed)

    r = sub.add_parser("restore", help="put data/ back the way git has it")
    r.set_defaults(func=cmd_restore)

    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
