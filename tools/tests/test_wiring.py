"""
test_wiring.py — the wiring drawing in docs/img is the one tools/gen_wiring.py draws.

The generator refuses to draw when board.json disagrees with the pins it draws, and checks every
wire against its own netlist. This test runs it with --check: it fails on either of those, and
when docs/img/wiring.svg or wiring.uk.svg is not what it would write. With P4_SATELLITE_ROOT set,
the satellite's pins are checked against its main/board.h as well.
"""

import os
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def test_wiring_drawing_is_current():
    r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "gen_wiring.py"), "--check"],
                       capture_output=True, text=True, encoding="utf-8")
    assert r.returncode == 0, r.stdout + r.stderr
