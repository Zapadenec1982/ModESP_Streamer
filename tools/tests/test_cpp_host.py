"""
test_cpp_host.py — C++ host unit tests via cmake + g++/clang.

Builds the C++ test runner (doctest-based, tests/host/) and runs it under pytest.
Toolchain discovery, in order: MinGW g++ (scoop / MSYS2 / standalone), MSVC through its
vcvars64.bat, the esp-clang shipped with ESP-IDF, a standalone clang++, and finally whatever
``g++`` is on PATH (Linux CI). A missing compiler or cmake is a FAILURE, not a skip, and so is a runner
binary that registers zero test cases: this file is part of the per-commit gate (every
commit passes the esp32p4 build and the host tests) and the gate has to prove the C++
tests actually ran. The explicit opt-out is ``python tools/run_host_tests.py --no-cpp``.

Usage:
    python tools/run_host_tests.py                       # the normal way
    python -m pytest tools/tests/test_cpp_host.py -v     # this file alone
"""

import subprocess
import sys
import os
import re
import glob
import time

import pytest

# ── Project paths ──
ROOT      = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
BUILD_DIR = os.path.join(ROOT, 'tests', 'host', 'build')
SRC_DIR   = os.path.join(ROOT, 'tests', 'host')

# ── IDF_TOOLS_PATH (може бути задано середовищем або за замовчуванням) ──
IDF_TOOLS_PATH = os.environ.get('IDF_TOOLS_PATH', r'C:\Espressif\tools')

# ── scoop home (зазвичай %USERPROFILE%\scoop) ──
_SCOOP_ROOT = os.path.join(os.path.expanduser('~'), 'scoop', 'apps', 'mingw', 'current', 'bin')

# ── Compiler search — MinGW g++ або Clang++ (в порядку пріоритету) ──
COMPILER_CANDIDATES = [
    # scoop install mingw  (рекомендовано: найпростіше встановлення на Windows)
    {'gxx': os.path.join(_SCOOP_ROOT, 'g++.exe'),
     'gcc': os.path.join(_SCOOP_ROOT, 'gcc.exe'),
     'make': os.path.join(_SCOOP_ROOT, 'mingw32-make.exe'),
     'generator': 'MinGW Makefiles'},
    # MSYS2 UCRT64 (winget install MSYS2.MSYS2 + pacman -S mingw-w64-ucrt-x86_64-gcc)
    {'gxx': r'C:\msys64\ucrt64\bin\g++.exe',   'gcc': r'C:\msys64\ucrt64\bin\gcc.exe',   'make': r'C:\msys64\ucrt64\bin\mingw32-make.exe',   'generator': 'MinGW Makefiles'},
    # MSYS2 MinGW64
    {'gxx': r'C:\msys64\mingw64\bin\g++.exe',  'gcc': r'C:\msys64\mingw64\bin\gcc.exe',  'make': r'C:\msys64\mingw64\bin\mingw32-make.exe',  'generator': 'MinGW Makefiles'},
    # Standalone MinGW
    {'gxx': r'C:\mingw64\bin\g++.exe',          'gcc': r'C:\mingw64\bin\gcc.exe',          'make': r'C:\mingw64\bin\mingw32-make.exe',          'generator': 'MinGW Makefiles'},
    {'gxx': r'C:\mingw32\bin\g++.exe',          'gcc': r'C:\mingw32\bin\gcc.exe',          'make': r'C:\mingw32\bin\mingw32-make.exe',          'generator': 'MinGW Makefiles'},
]

# Ninja (від ESP-IDF) для використання з Clang
NINJA_PATHS = [
    os.path.join(IDF_TOOLS_PATH, r'ninja\1.12.1\ninja.exe'),
    r'C:\Espressif\tools\ninja\1.12.1\ninja.exe',
    r'C:\Espressif\tools\ninja\1.11.1\ninja.exe',
]

# Clang++ варіанти (в порядку пріоритету)
CLANG_PATHS = [
    r'C:\Program Files\LLVM\bin\clang++.exe',   # winget install LLVM.LLVM
    r'C:\msys64\ucrt64\bin\clang++.exe',        # pacman -S mingw-w64-ucrt-x86_64-clang
    r'C:\msys64\mingw64\bin\clang++.exe',
]


