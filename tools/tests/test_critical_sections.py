"""No SharedState access between portENTER_CRITICAL and portEXIT_CRITICAL.

read_int/read_bool/read_float/read_string and state_get/state_set take SharedState's FreeRTOS
mutex. Inside a critical section interrupts are off on that core, and a contended take there
corrupts the kernel's event list or spins until the interrupt watchdog resets the board.
44d4cbc moved three such reads out of UiLvglModule::mirror_state; a fourth was later found
put back. This test keeps the whole tree honest instead of one function.
"""

from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
SOURCES = [p for pattern in ("modules/*/src/*.cpp", "components/*/src/*.cpp", "main/*.cpp")
           for p in ROOT.glob(pattern)]
BLOCKING = re.compile(r"\b(read_int|read_bool|read_float|read_string|state_get|state_set|"
                      r"post_command|post_command_to)\s*\(")
ENTER = re.compile(r"\bportENTER_CRITICAL\s*\(")
EXIT = re.compile(r"\bportEXIT_CRITICAL\s*\(")


def critical_violations(text):
    """(line, code) pairs for blocking SharedState calls inside a critical section."""
    found, inside = [], False
    for number, line in enumerate(text.splitlines(), 1):
        code = line.split("//", 1)[0]
        if ENTER.search(code):
            inside = True
        if inside and BLOCKING.search(code):
            found.append((number, line.strip()))
        if EXIT.search(code):
            inside = False
    return found


def test_detector_catches_the_29_09_regression():
    sample = """
    portENTER_CRITICAL(&mux_);
    snap_.network_seekable = network_push && read_bool("radio.seekable", false);
    portEXIT_CRITICAL(&mux_);
    const bool ok = read_bool("x", false);   // outside: fine
    """
    assert [n for n, _ in critical_violations(sample)] == [3]


def test_no_shared_state_access_inside_critical_sections():
    assert SOURCES, "no sources found - wrong root?"
    problems = []
    for path in SOURCES:
        for number, code in critical_violations(path.read_text(encoding="utf-8", errors="replace")):
            problems.append(f"{path.relative_to(ROOT)}:{number}: {code}")
    assert not problems, "SharedState access inside a critical section:\n" + "\n".join(problems)
