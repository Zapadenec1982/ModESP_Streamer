**English** · [Українська](PANEL.uk.md)

# Panel: screen, touch, encoder, board

How the Guition JC4880P443C hardware is brought into the firmware: sections of `boards/p4_player/board.json` → HAL
(configuration only) → drivers (own the hardware) → the `ui_lvgl` module through the seam
`components/modesp_hal/include/modesp/hal/lcd_port.h`. Screens, navigation, languages and fonts —
[UI.md](UI.md); audio buses and output — [AUDIO.md](AUDIO.md); the link to the satellite — [LINK_PROTOCOL.md](LINK_PROTOCOL.md);
JP1 and J5 pinout — [CONCEPT.md](CONCEPT.md), §4.

## Board

| Unit | What | Where it is set |
|---|---|---|
| SoC | ESP32-P4, revision v1.3; PSRAM 32 MB (hex, 200 MHz); flash 16 MB | `board.json` (`description`), `boards/p4_player/sdkconfig.board`, `partitions_p4_16mb.csv` |
| Wi-Fi | ESP32-C6 over SDIO: slot 1, 4 bits, 40 MHz, CLK 18, CMD 19, D0–D3 14–17, reset GPIO54 (LOW = reset) | `sdkconfig.board`, [C6_UPDATE.md](C6_UPDATE.md) |
| Screen | 4.3″ 480×800 ST7701S, MIPI-DSI 2 lanes | `board.json` `dsi_displays` |
| Touch | GT911 on I2C | `board.json` `i2c_touch` |
| microSD | slot 0 of the SDMMC controller | `board.json` `sdmmc_slots` |
| On-board codec | ES8311 — as a microphone only | [MIC_CAPTURE.md](MIC_CAPTURE.md) |

The P4 has one SDMMC controller for two slots: slot 1 is initialised by ESP-Hosted for the C6, so the
`sdmmc_card` driver substitutes empty `host.init` / `host.deinit` — otherwise a card mount failure would
also take down the link to the C6.

## The `lcd_port.h` seam

- `ILcdPanel` — the panel: `init()`, `geometry()`, esp_lcd `panel()` / `io()`, `set_backlight(%)`,
  `is_healthy()`.
- `ITouchPort` — the touch controller: `init()`, `handle()` for `lvgl_port_add_touch`.
- `ILcdPort` — what the factory of an `lcd`-category driver returns; `as_panel()` / `as_touch()` tell
  which of the two roles the driver implements (one driver = one role = one hardware id).

The module knows no pins, timings or init tables — it only receives esp_lcd handles for `esp_lvgl_port`.
This seam is separate from `display_port.h` (character OSD displays) and `panel::IPanelPort` (the BLE LED panel).

## `board.json`: panel sections

| Section | Entry |
|---|---|
| `i2c_buses` | `i2c_0` "ES_I2C": SDA 7, SCL 8, 400 kHz; shared by the codec, the touch and JP1 (pins 21/23) |
| `dsi_displays` | `lcd_0`: 2 lanes at 500 Mbit/s, DPI 34 MHz, 480×800, hsync 12 / hbp 42 / hfp 42, vsync 2 / vbp 8 / vfp 166, `color_bits` 24 (RGB888), reset GPIO5, backlight GPIO23 (LEDC 20 kHz, active high), PHY LDO channel 3, 2500 mV, `rotation` 270 |
| `i2c_touch` | `touch_0`: GT911 @0x5D on `i2c_0`; INT and RST not wired (−1) — the touch is polled; `x_max` 800, `y_max` 480; `swap_xy`, `mirror_x`, `mirror_y` — `false` |
| `sdmmc_slots` | `sd_0`: slot 0, 4 bits, CLK 43, CMD 44, D0–D3 39–42, no CD, 20 MHz, LDO channel 4, 3300 mV, mount point `/sd` |
| `encoders` | `enc_0`: A (`clk`) GPIO32, B (`dt`) GPIO28, pull-up |
| `gpio_inputs` | `enc_sw`: GPIO35, pull-up, inverted (pressed = LOW); strapping BOOTMODE, R34 10k pull-up |
| `gpio_outputs` | `amp_enable`: GPIO34, active high — TPA3255 /RESET and the DAC's XSMT (LOW = mute, external pull-down); the HAL sets it OFF at start, the `audio` module turns it on |