def _find_esp_idf_clang():
    """Знайти clang++ з ESP-IDF (esp-clang директорія з версійним підкаталогом)."""
    esp_clang_base = os.path.join(IDF_TOOLS_PATH, 'esp-clang')
    if not os.path.isdir(esp_clang_base):
        return None
    pattern = os.path.join(esp_clang_base, '*', 'esp-clang', 'bin', 'clang++.exe')
    matches = sorted(glob.glob(pattern), reverse=True)
    return matches[0] if matches else None


def _find_cmake():
    """Знайти cmake: спочатку в PATH, потім в IDF_TOOLS_PATH."""
    import shutil
    cmake_in_path = shutil.which('cmake')
    if cmake_in_path:
        return cmake_in_path
    cmake_base = os.path.join(IDF_TOOLS_PATH, 'cmake')
    if os.path.isdir(cmake_base):
        pattern = os.path.join(cmake_base, '*', 'bin', 'cmake.exe')
        matches = sorted(glob.glob(pattern), reverse=True)
        if matches:
            return matches[0]
    return None


def _msys2_bin_for(gxx_path):
    """
    Для MSYS2 g++ повертає директорію bin (потрібна в PATH для cc1plus, ld тощо).
    Повертає None якщо не MSYS2.
    """
    if 'msys64' in gxx_path.lower():
        return os.path.dirname(gxx_path)
    return None


def find_compiler():
    """
    Return dict with {'gxx', 'gcc', 'make', 'generator', 'extra_path'} for the first
    available compiler, or None if no compiler is found.

    'extra_path' — додаткові директорії що треба додати в PATH при запуску cmake/g++
    (потрібно для MSYS2 g++ щоб знаходив cc1plus.exe, ld.exe тощо).
    """
    import shutil

    ninja = next((n for n in NINJA_PATHS if os.path.exists(n)), shutil.which('ninja'))

    # 1. MinGW g++ (найнадійніший на Windows)
    for cand in COMPILER_CANDIDATES:
        if os.path.exists(cand['gxx']):
            make = cand['make'] if os.path.exists(cand['make']) else None
            extra = _msys2_bin_for(cand['gxx'])
            if not make and ninja:
                return {**cand, 'make': ninja, 'generator': 'Ninja', 'extra_path': extra}
            if make:
                return {**cand, 'extra_path': extra}

    # 2. MSVC (Visual Studio Build Tools). Ahead of the ESP-IDF clang on purpose: that
    #    clang is built for the chip and has no x86 target, so on a machine without MinGW it
    #    was "found" and then failed every build — which is how the C++ half of the gate
    #    went unrun on the bench PC until 2026-10-02.
    msvc_env = _find_msvc_env()
    if msvc_env and ninja:
        return {'gxx': 'cl', 'gcc': 'cl', 'make': ninja, 'generator': 'Ninja',
                'is_msvc': True, 'extra_path': None, 'env': msvc_env}

    # 3. ESP-IDF clang++ (встановлений разом з IDF)
    esp_clang = _find_esp_idf_clang()
    if esp_clang and ninja:
        return {
            'gxx': esp_clang,
            'gcc': esp_clang.replace('clang++.exe', 'clang.exe'),
            'make': ninja,
            'generator': 'Ninja',
            'is_clang': True,
            'extra_path': None,
        }

    # 3. Standalone clang++
    for cp in CLANG_PATHS:
        if os.path.exists(cp):
            return {
                'gxx': cp, 'gcc': cp.replace('clang++', 'clang'),
                'make': ninja or '', 'generator': 'Ninja' if ninja else 'Unix Makefiles',
                'is_clang': True,
                'extra_path': _msys2_bin_for(cp),
            }

    # 4. PATH fallback — Linux CI (ubuntu-latest: g++ + cmake + ninja-build) and any
    #    machine with the toolchain simply on PATH. cmake picks the compiler itself;
    #    Ninja when it is there, plain Makefiles otherwise.
    if shutil.which('g++'):
        path_ninja = shutil.which('ninja')
        return {'gxx': '', 'gcc': '', 'make': path_ninja or '',
                'generator': 'Ninja' if path_ninja else 'Unix Makefiles', 'extra_path': None}
    return None


