#!/usr/bin/env python3
"""
run_host_tests.py — run the host test suite: tools/tests/ (pytest) plus the native C++
doctest runner in tests/host/, which tools/tests/test_cpp_host.py builds and executes.

Why a wrapper: the ESP-IDF Python venv auto-loads the ``idf_ci`` / ``pytest-embedded``
plugins, whose ``pytest_configure`` reads a ``--target`` option meant for on-target/HIL
runs and crashes plain unit-test collection ("no option named 'target'"). Setting
PYTEST_DISABLE_PLUGIN_AUTOLOAD restores a clean pytest for the generator/validation suite.

Run it with a python that has pytest + jsonschema — the simplest is the ESP-IDF venv
python (inside an ESP-IDF shell), e.g.::

    python tools/run_host_tests.py                 # pytest suite + C++ doctest runner
    python tools/run_host_tests.py --no-cpp        # Python suite only (explicit opt-out)
    python tools/run_host_tests.py --cpp-timeout 1800   # C++ runner bound, seconds (default 900)
    python tools/run_host_tests.py -k mqtt -v      # extra args pass through to pytest

The C++ runner is part of the per-commit gate (every commit passes the esp32p4 build and
this suite), so it runs by default.
It needs a host C++ compiler and cmake, and a missing toolchain is a test FAILURE, not a
skip: before 2026-09-21 the default run ignored test_cpp_host.py altogether and the gate
executed zero C++ tests. ``--cpp-host`` (the old opt-in flag) is still accepted and does
nothing.

The C++ binary runs under a time bound — P4_CPP_TEST_TIMEOUT_S or --cpp-timeout, default
900 s — and exceeding it is a failure, never a skip. The bound is generous because the
two-thread ring tests (tests/host/test_pcm_ring.cpp) crawl on a busy host: a green run
took ~41 min under five concurrent idf.py builds on 2026-09-21, while on an idle machine
the whole suite takes seconds.

The link-protocol mirror check (tools/check_protocol_mirror.py) runs from
tools/tests/test_protocol_mirror.py when P4_SATELLITE_ROOT names a modesp-bt-satellite
checkout; otherwise that one test skips and this wrapper says so up front.
"""
import os
import sys
import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent
TESTS = HERE / "tests"


def main() -> int:
    args, no_cpp, cpp_timeout = [], False, None
    it = iter(sys.argv[1:])
    for a in it:
        if a == "--no-cpp":
            no_cpp = True
        elif a == "--cpp-host":            # the pre-2026-09-21 opt-in flag; C++ is the default now
            pass
        elif a == "--cpp-timeout":
            cpp_timeout = next(it, "")
        elif a.startswith("--cpp-timeout="):
            cpp_timeout = a.split("=", 1)[1]
        else:
            args.append(a)
    if cpp_timeout is not None and not (cpp_timeout.isdigit() and int(cpp_timeout) > 0):
        print(f"--cpp-timeout needs a positive number of seconds, got {cpp_timeout!r}", file=sys.stderr)
        return 2

    env = dict(os.environ, PYTEST_DISABLE_PLUGIN_AUTOLOAD="1")
    if cpp_timeout:
        env["P4_CPP_TEST_TIMEOUT_S"] = cpp_timeout

    cmd = [sys.executable, "-m", "pytest", str(TESTS), "-q"]
    if no_cpp:
        cmd.append(f"--ignore={TESTS / 'test_cpp_host.py'}")
    cmd += args

    if no_cpp:
        print("C++ host tests: SKIPPED on request (--no-cpp) - this is not the full gate")
    else:
        bound = env.get("P4_CPP_TEST_TIMEOUT_S", "900 default")
        print(f"C++ host tests: included (tests/host via tools/tests/test_cpp_host.py), "
              f"runner bound {bound} s - a timeout is a failure")
    satellite = os.environ.get("P4_SATELLITE_ROOT")
    if satellite:
        print(f"protocol mirror: comparing against P4_SATELLITE_ROOT={satellite}")
    else:
        print("protocol mirror: P4_SATELLITE_ROOT not set - tools/check_protocol_mirror.py "
              "is skipped; point it at a modesp-bt-satellite checkout to include the check")

    print("running:", " ".join(cmd))
    return subprocess.call(cmd, env=env)


if __name__ == "__main__":
    raise SystemExit(main())