The `i2s_buses` and `uart_buses` sections — [AUDIO.md](AUDIO.md), [LINK_PROTOCOL.md](LINK_PROTOCOL.md).

`bindings.json`: `lcd_0 → dsi_st7701` (role `lcd_main`), `touch_0 → gt911` (`touch_main`),
`enc_0 → rotary_encoder` (`encoder_main`), `enc_sw → digital_input` (`encoder_sw`) — all into the
`ui_lvgl` module; `amp_enable → gpio_output` — into `audio`; `sd_0 → sdmmc_card` — into `storage`.

File limits:

- `board.json` and `bindings.json` are read into one buffer `ConfigService::MAX_JSON_SIZE` (4096 bytes,
  `components/modesp_services/include/modesp/services/config_service.h`). The build rejects a bigger
  file (and prints the headroom of both). On the device such a file is rejected with a `ConfigSvc` ERROR
  that names the limit; `config` is a CRITICAL module, so the panel goes into safe mode: no HAL, audio
  or screen, only the network and HTTP for repair. Explanations go in `docs/`, not in a `label`.
- The build copies both files into `data/`, and `data/` goes into `data.bin` of the `data` partition
  (0x430000), not into the app. `idf.py app-flash` does not carry a `board.json` change to the panel —
  a full `idf.py flash` is needed.

## Orientation: landscape 800×480

`rotation: 270` — the UI is landscape 800×480; the board's microphone is then at the top edge of the
picture. Screen rotation (`rotation`) and the touch flags (`swap_xy` / `mirror_*`) are separate
`board.json` settings.

**Screen.** A DPI panel does not swap axes by itself (`esp_lcd_panel_swap_xy` is not supported on it),
so LVGL rotates (`flags.sw_rotate` + `lv_display_set_rotation()`). The module installs its own flush
(`modules/ui_lvgl/src/ppa_flush.cpp`): the PPA rotates each block straight into the DPI frame buffer
(section "Frame path"). If the PPA client or the frame buffer is unavailable, the `esp_lvgl_port` flush
remains — with PPA when `CONFIG_LVGL_PORT_ENABLE_PPA=y` (`sdkconfig.board`), otherwise on the CPU.
`sdkconfig.board` seeds only a **new** `sdkconfig`: an old line `# CONFIG_… is not set` in an existing
`sdkconfig` wins. What actually took effect — `build/config/sdkconfig.json`.

**Touch.** `esp_lvgl_port` does not rotate touch, but LVGL 9.3 does: `indev_pointer_proc()`
(`src/indev/lv_indev.c`) rotates the point of every pointer device by the rotation of its display. The
GT911 reports coordinates in the matrix's frame, so the touch follows `rotation` by itself, and the GT911
flags (`swap_xy` / `mirror_*`) are needed only when the touch does not match the matrix. The bench `tap=`
(`/api/screenshot`) takes the same path in reverse.

Check after any orientation change: press the four corners and look at `ui.touch_x` /
`ui.touch_y` in `GET /api/state` (or the line `UiLvgl: touch x,y`, section "Bench check") —
it should come out as a rectangle of roughly (0,0)…(799,479).

## Drivers