def _find_vcvars64():
    """vcvars64.bat of the newest Visual Studio / Build Tools that has the C++ tools, or None."""
    vswhere = os.path.join(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)'),
                           'Microsoft Visual Studio', 'Installer', 'vswhere.exe')
    if os.path.exists(vswhere):
        r = subprocess.run([vswhere, '-latest', '-products', '*', '-requires',
                            'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
                            '-property', 'installationPath'],
                           capture_output=True, text=True)
        for line in r.stdout.splitlines():
            bat = os.path.join(line.strip(), 'VC', 'Auxiliary', 'Build', 'vcvars64.bat')
            if line.strip() and os.path.exists(bat):
                return bat
    pattern = os.path.join(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)'),
                           'Microsoft Visual Studio', '*', '*', 'VC', 'Auxiliary', 'Build', 'vcvars64.bat')
    matches = sorted(glob.glob(pattern), reverse=True)
    return matches[0] if matches else None


def _find_msvc_env():
    """
    The environment vcvars64.bat sets up (PATH, INCLUDE, LIB…), captured once, or None.
    cl.exe cannot run without it, and cmake only finds cl through it.
    """
    if os.name != 'nt':
        return None
    bat = _find_vcvars64()
    if not bat:
        return None
    r = subprocess.run(f'cmd /s /c ""{bat}" >nul && set"', capture_output=True, text=True,
                       shell=True, errors='replace')
    if r.returncode != 0:
        return None
    env = {}
    for line in r.stdout.splitlines():
        k, sep, v = line.partition('=')
        if sep and k:
            env[k.upper()] = v          # Windows names are case-blind; vcvars prints "Path"
    return env if 'INCLUDE' in env else None


def _make_env(compiler):
    """
    Будує env dict для subprocess із доданим extra_path (якщо MSYS2 g++).
    MSYS2 g++ потребує свій bin у PATH щоб знаходив cc1plus.exe, ld.exe тощо.
    MSVC brings its whole environment from vcvars64.bat instead.
    """
    if compiler.get('env'):
        env = dict(compiler['env'])
        # Keep what the caller set (P4_CPP_TEST_TIMEOUT_S, PYTEST_*), but vcvars owns PATH.
        for k, v in os.environ.items():
            env.setdefault(k, v)
        return env
    env = os.environ.copy()
    extra = compiler.get('extra_path')
    if extra and extra not in env.get('PATH', ''):
        env['PATH'] = extra + os.pathsep + env.get('PATH', '')
    return env


def _to_cmake_path(p):
    """Convert Windows backslash path to forward-slash for CMake."""
    return p.replace('\\', '/')


# ── generated/ headers the C++ host build includes ──
GENERATED_DIR = os.path.join(ROOT, 'generated')
GENERATED_HEADERS = ('features_config.h', 'state_meta.h')   # pulled in by modesp_core headers


def _ensure_generated():
    """
    components/modesp_core/include/modesp/{base_module,shared_state}.h include
    generated/features_config.h and generated/state_meta.h, which idf.py writes at CMake
    configure time and git does not track. In a fresh clone (CI) run the generator here,
    with exactly the arguments the root CMakeLists.txt uses.
    """
    if all(os.path.exists(os.path.join(GENERATED_DIR, h)) for h in GENERATED_HEADERS):
        return
    cmd = [sys.executable, os.path.join(ROOT, 'tools', 'generate_ui.py'),
           '--project', os.path.join(ROOT, 'project.json'),
           '--modules-dir', os.path.join(ROOT, 'modules'),
           '--output-data', os.path.join(ROOT, 'data'),
           '--output-gen', GENERATED_DIR]
    print(f"\n[C++] generated/ is missing — running tools/generate_ui.py as idf.py would")
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT)
    if r.returncode != 0:
        pytest.fail(
            f"generate_ui.py failed (returncode={r.returncode}):\n"
            f"STDOUT:\n{r.stdout}\n"
            f"STDERR:\n{r.stderr}"
        )


# ── Time bound for the doctest binary ──
RUNNER_TIMEOUT_DEFAULT_S = 900


