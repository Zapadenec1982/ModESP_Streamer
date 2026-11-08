#!/usr/bin/env python3
"""
gen_wiring.py — draws the panel's wiring as built: docs/img/wiring.svg (English) and
docs/img/wiring.uk.svg (Ukrainian).

Every board is drawn at one scale, pin pitch 2.54 mm = 20 units. A solid outline follows the
board itself (the panel, the ESP32-DevKitC); a dashed outline is schematic, and so is the order
of its pins. The panel's pins come from boards/p4_player/board.json and docs/CONCEPT.md,
"Pinout", and the script refuses to draw if board.json says otherwise.

The drawing is checked against its own netlist: each net reaches every one of its pins and is
one piece, a wire ends only on a pin of its own net, wires of two nets never overlap or meet,
no wire runs over another net's pad or through a board, and wire labels do not overlap.

Usage:

    python tools/gen_wiring.py                  # write both files
    python tools/gen_wiring.py --check          # exit 1 if a file differs from what would be written
    python tools/gen_wiring.py --satellite <modesp-bt-satellite checkout>
    P4_SATELLITE_ROOT=<checkout> python tools/gen_wiring.py

With a satellite checkout its pins are also checked against its main/board.h.
tools/tests/test_wiring.py runs --check.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
from html import escape
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = {"en": ROOT / "docs/img/wiring.svg", "uk": ROOT / "docs/img/wiring.uk.svg"}

P = 20.0            # 2.54 mm
MM = P / 2.54
W, H = 2040, 1290


# ---------------------------------------------------------------- pin facts against the sources
def check_board() -> list[str]:
    board = json.loads((ROOT / "boards/p4_player/board.json").read_text(encoding="utf-8"))

    def by_id(key, id_):
        return next(b for b in board[key] if b["id"] == id_)

    a, b = by_id("i2s_buses", "i2s_a"), by_id("i2s_buses", "i2s_b")
    enc, uart, i2c = by_id("encoders", "enc_0"), by_id("uart_buses", "uart_sat"), by_id("i2c_buses", "i2c_0")
    facts = {
        "i2s_a mclk, bclk, ws, dout, din": ((a["mclk"], a["bclk"], a["ws"], a["dout"], a["din"]), (49, 50, 51, 52, 29)),
        "i2s_b bclk, ws, din": ((b["bclk"], b["ws"], b["din"]), (33, 30, 31)),
        "enc_0 clk, dt": ((enc["clk"], enc["dt"]), (32, 28)),
        "enc_sw": (by_id("gpio_inputs", "enc_sw")["gpio"], 35),
        "amp_enable": (by_id("gpio_outputs", "amp_enable")["gpio"], 34),
        "uart_sat tx, rx": ((uart["tx"], uart["rx"]), (26, 27)),
        "i2c_0 sda, scl": ((i2c["sda"], i2c["scl"]), (7, 8)),
    }
    return [f"board.json: {k} is {got}, the drawing has {want}" for k, (got, want) in facts.items() if got != want]


def check_satellite(root: Path) -> list[str]:
    path = root / "main/board.h"
    if not path.is_file():
        return [f"satellite: no {path}"]
    h = path.read_text(encoding="utf-8")
    errs = [f"satellite board.h: {n} is not GPIO_NUM_{v}"
            for n, v in (("BOARD_I2S_BCLK_GPIO", 13), ("BOARD_I2S_WS_GPIO", 26), ("BOARD_I2S_DOUT_GPIO", 27))
            if not re.search(rf"#define\s+{n}\s+GPIO_NUM_{v}\b", h)]
    # the DevKitC build: the link on UART2, TX 17, RX 16
    if not re.search(r"UART_NUM_2\s*\n#define\s+BOARD_LINK_TX_GPIO\s+GPIO_NUM_17\s*\n"
                     r"#define\s+BOARD_LINK_RX_GPIO\s+GPIO_NUM_16\b", h):
        errs.append("satellite board.h: the UART2 link is not TX 17, RX 16")
    return errs


# ---------------------------------------------------------------- the drawing
def draw(lang: str):
    def L(en, uk):
        return en if lang == "en" else uk

    NET = {
        "BCK":    ("#0a58ca", L("GPIO50 · I2S0 BCK, 64·fs, to the PCM5102A", "GPIO50 · I2S0 BCK, 64·fs, до PCM5102A")),
        "WS":     ("#2f9bff", "GPIO51 · I2S0 WS"),
        "DOUT":   ("#00337a", "GPIO52 · I2S0 DOUT"),
        "AMPEN":  ("#d1242f", L("GPIO34 · amp_enable: XSMT, /RESET, 10 kΩ to GND", "GPIO34 · amp_enable: XSMT, /RESET, 10 кОм на GND")),
        "BTBCK":  ("#8250df", L("GPIO33 · BCLK, the satellite's IO13", "GPIO33 · BCLK, IO13 супутника")),
        "BTWS":   ("#b083f0", L("GPIO30 · WS, the satellite's IO26", "GPIO30 · WS, IO26 супутника")),
        "BTDATA": ("#55309b", L("GPIO31 · DATA from the satellite's IO27", "GPIO31 · DATA від IO27 супутника")),
        "BTGND":  ("#111418", L("GND, twisted with BCLK", "GND, скручений із BCLK")),
        "UTX":    ("#e8590c", L("J5.2 TX1 (GPIO26) to the satellite's RX2 (IO16)", "J5.2 TX1 (GPIO26) до RX2 супутника (IO16)")),
        "URX":    ("#a84300", L("J5.3 RX1 (GPIO27) from the satellite's TX2 (IO17)", "J5.3 RX1 (GPIO27) від TX2 супутника (IO17)")),
        "ENCA":   ("#1a7f37", L("GPIO32 · encoder A", "GPIO32 · енкодер A")),
        "ENCB":   ("#2da44e", L("GPIO28 · encoder B", "GPIO28 · енкодер B")),
        "ENCSW":  ("#57b56e", L("GPIO35 · encoder button", "GPIO35 · кнопка енкодера")),
        "AL":     ("#57606a", "L"),
        "AR":     ("#8c959f", "R"),
    }
    EXPECTED = {
        "BCK":    ["JP1.GPIO50", "DAC.BCK"],
        "WS":     ["JP1.GPIO51", "DAC.LRCK"],
        "DOUT":   ["JP1.GPIO52", "DAC.DIN"],
        "AMPEN":  ["JP1.GPIO34", "DAC.XSMT", "AMP.RESET", "R1.A"],
        "BTBCK":  ["JP1.GPIO33", "SAT.IO13"],
        "BTWS":   ["JP1.GPIO30", "SAT.IO26"],
        "BTDATA": ["JP1.GPIO31", "SAT.IO27"],
        "BTGND":  ["JP1.GND_R", "SAT.GND"],
        "UTX":    ["J5.2", "SAT.IO16"],
        "URX":    ["J5.3", "SAT.IO17"],
        "ENCA":   ["JP1.GPIO32", "ENC.A"],
        "ENCB":   ["JP1.GPIO28", "ENC.B"],
        "ENCSW":  ["JP1.GPIO35", "ENC.SW"],
        "AL":     ["DAC.L", "AMP.INL"],
        "AR":     ["DAC.R", "AMP.INR"],
    }
    # rails: the pad colour says it, a small tag names it where it is a tie
    TIES = {
        "5V":  ["JP1.5V_1", "JP1.5V_2", "J5.1"],
        "3V3": ["JP1.3V3_1", "JP1.3V3_2", "ENC.+"],
        "GND": ["JP1.GND_L", "JP1.GND_8", "J5.4", "DAC.SCK", "DAC.FMT", "DAC.DEMP", "ENC.GND", "SAT.GND_J3", "SAT.GND_J3B"],
    }
    TIE_COLOR = {"5V": "#e5484d", "3V3": "#f08c00", "GND": "#111418"}
    NC_COLOR = "#c9b26a"

    boards, pads, wires, tags, notes = [], [], [], [], []
    PIN = {}            # "BOARD.NAME" -> (x, y)
    BODY = {}           # board -> (x, y, w, h)
    tie_of = {p: rail for rail, ps in TIES.items() for p in ps}
    net_of = {p: n for n, ps in EXPECTED.items() for p in ps}

    def fmt(v):
        return f"{v:.1f}".rstrip("0").rstrip(".")

    def text(x, y, s, size=11, fill="#1f2328", anchor="start", weight=None, halo=None, rot=None):
        a = f' font-size="{size}" fill="{fill}"'
        if anchor != "start":
            a += f' text-anchor="{anchor}"'
        if weight:
            a += f' font-weight="{weight}"'
        if halo:
            a += f' stroke="{halo}" stroke-width="3" stroke-linejoin="round" paint-order="stroke"'
        if rot is not None:
            a += f' transform="rotate({rot} {fmt(x)} {fmt(y)})"'
        return f'<text x="{fmt(x)}" y="{fmt(y)}"{a}>{escape(s)}</text>'

    def rect(x, y, w, h, fill, stroke="none", rx=0, sw=1.0, dash=None):
        a = f'x="{fmt(x)}" y="{fmt(y)}" width="{fmt(w)}" height="{fmt(h)}" fill="{fill}"'
        if stroke != "none":
            a += f' stroke="{stroke}" stroke-width="{sw}"'
        if rx:
            a += f' rx="{rx}"'
        if dash:
            a += f' stroke-dasharray="{dash}"'
        return f"<rect {a}/>"

    def circle(x, y, r, fill, stroke="none", sw=1.0):
        a = f'cx="{fmt(x)}" cy="{fmt(y)}" r="{fmt(r)}" fill="{fill}"'
        if stroke != "none":
            a += f' stroke="{stroke}" stroke-width="{sw}"'
        return f"<circle {a}/>"

    def pad(pid, x, y, r=6.0):
        """A pad. Its colour comes from its net, its rail, or says 'not used'."""
        assert pid not in PIN, pid
        PIN[pid] = (x, y)
        if pid in net_of:
            col = NET[net_of[pid]][0]
        elif pid in tie_of:
            col = TIE_COLOR[tie_of[pid]]
        else:
            col = NC_COLOR
        pads.append(circle(x, y, r, col, "#ffffff", 1.6))

    def tie_tag(pid, dx, dy, anchor="middle"):
        x, y = PIN[pid]
        rail = tie_of[pid]
        tags.append(("tie", x + dx, y + dy, {"5V": "+5V", "3V3": "+3V3", "GND": "GND"}[rail], TIE_COLOR[rail], anchor, 9.5))

    def tag(net, x, y, s, size=10.5, anchor="start"):
        """A label sitting on its own wire."""
        tags.append((net, x, y + size * 0.36, s, NET[net][0], anchor, size))

    def wire(net, *pts):
        """A wire. Tuples are coordinates, strings are pins."""
        res = [PIN[p] if isinstance(p, str) else p for p in pts]
        for a, b in zip(res, res[1:]):
            assert a[0] == b[0] or a[1] == b[1], (net, a, b)
        for p in pts:
            if isinstance(p, str):
                assert net_of.get(p) == net, (net, p)
        wires.append((net, res))

    # ------------------------------------------------------------ panel: back view, landscape
    PX, PY = 40.0, 300.0
    PW, PH = 114.4 * MM, 66.8 * MM
    PR = PX + PW
    BODY["PANEL"] = (PX, PY, PW, PH)
    XR = PR - 83
    XL = XR - P
    Y1 = PY + 150
    boards.append(rect(PX, PY, PW, PH, "#c7ccd1", "#6e7781", rx=9, sw=1.5))
    boards.append(rect(PX + 61, PY + 3, PW - 122, PH - 6, "#eef1f4", "#8c959f", rx=4))
    boards.append(text(PX + 76, PY + 26, L("JC4880P443C · back view, 114.4 × 66.8 mm", "JC4880P443C · вид ззаду, 114.4 × 66.8 мм"), 15, weight=700))
    boards.append(text(PR - 4, PY + 16, L("frame", "рамка"), 9, "#57606a", "end"))
    for hx, hy in ((PR - 35.5 * MM, PY + 6.3 * MM), (PR - 35.5 * MM, PY + 61.9 * MM)):
        boards.append(circle(hx, hy, 8, "#ffffff", "#8c959f", 1.5))
    boards.append(rect(PR - 29.5 * MM - 24, PY + 6 * MM - 11, 48, 22, "#ffffff", "#8c959f", rx=2))
    boards.append(text(PR - 29.5 * MM, PY + 6 * MM + 4, "BAT", 9.5, "#57606a", "middle"))
    boards.append(rect(PR - 17.7 * MM - 12, PY + 6 * MM - 9, 24, 18, "#ffffff", "#8c959f", rx=2))
    boards.append(text(PR - 17.7 * MM, PY + 6 * MM + 3, "SW3", 8.5, "#57606a", "middle"))
    # the two USB-C ports stand next to JP1
    for uy in (Y1 + 92, Y1 + 196):
        boards.append(rect(XL - 88, uy - 35, 26, 70, "#dfe3e7", "#a4acb5", rx=12, sw=1.2))
    boards.append(text(XL - 215, Y1 + 150, L("USB-C × 2: HS (UAC2), FS (console)", "USB-C × 2: HS (UAC2), FS (консоль)"),
                       10, "#57606a", "middle", rot=-90))
    # JP1, rows top to bottom as on a photo of the header
    boards.append(rect(XL - 11, Y1 - 11, P + 22, 12 * P + 22, "#24292f", rx=3))
    boards.append(text(XL + P / 2, Y1 - 15, "JP1", 11, weight=700, anchor="middle"))
    JP = [("3V3_1", "5V_1"), ("3V3_2", "5V_2"), ("GND_L", "GND_R"), ("GPIO52", "GPIO33"), ("GPIO51", "GPIO31"),
          ("GPIO50", "GPIO30"), ("GPIO49", "GPIO29"), ("GPIO35", "GND_8"), ("GPIO34", "ESP_3V3"),
          ("GPIO32", "C6_U0RXD"), ("GPIO28", "C6_U0TXD"), ("SDA", "C6_IO9"), ("SCL", "C6_CHIP_PU")]
    JPN = {"3V3_1": "3V3", "3V3_2": "3V3", "GND_L": "GND", "5V_1": "5V", "5V_2": "5V", "GND_R": "GND", "GND_8": "GND",
           "SDA": "I2C_SDA", "SCL": "I2C_SCL"}
    for r, (left, right) in enumerate(JP):
        y = Y1 + r * P
        pad(f"JP1.{left}", XL, y, 6.5)
        pad(f"JP1.{right}", XR, y, 6.5)
        notes.append(text(XL - 11, y - 3, JPN.get(left, left), 10, anchor="end", weight=600, halo="#eef1f4"))
        notes.append(text(XR + 11, y - 3, JPN.get(right, right), 10, weight=600, halo="#eef1f4"))
    # J5: pin order from the schematic, sheet 04; its place on the board is not known
    JX = XR + 2
    boards.append(rect(JX - 11, PY + 426, 22, 3 * P + 22, "#ffffff", "#57606a", rx=2, dash="4 3"))
    boards.append(text(JX + 15, PY + 519, L("J5 (place schematic)", "J5 (положення умовне)"), 9.5, "#1f2328", "middle",
                       weight=600, halo="#eef1f4"))
    for i, n in enumerate(("4", "3", "2", "1")):
        pad(f"J5.{n}", JX, PY + 437 + i * P, 6)
    for n, s in {"4": "4 GND", "3": "3 RX1", "2": "2 TX1", "1": "1 5V"}.items():
        x, y = PIN[f"J5.{n}"]
        notes.append(text(x - 10, y + 3.5, s, 9.5, anchor="end", weight=600, halo="#eef1f4"))

    # on-board blocks: docs/CONCEPT.md
    oy = PY + 66
    notes.append(text(PX + 76, oy - 18, L("On the board", "На платі"), 12.5, weight=700))
    for head, body in [
        ("ESP32-P4", L("2 × 360 MHz, 32 MB PSRAM, 16 MB flash, revision v1.3", "2 × 360 МГц, 32 МБ PSRAM, 16 МБ flash, ревізія v1.3")),
        ("ESP32-C6", L("Wi-Fi over SDIO: CLK 18, CMD 19, D0–D3 14–17; reset GPIO54", "Wi-Fi через SDIO: CLK 18, CMD 19, D0–D3 14–17; скидання GPIO54")),
        ("ST7701S", L("4.3\" 480 × 800, 2-lane MIPI-DSI; reset GPIO5, backlight GPIO23", "4.3\" 480 × 800, 2-lane MIPI-DSI; reset GPIO5, підсвітка GPIO23")),
        ("GT911", L("touch, ES_I2C 0x5D, polled", "дотик, ES_I2C 0x5D, опитування")),
        (L("ES8311 + mic", "ES8311 + мік."), "ES_I2C 0x18/0x19; I2S2: MCLK 13, BCLK 12, WS 10, DIN 48"),
        ("NS4150", L("on-board amplifier, not used: PA_CTRL GPIO11 = LOW", "бортовий підсилювач не використовується: PA_CTRL GPIO11 = LOW")),
        ("microSD", L("SDMMC slot 0: CLK 43, CMD 44, D0–D3 39–42", "SDMMC слот 0: CLK 43, CMD 44, D0–D3 39–42")),
        ("USB-C HS", L("UAC2, a sound card for a computer", "UAC2, звукова карта для комп'ютера")),
        ("USB-C FS", L("USB-Serial/JTAG: flashing and console; DTR = reset", "USB-Serial/JTAG: прошивка і консоль; DTR = скидання")),
        (L("Power", "Живлення"), L("USB5V_IN, IP5306, TLV62569 (3V3, 2 A), MP3202 (backlight)", "USB5V_IN, IP5306, TLV62569 (3V3, 2 А), MP3202 (підсвітка)")),
    ]:
        notes.append(text(PX + 76, oy, head, 11, weight=600))
        notes.append(text(PX + 168, oy, body, 10.5, "#3d444d"))
        oy += 19
    oy += 8
    notes.append(text(PX + 76, oy, L("I2S ports", "Порти I2S"), 12.5, weight=700))
    for head, body in [
        ("I2S0 TX", L("master, APLL, 32-bit slots: 50, 51, 52 to the PCM5102A", "майстер, APLL, слоти 32 біти: 50, 51, 52 до PCM5102A")),
        ("I2S0 RX", L("BT from a slave satellite: I2S0 drives 33, 30; DATA on 31", "BT від супутника-slave: I2S0 веде 33, 30; DATA на 31")),
        ("I2S1 RX", L("BT from a satellite that clocks the link: 33, 30, 31", "BT від супутника, що сам тактує лінк: 33, 30, 31")),
        ("I2S2 RX", L("the ES8311's microphone ADC; XTAL", "АЦП мікрофона ES8311; XTAL")),
    ]:
        oy += 19
        notes.append(text(PX + 76, oy, head, 11, weight=600))
        notes.append(text(PX + 136, oy, body, 10.5, "#3d444d"))
    oy += 27
    notes.append(text(PX + 76, oy, L("Sources", "Джерела звуку"), 12.5, weight=700))
    oy += 19
    notes.append(text(PX + 76, oy, L("microSD · radio, DLNA, AirPlay (Wi-Fi over the C6) · USB UAC2 · BT (satellite)",
                                     "microSD · радіо, DLNA, AirPlay (Wi-Fi через C6) · USB UAC2 · BT (супутник)"), 10.5, "#3d444d"))

    # ------------------------------------------------------------ ESP32-DevKitC: the satellite
    SX, SY = 1410.0, 192.0
    SJ2, SJ3 = SX + 10, SX + 210
    SR1 = SY + 38
    BODY["SAT"] = (SX, SY, 220.0, 428.0)
    boards.append(rect(SX, SY, 220, 428, "#1b1f23", "#000000", rx=6))
    boards.append(rect(SX + 39, SY + 4, 142, 201, "#b6bcc3", "#6e7781", rx=3))
    boards.append(rect(SX + 39, SY + 4, 142, 47, "#2d333b", rx=3))
    boards.append(text(SX + 110, SY + 33, L("antenna", "антена"), 10, "#c9d1d9", "middle"))
    boards.append(text(SX + 110, SY + 110, "ESP32-", 12, "#1f2328", "middle", weight=700))
    boards.append(text(SX + 110, SY + 126, "WROOM-32", 12, "#1f2328", "middle", weight=700))
    boards.append(text(SX + 110, SY + 232, L("BT satellite", "BT-супутник"), 13.5, "#ffffff", "middle", weight=700))
    boards.append(text(SX + 110, SY + 250, "ESP32-DevKitC", 10.5, "#c9d1d9", "middle"))
    boards.append(text(SX + 110, SY + 268, "A2DP → I2S, UART2", 9.5, "#c9d1d9", "middle"))
    boards.append(rect(SX + 80, SY + 332, 60, 42, "#2d333b", "#6e7781", rx=2))
    boards.append(text(SX + 110, SY + 357, "USB-UART", 9, "#e6edf3", "middle"))
    boards.append(rect(SX + 42, SY + 386, 26, 20, "#57606a", "#8c959f", rx=2))
    boards.append(text(SX + 55, SY + 400, "EN", 9, "#ffffff", "middle"))
    boards.append(rect(SX + 152, SY + 386, 26, 20, "#57606a", "#8c959f", rx=2))
    boards.append(text(SX + 165, SY + 400, "BOOT", 7.5, "#ffffff", "middle"))
    boards.append(rect(SX + 88, SY + 402, 44, 32, "#8c959f", "#57606a", rx=2))
    boards.append(text(SX + 110, SY + 422, "USB", 9, "#111418", "middle", weight=600))
    J2 = ["3V3", "EN", "VP", "VN", "IO34", "IO35", "IO32", "IO33", "IO25", "IO26", "IO27", "IO14", "IO12", "GND", "IO13",
          "D2", "D3", "CMD", "5V"]
    J3 = ["GND_J3", "IO23", "IO22", "TX", "RX", "IO21", "GND_J3B", "IO19", "IO18", "IO5", "IO17", "IO16", "IO4", "IO0",
          "IO2", "IO15", "D1", "D0", "CLK"]
    J3N = {"GND_J3": "GND", "GND_J3B": "GND", "TX": "TX0", "RX": "RX0"}
    for i, n in enumerate(J2):
        pad(f"SAT.{n}", SJ2, SR1 + i * P, 5.6)
        notes.append(text(SJ2 + 10, SR1 + i * P + 3.5, n, 9.5, "#ffffff", halo="#1b1f23"))
    for i, n in enumerate(J3):
        pad(f"SAT.{n}", SJ3, SR1 + i * P, 5.6)
        notes.append(text(SJ3 - 10, SR1 + i * P + 3.5, J3N.get(n, n), 9.5, "#ffffff", "end", halo="#1b1f23"))

    # ------------------------------------------------------------ encoder: outline and pin order schematic
    EX, EY = 1070.0, 700.0
    BODY["ENC"] = (EX, EY - 13, 170.0, 108.0)
    boards.append(rect(EX, EY - 13, 170, 108, "#f6f8fa", "#57606a", rx=5, dash="5 3"))
    boards.append(circle(EX + 102, EY + 22, 22, "#d0d7de", "#57606a", 1.5))
    boards.append(circle(EX + 102, EY + 22, 10, "#8c959f"))
    boards.append(text(EX + 102, EY + 62, L("Encoder", "Енкодер"), 10.5, weight=700, anchor="middle"))
    boards.append(text(EX + 102, EY + 76, L("PCNT, 4 edges per detent","PCNT, 4 фронти на клацання"), 8.5, "#57606a", "middle"))
    for i, n in enumerate(("A", "B", "SW", "+", "GND")):
        pad(f"ENC.{n}", EX, EY + i * P, 5.6)
        notes.append(text(EX + 9, EY + 3.5 + i * P, n, 9.5, "#1f2328", weight=600))

    # ------------------------------------------------------------ PCM5102A board: outline and pin order schematic
    DX, DY, DW, DH = 1260.0, 845.0, 200.0, 175.0
    BODY["DAC"] = (DX, DY, DW, DH)
    boards.append(rect(DX, DY, DW, DH, "#f6f8fa", "#57606a", rx=6, sw=1.4, dash="5 3"))
    boards.append(text(DX + 130, DY + 21, L("PCM5102A · DAC", "PCM5102A · ЦАП"), 12.5, weight=700, anchor="middle"))
    for i, s in enumerate((L("PLL from BCK, no MCLK", "PLL від BCK, без MCLK"), L("out 2.1 Vrms", "вихід 2.1 Vrms"),
                           L("LOW on XSMT = mute", "LOW на XSMT = тиша"))):
        boards.append(text(DX + 62, DY + 52 + i * 15, s, 10, "#57606a"))
    for i, n in enumerate(("XSMT", "SCK", "BCK", "LRCK", "DIN", "FMT", "DEMP")):
        pad(f"DAC.{n}", DX, DY + 35 + i * P, 5.6)
        notes.append(text(DX + 9, DY + 38.5 + i * P, n, 9.5, weight=600))
    for n, y in (("L", 940.0), ("R", 960.0)):
        pad(f"DAC.{n}", DX + DW, y, 5)
        notes.append(text(DX + DW - 9, y + 3.5, n, 9.5, anchor="end", weight=600))
    for n in ("SCK", "FMT", "DEMP"):
        tie_tag(f"DAC.{n}", -10, 3.5, "end")

    # ------------------------------------------------------------ amplifier and speakers
    AMX, AY = 1600.0, 790.0
    BODY["AMP"] = (AMX, AY, 230.0, 255.0)
    boards.append(rect(AMX, AY, 230, 255, "#f6f8fa", "#57606a", rx=6, sw=1.4, dash="5 3"))
    boards.append(text(AMX + 16, AY + 26, "TPA3255", 14, weight=700))
    boards.append(text(AMX + 16, AY + 44, L("Class-D, gain 21.5 dB", "Class-D, підсилення 21.5 дБ"), 10.5, "#57606a"))
    boards.append(text(AMX + 16, AY + 66, L("PVDD 24 V, own supply", "PVDD 24 В, своє живлення"), 10.5, "#e5484d", weight=600))
    boards.append(text(AMX + 16, AY + 81, L("Class-D ground separate", "земля Class-D окремо"), 10.5, "#57606a"))
    for n, y, s in (("RESET", 900.0, "/RESET"), ("INL", 940.0, "IN L"), ("INR", 960.0, "IN R")):
        pad(f"AMP.{n}", AMX, y, 5.6 if n == "RESET" else 5)
        notes.append(text(AMX + 9, y + 3.5, s, 9.5, weight=600))
    outs = (("L+", AY + 110), ("L−", AY + 130), ("R+", AY + 175), ("R−", AY + 195))
    for lab, oy2 in outs:
        pads.append(circle(AMX + 230, oy2, 5, "#57606a", "#ffffff", 1.4))
        notes.append(text(AMX + 221, oy2 + 3.5, lab, 9.5, anchor="end", weight=600))
    for (_, ya), (_, yb), ch in ((outs[0], outs[1], "L"), (outs[2], outs[3], "R")):
        sx = AMX + 300
        cy = (ya + yb) / 2
        boards.append(f'<path d="M{fmt(AMX + 230)} {fmt(ya)} H{fmt(sx)} M{fmt(AMX + 230)} {fmt(yb)} H{fmt(sx)}" '
                      'fill="none" stroke="#57606a" stroke-width="2.6"/>')
        boards.append(rect(sx, cy - 14, 15, 28, "#ffffff", "#1f2328", sw=1.5))
        boards.append(f'<path d="M{fmt(sx + 15)} {fmt(cy - 14)} l20 -13 v54 l-20 -13 z" fill="#d0d7de" stroke="#1f2328" stroke-width="1.5"/>')
        boards.append(text(sx + 18, cy + 4, ch, 11, weight=700))
    boards.append(text(AMX + 318, AY + 232, "Denon SC-G101", 10.5, "#57606a", "middle"))
    # the 10 k pull-down on the amp_enable line
    RX, RY = 1500.0, 830.0
    PIN["R1.A"] = (RX, RY + 30)
    boards.append(rect(RX - 6, RY + 30, 12, 34, "#ffffff", "#d1242f", sw=1.5))
    boards.append(f'<path d="M{fmt(RX)} {fmt(RY + 64)} v12 M{fmt(RX - 9)} {fmt(RY + 76)} h18 M{fmt(RX - 6)} {fmt(RY + 80)} h12 '
                  f'M{fmt(RX - 3)} {fmt(RY + 84)} h6" fill="none" stroke="#111418" stroke-width="1.6"/>')
    notes.append(text(RX + 11, RY + 51, L("10 kΩ", "10 кОм"), 9.5, weight=600))

    # ------------------------------------------------------------ routing
    # JP1's left column leaves to the left, goes down round the header and turns right under the panel,
    # upper rows outside the lower ones so no two of them cross
    XV = {"DOUT": 704, "WS": 716, "BCK": 728, "ENCSW": 740, "AMPEN": 752, "ENCA": 764, "ENCB": 776}
    YB = {"ENCB": 850, "ENCA": 865, "AMPEN": 880, "ENCSW": 895, "BCK": 920, "WS": 940, "DOUT": 960}
    SRC = {"DOUT": "JP1.GPIO52", "WS": "JP1.GPIO51", "BCK": "JP1.GPIO50", "ENCSW": "JP1.GPIO35", "AMPEN": "JP1.GPIO34",
           "ENCA": "JP1.GPIO32", "ENCB": "JP1.GPIO28"}

    def down(n):
        return (SRC[n], (XV[n], PIN[SRC[n]][1]), (XV[n], YB[n]))

    wire("DOUT", *down("DOUT"), "DAC.DIN")
    wire("WS", *down("WS"), "DAC.LRCK")
    wire("BCK", *down("BCK"), "DAC.BCK")
    wire("AMPEN", *down("AMPEN"), "DAC.XSMT")
    wire("AMPEN", (1220, 880), (1220, RY), (1560, RY), (1560, PIN["AMP.RESET"][1]), "AMP.RESET")
    wire("AMPEN", (RX, RY), "R1.A")
    wire("ENCB", *down("ENCB"), (1025, YB["ENCB"]), (1025, PIN["ENC.B"][1]), "ENC.B")
    wire("ENCA", *down("ENCA"), (1010, YB["ENCA"]), (1010, PIN["ENC.A"][1]), "ENC.A")
    wire("ENCSW", *down("ENCSW"), (1045, YB["ENCSW"]), (1045, PIN["ENC.SW"][1]), "ENC.SW")
    wire("AL", "DAC.L", "AMP.INL")
    wire("AR", "DAC.R", "AMP.INR")

    # the satellite: GND and BCLK run side by side, as the two twisted wires do
    wire("BTGND", "JP1.GND_R", "SAT.GND")
    wire("BTBCK", "JP1.GPIO33", "SAT.IO13")
    wire("BTDATA", "JP1.GPIO31", (1300, PIN["JP1.GPIO31"][1]), (1300, PIN["SAT.IO27"][1]), "SAT.IO27")
    wire("BTWS", "JP1.GPIO30", (1320, PIN["JP1.GPIO30"][1]), (1320, PIN["SAT.IO26"][1]), "SAT.IO26")
    wire("UTX", "J5.2", (980, PIN["J5.2"][1]), (980, 650), (1660, 650), (1660, PIN["SAT.IO16"][1]), "SAT.IO16")
    wire("URX", "J5.3", (965, PIN["J5.3"][1]), (965, 665), (1675, 665), (1675, PIN["SAT.IO17"][1]), "SAT.IO17")

    # ------------------------------------------------------------ wire labels, sitting on the wires
    for n, s in (("ENCB", L("GPIO28 · encoder B", "GPIO28 · енкодер B")), ("ENCA", L("GPIO32 · encoder A", "GPIO32 · енкодер A")),
                 ("AMPEN", "GPIO34 · amp_enable"), ("ENCSW", L("GPIO35 · encoder button", "GPIO35 · кнопка енкодера")),
                 ("BCK", "GPIO50 · BCK 64·fs"), ("WS", "GPIO51 · WS"), ("DOUT", "GPIO52 · DOUT")):
        tag(n, 800, YB[n], s)
    tag("AMPEN", 1240, RY, "/RESET, XSMT")
    for n, pin, s in (("BTGND", "JP1.GND_R", L("GND, twisted with BCLK", "GND, скручений із BCLK")),
                      ("BTBCK", "JP1.GPIO33", "GPIO33 · BCLK"),
                      ("BTDATA", "JP1.GPIO31", L("GPIO31 · DATA from the satellite", "GPIO31 · DATA від супутника")),
                      ("BTWS", "JP1.GPIO30", "GPIO30 · WS")):
        tag(n, 985, PIN[pin][1], s)
    tag("UTX", 1000, 650, L("TX: J5.2, GPIO26, to RX2 (IO16)", "TX: J5.2, GPIO26, до RX2 (IO16)"))
    tag("URX", 1000, 665, L("RX: J5.3, GPIO27, from TX2 (IO17)", "RX: J5.3, GPIO27, від TX2 (IO17)"))
    tag("AL", 1530, 940, "L")
    tag("AR", 1530, 960, "R")

    # ------------------------------------------------------------ text in free space
    hdr = [
        text(40, 44, L("ModESP Streamer: wiring", "ModESP Streamer: схема з'єднань"), 26, weight=700),
        text(40, 70, L("As built: one PCM5102A on I2S0, the BT satellite on I2S-B and the J5 UART, the encoder on PCNT.",
                       "Як зібрано зараз: один PCM5102A на I2S0, BT-супутник на I2S-B і UART J5, енкодер на PCNT."), 13.5, "#3d444d"),
        text(40, 90, L("One scale: pin pitch 2.54 mm = 20 units. A solid outline follows the board; a dashed outline and the order of its pins are schematic.",
                       "Один масштаб: крок виводів 2.54 мм = 20 одиниць. Суцільний контур відповідає платі; пунктирний контур і порядок його виводів умовні."),
             13.5, "#3d444d"),
        f'<path d="M40 116 v8 h{fmt(10 * MM)} v-8" fill="none" stroke="#1f2328" stroke-width="1.5"/>',
        text(40 + 10 * MM + 8, 124, L("10 mm", "10 мм"), 11),
        text(40, 158, L("Where the geometry comes from", "Звідки геометрія"), 13, weight=700),
    ]
    for i, s in enumerate((
        L("Panel: outline 114.4 × 66.8 mm from the Guition manual; JP1, BAT, SW3 and the frame from a photo; JP1 and J5 pin order from the schematic, sheets 03 and 04.",
          "Панель: контур 114.4 × 66.8 мм із мануалу Guition; JP1, BAT, SW3 і рамка за фото; порядок JP1 і J5 зі схеми плати, аркуші 03 і 04."),
        L("ESP32-DevKitC: J2 and J3 pin order from Espressif's documentation; about 28 × 54 mm.",
          "ESP32-DevKitC: порядок J2 і J3 з документації Espressif; приблизно 28 × 54 мм."),
        L("PCM5102A board, TPA3255 board, encoder: drawn without the boards' own pinout, so their outlines are dashed.",
          "Плата PCM5102A, плата TPA3255, енкодер: намальовані без розпіновки самих плат, тому контури пунктирні."),
        L("Pins: boards/p4_player/board.json and docs/CONCEPT.md, “Pinout”; the satellite's: main/board.h in ModESP_BT_Satellite.",
          "Піни: boards/p4_player/board.json і docs/CONCEPT.uk.md, «Розпіновка»; супутника: main/board.h у ModESP_BT_Satellite."),
    )):
        hdr.append(text(40, 178 + i * 19, s, 11.5, "#3d444d"))

    side = []
    LX = 1700
    side.append(text(LX, 66, L("Colours", "Кольори"), 15, weight=700))
    leg = [("BCK", L("I2S0 to the DAC: BCK, WS, DOUT", "I2S0 до ЦАП: BCK, WS, DOUT")), ("AMPEN", "amp_enable, GPIO34"),
           ("BTBCK", L("I2S-B, the satellite: BCLK, WS, DATA", "I2S-B, супутник: BCLK, WS, DATA")),
           ("BTGND", L("ground wire beside BCLK", "дріт землі поруч із BCLK")), ("UTX", L("UART link on J5", "UART-лінк на J5")),
           ("ENCA", L("encoder: A, B, button", "енкодер: A, B, кнопка")), ("AL", L("analog audio", "аналоговий звук"))]
    for i, (n, s) in enumerate(leg):
        y = 88 + i * 20
        side.append(f'<path d="M{LX} {y} h34" stroke="{NET[n][0]}" stroke-width="3" fill="none"/>')
        side.append(text(LX + 44, y + 4, s, 11.5))
    y = 88 + len(leg) * 20 + 6
    for dx, rail, s in ((8, "5V", "+5V"), (78, "3V3", "+3V3"), (150, "GND", "GND")):
        side.append(circle(LX + dx, y, 6, TIE_COLOR[rail], "#fff", 1.2))
        side.append(text(LX + dx + 12, y + 4, s, 11.5))
    side.append(circle(LX + 8, y + 22, 6, NC_COLOR, "#fff", 1.2))
    side.append(text(LX + 20, y + 26, L("not used or not shown", "не використано або не показано"), 11.5))

    foot = []
    fy = 1010
    foot.append(text(40, fy, L("Ties and power", "Перемички і живлення"), 15, weight=700))
    for i, s in enumerate((
        L("PCM5102A: SCK, FMT and DEMP to GND: PLL from BCK, I2S format. XSMT from GPIO34: LOW = mute.",
          "PCM5102A: SCK, FMT і DEMP на GND: PLL від BCK, формат I2S. XSMT від GPIO34: LOW = тиша."),
        L("GPIO34 amp_enable drives the TPA3255's /RESET and the DAC's XSMT. The 10 kΩ to GND keeps both silent until the firmware starts; HIGH = playing.",
          "GPIO34 amp_enable веде /RESET TPA3255 і XSMT ЦАПа. 10 кОм на GND тримають обидва в тиші до старту прошивки; HIGH = грає."),
        L("Encoder: “+” to 3V3 only, not 5 V. A and B use the P4's internal pull-ups; GPIO35 has R34 10 kΩ up and the BOOT button (SW1) on the board.",
          "Енкодер: «+» лише на 3V3, не на 5 В. A і B на внутрішніх підтяжках P4; на GPIO35 на платі R34 10 кОм угору і кнопка BOOT (SW1)."),
        L("Ground: the analog ground and the board ground meet at one point near the amplifier input; the Class-D power ground is separate.",
          "Земля: аналогова земля і земля плати сходяться в одній точці біля входу підсилювача; силова земля Class-D окремо."),
        L("Not shown: power for the PCM5102A board and the satellite. The TPA3255 has its own 24 V supply.",
          "Не показано: живлення плати PCM5102A і супутника. TPA3255 має своє живлення 24 В."),
    )):
        foot.append(text(40, fy + 22 + i * 19, s, 11.5, "#3d444d"))
    fy2 = fy + 22 + 5 * 19 + 18
    foot.append(text(40, fy2, L("Constraints", "Обмеження"), 15, weight=700))
    for i, s in enumerate((
        L("GPIO34 and GPIO35 are strapping pins: nothing an external chip drives during reset sits on them, so the satellite's clocks are on 33, 30 and 31.",
          "GPIO34 і GPIO35 — strapping-піни: на них немає нічого, що зовнішній чип веде під час скидання, тому клоки супутника на 33, 30 і 31."),
        L("BCLK and WS: outputs of I2S0 while the satellite is an I2S slave (link protocol 1.9 or later), inputs of I2S1 otherwise. Drive strength: BCLK 2, WS 3.",
          "BCLK і WS: виходи I2S0, поки супутник — I2S slave (протокол лінку 1.9 і новіше), інакше входи I2S1. Сила драйвера: BCLK 2, WS 3."),
        L("At the satellite BCLK is IO13, next to a GND pin, and its wire is twisted with a ground wire.",
          "На супутнику BCLK — IO13, поруч із контактом GND, і його дріт скручений із дротом землі."),
        L("UART on J5: 460800 8N1, no flow control, COBS frames; UART2 on the satellite. R70, R75 and R76 are removed: J5 is TTL, not RS485.",
          "UART на J5: 460800 8N1, без керування потоком, кадри COBS; на супутнику UART2. R70, R75 і R76 випаяні: J5 — TTL, не RS485."),
        L("J5: pin order from the schematic, sheet 04; which pin is TX is set in board.json. Its place on the board is not known.",
          "J5: порядок виводів зі схеми, аркуш 04; котрий із них TX, задає board.json. Його місце на платі невідоме."),
        L("GPIO49 (I2S0 MCLK) is held LOW and is not free; GPIO29 (I2S0 DIN) is read only by AUX, off in this build. C6_*: the ESP32-C6 recovery path, C6_UPDATE.md.",
          "GPIO49 (I2S0 MCLK) тримається в LOW і не вільний; GPIO29 (I2S0 DIN) читає лише AUX, вимкнений у цій збірці. C6_*: відновлення ESP32-C6, C6_UPDATE.uk.md."),
    )):
        foot.append(text(40, fy2 + 22 + i * 19, s, 11.5, "#3d444d"))

    # ------------------------------------------------------------ geometry helpers
    def on_seg(p, a, b):
        if a[0] == b[0] == p[0]:
            return min(a[1], b[1]) <= p[1] <= max(a[1], b[1])
        if a[1] == b[1] == p[1]:
            return min(a[0], b[0]) <= p[0] <= max(a[0], b[0])
        return False

    def segs(pts):
        return list(zip(pts, pts[1:]))

    def tag_box(x, y, s, size, anchor):
        w = sum(0.72 if c.isupper() else 0.32 if c in " ·,.:" else 0.6 for c in s) * size
        x0 = x if anchor == "start" else x - w / 2 if anchor == "middle" else x - w
        return (x0, y - size * 0.85, x0 + w, y + size * 0.25)

    def wire_svg():
        out, dots, by_net = [], {}, {}
        for net, pts in wires:
            by_net.setdefault(net, []).append(pts)
        for net, polys in by_net.items():
            col, title = NET[net]
            out.append(f'<g class="net" stroke="{col}"><title>{escape(title)}</title>')
            for pts in polys:
                out.append('<path d="M' + " L".join(f"{fmt(x)} {fmt(y)}" for x, y in pts) + '"/>')
            for i, a in enumerate(polys):
                for end in (a[0], a[-1]):
                    for j, b in enumerate(polys):
                        if i != j and any(on_seg(end, s, t) for s, t in segs(b)):
                            dots[(end, net)] = col
            out.append("</g>")
        for (pt, _), col in dots.items():
            if pt not in PIN.values():
                out.append(circle(pt[0], pt[1], 3.6, col, "#ffffff", 1))
        return out

    def tag_svg():
        out = []
        for net, x, y, s, col, anchor, size in tags:
            if net != "tie":
                b = tag_box(x, y, s, size, anchor)
                out.append(rect(b[0] - 3, b[1] - 1, b[2] - b[0] + 6, b[3] - b[1] + 2, "#ffffff", rx=2))
            out.append(text(x, y, s, size, col, anchor, 600, "#ffffff" if net == "tie" else None))
        return out

    # ------------------------------------------------------------ checks
    errors, warns = [], []
    for net, want in EXPECTED.items():
        polys = [p for n, p in wires if n == net]
        if not polys:
            errors.append(f"{net}: no wire")
            continue
        ends = {pt for p in polys for pt in p}
        errors += [f"{net}: does not reach {pid}" for pid in want if PIN[pid] not in ends]
        parent = list(range(len(polys)))

        def find(i):
            while parent[i] != i:
                parent[i] = parent[parent[i]]
                i = parent[i]
            return i

        for i, a in enumerate(polys):
            for j, b in enumerate(polys):
                if i != j and any(on_seg(e, s, t) for e in (a[0], a[-1]) for s, t in segs(b)):
                    parent[find(i)] = find(j)
        if len({find(i) for i in range(len(polys))}) != 1:
            errors.append(f"{net}: not connected")

    # a wire runs over no pad of another net
    for net, pts in wires:
        for s, t in segs(pts):
            for pid, (x, y) in PIN.items():
                if net_of.get(pid) == net:
                    continue
                if s[0] == t[0] and abs(x - s[0]) < 7 and min(s[1], t[1]) - 6 < y < max(s[1], t[1]) + 6:
                    errors.append(f"{net}: vertical passes pad {pid}")
                if s[1] == t[1] and abs(y - s[1]) < 7 and min(s[0], t[0]) - 6 < x < max(s[0], t[0]) + 6:
                    errors.append(f"{net}: horizontal passes pad {pid}")

    # different nets never overlap and never end on each other
    items = [(n, a, b) for n, pts in wires for a, b in segs(pts)]
    crossings = 0
    for i in range(len(items)):
        n1, a1, b1 = items[i]
        for j in range(i + 1, len(items)):
            n2, a2, b2 = items[j]
            if n1 == n2:
                continue
            v1, v2 = a1[0] == b1[0], a2[0] == b2[0]
            if v1 == v2:
                if v1 and a1[0] == a2[0] and max(min(a1[1], b1[1]), min(a2[1], b2[1])) <= min(max(a1[1], b1[1]), max(a2[1], b2[1])):
                    errors.append(f"overlap {n1}/{n2} at x={a1[0]}")
                if not v1 and a1[1] == a2[1] and max(min(a1[0], b1[0]), min(a2[0], b2[0])) <= min(max(a1[0], b1[0]), max(a2[0], b2[0])):
                    errors.append(f"overlap {n1}/{n2} at y={a1[1]}")
                continue
            h, v = ((a1, b1), (a2, b2)) if v2 else ((a2, b2), (a1, b1))
            x, y = v[0][0], h[0][1]
            if min(h[0][0], h[1][0]) <= x <= max(h[0][0], h[1][0]) and min(v[0][1], v[1][1]) <= y <= max(v[0][1], v[1][1]):
                if (x, y) in (a1, b1, a2, b2):
                    errors.append(f"T between {n1} and {n2} at {x},{y}")
                else:
                    crossings += 1

    # wires stay out of board bodies, except the last run into their own pad
    for net, pts in wires:
        for s, t in segs(pts):
            for bname, (bx, by, bw, bh) in BODY.items():
                if bname == "PANEL":
                    continue
                x0, x1 = sorted((s[0], t[0]))
                y0, y1 = sorted((s[1], t[1]))
                if x1 <= bx or x0 >= bx + bw or y1 <= by or y0 >= by + bh:
                    continue
                if not any(p.startswith(bname + ".") and PIN[p] in pts for p in EXPECTED[net]):
                    errors.append(f"{net}: runs through {bname} at {s}-{t}")

    # wire labels do not overlap each other, and no wire of another net runs through one
    boxes = [(tag_box(x, y, s, size, a), s) for net, x, y, s, col, a, size in tags]
    for i in range(len(boxes)):
        for j in range(i + 1, len(boxes)):
            a, b = boxes[i][0], boxes[j][0]
            if a[0] < b[2] and b[0] < a[2] and a[1] < b[3] and b[1] < a[3]:
                errors.append(f"labels overlap: {boxes[i][1]} / {boxes[j][1]}")
    for net, x, y, s, col, a, size in tags:
        box = tag_box(x, y, s, size, a)
        for n, p, q in items:
            if n == net:
                continue
            x0, x1 = sorted((p[0], q[0]))
            y0, y1 = sorted((p[1], q[1]))
            if x0 < box[2] and box[0] < x1 + 0.1 and y0 < box[3] and box[1] < y1 + 0.1:
                warns.append(f"wire {n} under label '{s}'")

    errors += [f"tie {rail}: no pin {p}" for rail, ps in TIES.items() for p in ps if p not in PIN]

    svg = [
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" width="{W}" height="{H}" '
        "font-family=\"Roboto, 'Segoe UI', Arial, sans-serif\">",
        f"<title>{escape(L('ModESP Streamer: wiring', "ModESP Streamer: схема з'єднань"))}</title>",
        "<style>.net path{fill:none;stroke-width:2;stroke-linejoin:round;stroke-linecap:round}"
        ".net:hover path{stroke-width:4.5}</style>",
        rect(0, 0, W, H, "#ffffff"),
    ]
    svg += boards + wire_svg() + pads + notes + tag_svg() + hdr + side + foot + ["</svg>"]
    return "\n".join(svg) + "\n", errors, sorted(set(warns)), crossings


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0], formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="write nothing; exit 1 if a file differs from what would be written")
    ap.add_argument("--satellite", default=os.environ.get("P4_SATELLITE_ROOT"),
                    help="a modesp-bt-satellite checkout, to check its pins too (default: $P4_SATELLITE_ROOT)")
    args = ap.parse_args(argv)

    errors = check_board()
    if args.satellite:
        errors += check_satellite(Path(args.satellite))
    out = {}
    for lang in OUT:
        svg, errs, warns, crossings = draw(lang)
        errors += [f"{lang}: {e}" for e in errs]
        for w in warns:
            print(f"warn {lang}: {w}")
        print(f"{lang}: {len(svg)} bytes, {crossings} crossings")
        out[lang] = svg
    if errors:
        for e in sorted(set(errors)):
            print("ERROR:", e)
        return 1
    if args.check:
        stale = [p for lang, p in OUT.items()
                 if not p.is_file() or p.read_text(encoding="utf-8").replace("\r\n", "\n") != out[lang]]
        for p in stale:
            print(f"stale: {p.relative_to(ROOT)} - run python tools/gen_wiring.py")
        return 1 if stale else 0
    for lang, p in OUT.items():
        with open(p, "w", encoding="utf-8", newline="\n") as f:
            f.write(out[lang])
        print(f"wrote {p.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