- `drivers/dsi_st7701` (`espressif/esp_lcd_st7701` ^2.0.2, MIPI-DSI): PHY LDO → DSI bus → DBI →
  panel with the vendor's init table (BSP csvke, Apache-2.0) → reset → init → display on → LEDC backlight
  (10 bits, 0 % until the module's first command). Two DPI frame buffers in PSRAM, DMA2D copy.
- `drivers/gt911` (`espressif/esp_lcd_touch_gt911` ^1.2.1): a device on `i2c_0`; with no chip `init()`
  returns false and the module works without touch. The driver accepts `rst_gpio` / `int_gpio` from `board.json`.
- `drivers/rotary_encoder`: PCNT, 2 channels (both edges of both lines — 4 pulses per EC11 detent),
  1 µs glitch filter, a 32-bit accumulator in `update()`, the counter is re-centred before it
  overflows; `read()` returns detents. Binding settings: `steps_per_detent` (4), `invert`.
- `drivers/digital_input` (the encoder button), `drivers/gpio_output` (`amp_enable`).

## Frame path

The `ui_lvgl` module (tasks, pages, `ui.*` state, Kconfig, HTTP routes) — [UI.md](UI.md). This section covers
the panel hardware; paths are relative to `modules/ui_lvgl/`.

- The LVGL task (`esp_lvgl_port` 2.9, LVGL 9.3) — core 0, priority 1, 16 KB stack in PSRAM
  (`CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM`; the task neither writes nor erases flash, so a stack in
  PSRAM is allowed). The LVGL heap is in PSRAM (`CONFIG_LV_USE_CUSTOM_MALLOC` + `src/lv_mem_psram.c`).
  `LV_COLOR_DEPTH` = 24, as `color_bits`.
- Two draw buffers of 480×100 RGB888 each in PSRAM (`buff_dma` is off: the port accepts DMA buffers only
  for RGB565; PSRAM on the P4 is reachable by DMA anyway).
- The PPA (SRM, non-blocking) rotates each block straight into the DPI frame buffer while LVGL draws the
  next one into the second draw buffer (`src/ppa_flush.cpp`). Blocks that the PPA on v1.3 does not
  complete whole go in strips (`include/ppa_geometry.h`), the narrowest are rotated by the CPU.
- `CONFIG_UI_DOUBLE_FB` (default `y`): the frame is assembled in a frame buffer that is not on the glass,
  and the DPI switches to it when the frame is ready — no tearing. DMA2D brings the second buffer up to
  date with a copy; a scrolling list is shifted by DMA2D inside the frame buffer. The display refresh and
  touch polling timers are 15 ms (a DPI frame ≈ 16.5 ms at 34 MHz); on shift frames the LVGL task is raised
  to `CONFIG_UI_SHIFT_PRIORITY` (default 2). The port's `avoid_tearing` is 0, not used.
- Backlight: LEDC 20 kHz, 10 bits, 0 % until the module's first command; the level — `ui.backlight`,
  `ui.standby_bl` ([UI.md](UI.md), "Standby").

## Bench check

The lines below are INFO. The shipping build is at WARN (`CONFIG_LOG_DEFAULT_LEVEL_WARN`), so the
start-up lines are visible only on a build with `CONFIG_LOG_DEFAULT_LEVEL_INFO` in the local `sdkconfig`.
On a running panel the tag opens without reflashing, but only for later events:

```
curl -u <user>:<password> -X POST -H 'Content-Type: application/json' http://<panel>/api/loglevel -d '{"tag":"UiLvgl","level":"info"}'
```

1. Start:
   - `DsiSt7701: [lcd_main] 480x800 RGB24, 2 lanes @ 500 Mbps, DPI 34 MHz, reset GPIO 5, backlight GPIO 23`
   - `Gt911: [touch_main] GT911 @0x5D on 'i2c_0' (400 kHz), 800x480, int=-1 rst=-1, swap=0 mx=0 my=0`
   - `RotaryEnc: [encoder_main] PCNT on GPIO 32/28, 4 edges/detent`
   - `UiFlush: PPA rotates straight into the frame buffer (480x800, RGB888 in, RGB888 out, two buffers, lists shifted)`
   - `UiLvgl: Initialized: panel 480x800, UI 800x480 (rotation 270), touch yes, theme '…'`
   - `UiLvgl: heap before lvgl_port_init` / `heap after LVGL screen` — LVGL, the draw buffers and the
     frame buffers must sit in PSRAM, not eat into DMA memory.

   At WARN level the deviations are visible: `UiFlush: … frames are drawn on the glass` (no second
   frame buffer), `UiFlush: no PPA client`, `UiLvgl: touch init failed — continuing without touch`,
   `UiLvgl: panel init failed — screen disabled`.
2. Knob: turning — `audio.volume` in `GET /api/state` changes by 2 per detent and the volume slider
   appears; press — pause / play (on a station, the mute); hold — menu ([UI.md](UI.md), "Input").
3. Touch: a 30 px ring follows the finger; `ui.touch_x` / `ui.touch_y` and the line `UiLvgl: touch x,y`;
   four corners — section "Orientation".
4. Screenshot: `curl -u <user>:<password> -o shot.bmp http://<panel>/api/screenshot`.

## Limitations

- GT911 INT and RST are not wired in `board.json`: the touch is polled by an LVGL timer.
- The TPA3255 amplifier's FAULT and CLIP_OTW are not wired in: `board.json` has no inputs for them, and
  the firmware does not read them. The driver `drivers/pcf8574_input` is in the tree, but not bound in `bindings.json`.