def runner_timeout_s():
    """
    Seconds the C++ runner may take: $P4_CPP_TEST_TIMEOUT_S (tools/run_host_tests.py
    --cpp-timeout sets it), else RUNNER_TIMEOUT_DEFAULT_S. Exceeding it is a FAILURE.

    Generous on purpose: the runner is unoptimised (no CMAKE_BUILD_TYPE) and the two-thread
    ring tests (tests/host/test_pcm_ring.cpp) crawl when the host is busy — on 2026-09-21 a
    green 181-case run took ~41 min under five concurrent idf.py builds and the old fixed
    120 s reported a false failure. On an idle machine the whole suite takes seconds.
    """
    raw = os.environ.get('P4_CPP_TEST_TIMEOUT_S', '').strip()
    if not raw:
        return RUNNER_TIMEOUT_DEFAULT_S
    if not raw.isdigit() or int(raw) <= 0:
        pytest.fail(f"P4_CPP_TEST_TIMEOUT_S={raw!r} is not a positive number of seconds")
    return int(raw)


# ── Cache the build environment so _run_suite can reuse MSYS2 PATH ──
_build_env = None


@pytest.fixture(scope='session')
def built_binary():
    """
    Build the C++ test runner once per session.

    Runs cmake configure + build, then returns the path to the binary.
    Fails the session if configure or build fails.
    """
    os.makedirs(BUILD_DIR, exist_ok=True)
    _ensure_generated()

    compiler = find_compiler()
    if not compiler:
        # A missing toolchain used to be a skip, which let the per-commit gate pass with
        # zero C++ tests executed (audit 2026-09-21). It is a failure: install a compiler,
        # or run tools/run_host_tests.py --no-cpp when a Python-only run is really meant.
        pytest.fail(
            "No host C++ compiler found — the C++ host tests cannot run.\n"
            "  Windows: scoop install mingw   |   winget install MSYS2.MSYS2, then\n"
            "           C:\\msys64\\usr\\bin\\bash -lc 'pacman -S --noconfirm mingw-w64-ucrt-x86_64-gcc'\n"
            "  Linux:   apt-get install g++ cmake ninja-build\n"
            "  Python-only run (explicit opt-out): python tools/run_host_tests.py --no-cpp"
        )

    cmake_exe = _find_cmake()
    if not cmake_exe:
        pytest.fail(
            "cmake not found — install ESP-IDF (it ships cmake) or put cmake on PATH.\n"
            "  Python-only run (explicit opt-out): python tools/run_host_tests.py --no-cpp"
        )

    cmake_args = [
        cmake_exe, '-S', SRC_DIR, '-B', BUILD_DIR,
        '-G', compiler.get('generator', 'MinGW Makefiles'),
    ]
    if compiler.get('gxx'):
        cmake_args += [f'-DCMAKE_CXX_COMPILER={_to_cmake_path(compiler["gxx"])}']
    if compiler.get('gcc'):
        cmake_args += [f'-DCMAKE_C_COMPILER={_to_cmake_path(compiler["gcc"])}']
    if compiler.get('make'):
        cmake_args += [f'-DCMAKE_MAKE_PROGRAM={_to_cmake_path(compiler["make"])}']

    env = _make_env(compiler)

    # Cache env so _run_suite can pass MSYS2 PATH when running the binary.
    # MSYS2-built binaries need ucrt64/bin in PATH to find DLLs at runtime.
    global _build_env
    _build_env = env

    print(f"\n[C++] Compiler: {compiler.get('gxx', '(PATH g++)')}")
    print(f"[C++] CMake:    {cmake_exe}")
    print(f"[C++] Generator: {compiler.get('generator')}")
    if compiler.get('extra_path'):
        print(f"[C++] Extra PATH: {compiler['extra_path']}")

    # ── Configure ──
    r = subprocess.run(cmake_args, capture_output=True, text=True, cwd=ROOT, env=env)
    if r.returncode != 0:
        pytest.fail(
            f"CMake configure failed (returncode={r.returncode}):\n"
            f"STDOUT:\n{r.stdout}\n"
            f"STDERR:\n{r.stderr}"
        )

    # ── Build ──
    r = subprocess.run(
        [cmake_exe, '--build', BUILD_DIR, '--parallel'],
        capture_output=True, text=True, cwd=ROOT, env=env
    )
    if r.returncode != 0:
        pytest.fail(
            f"CMake build failed (returncode={r.returncode}):\n"
            f"STDOUT:\n{r.stdout}\n"
            f"STDERR:\n{r.stderr}"
        )

    # ── Locate binary ──
    exe_win  = os.path.join(BUILD_DIR, 'test_runner.exe')
    exe_unix = os.path.join(BUILD_DIR, 'test_runner')
    if os.path.exists(exe_win):
        return exe_win
    if os.path.exists(exe_unix):
        return exe_unix
    pytest.fail(f"test_runner binary not found in {BUILD_DIR}")


def _run_suite(binary, filter_str=None):
    """
    Run the doctest binary (optionally with a --test-case filter) under runner_timeout_s().
    Uses _build_env so MSYS2-built binaries can find their runtime DLLs.
    Returns (combined_output, returncode, elapsed_s). A timeout FAILS the test — the
    process tree is killed first, so a stuck runner never outlives the gate.
    """
    # Automated runs must not wait on debugger handling or hang indefinitely.
    cmd = [binary, '--no-colors', '--no-breaks=true', '--no-debug-output=true']
    if filter_str:
        cmd.append(f'--test-case={filter_str}')
    timeout = runner_timeout_s()
    t0 = time.monotonic()
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, errors='replace', env=_build_env)
    try:
        out, _ = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        if os.name == 'nt':   # TerminateProcess alone has been seen to leave the runner behind
            subprocess.run(['taskkill', '/F', '/T', '/PID', str(proc.pid)], capture_output=True)
        proc.kill()
        try:
            out, _ = proc.communicate(timeout=30)
        except subprocess.TimeoutExpired:
            out = ''
        pytest.fail(
            f"C++ test runner still running after {timeout} s (P4_CPP_TEST_TIMEOUT_S) — killed "
            f"and treated as a hang.\n"
            f"A busy host can legitimately take this long (test_pcm_ring.cpp's thread tests "
            f"crawl under load): raise the bound with --cpp-timeout if that is the case.\n"
            f"Output so far:\n{out}"
        )
    return out or '', proc.returncode, time.monotonic() - t0


# ════════════════════════════════════════════════════════════════
# Tests
# ════════════════════════════════════════════════════════════════

def test_build(built_binary):
    """Verify the C++ test binary was built successfully."""
    assert os.path.exists(built_binary), f"Binary not found: {built_binary}"
    size = os.path.getsize(built_binary)
    assert size > 1024, f"Binary suspiciously small ({size} bytes)"
    print(f"\n[C++] Binary: {built_binary} ({size // 1024} KB)")


DOCTEST_SUMMARY = re.compile(
    r'\[doctest\] test cases:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed\s*\|\s*(\d+) skipped')


def parse_doctest_summary(out):
    """(total, passed, failed, skipped) from doctest's summary line, or None if it is absent."""
    m = DOCTEST_SUMMARY.search(out)
    return tuple(int(g) for g in m.groups()) if m else None


def test_all_pass(built_binary):
    """Run the full suite: zero failures AND a non-empty suite.

    Until 2026-09-21 a binary that registered no test cases passed as "placeholders
    only — OK", so a runner that had lost every TEST_CASE (a CMake list edit, a wrong
    binary, a filter typo) still turned the gate green. An empty suite is a failure:
    the gate has to prove the C++ tests ran, not merely that a binary exited 0.
    """
    out, rc, elapsed = _run_suite(built_binary)
    assert rc == 0, f"C++ test runner exited with code {rc}:\n{out}"

    summary = parse_doctest_summary(out)
    assert summary is not None, f"doctest summary line not found in the runner output:\n{out}"
    total, passed, failed, skipped = summary

    assert failed == 0, f"{failed} C++ test(s) failed!\n{out}"
    assert total > 0 and passed > 0, (
        f"the C++ runner registered no test cases (total={total}, passed={passed}) — "
        f"the binary ran nothing; check TEST_SOURCES in tests/host/CMakeLists.txt:\n{out}"
    )
    print(f"\n[C++] {passed} test case(s) passed, {failed} failed, {skipped} skipped "
          f"in {elapsed:.1f} s (bound {runner_timeout_s()} s)")
