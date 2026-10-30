**English** · [Українська](CONCEPT.uk.md)

# P4 Media Player

A network stereo player with DSP on the Guition JC4880P443C panel (ESP32-P4 + ESP32-C6). Sources:
internet radio, DLNA, AirPlay, Bluetooth through a separate ESP32 satellite, USB audio from a computer,
files from microSD. Audio goes through the DSP in float and on to a PCM5102A DAC and a TPA3255 amplifier.
Control: touch and an encoder on the panel, and the web interface; Home Assistant sees the player through DLNA and
Music Assistant. The firmware has MQTT, off by default.

This document describes the system as a whole. The subsystems are in separate documents:

| Document | What it covers |
|---|---|
| [AUDIO.md](AUDIO.md) | the audio core: I2S ports, rates and sessions, the PCM ring, DSP, volume, limiters, outputs, the BT and USB paths, diagnostics |
| [UI.md](UI.md) | the panel interface and the web interface, languages, fonts, icons |
| [NETWORK.md](NETWORK.md) | Wi-Fi through the C6, HTTP API, WebSocket, radio, DLNA, AirPlay, MQTT |
| [LINK_PROTOCOL.md](LINK_PROTOCOL.md) | the byte contract with the BT satellite |
| [PANEL.md](PANEL.md) | panel hardware and `board.json` |
| [C6_UPDATE.md](C6_UPDATE.md) | ESP32-C6 firmware |
| [MIC_CAPTURE.md](MIC_CAPTURE.md), [SOAK_TEST.md](SOAK_TEST.md) | recording with the on-board microphone, SDIO soak test |

## 1. Scope

- One source at a time — `audio.source` (§7).
- The output clock follows the source rate, 44.1–96 kHz (§3).
- One stereo 2.0 output (§7a).
- The hardware is described by `boards/p4_player/board.json` and `bindings.json`, not by code. Two exceptions are on-board
  blocks that the HAL does not configure: SDIO to the C6 (`boards/p4_player/sdkconfig.board`, brought up by
  ESP-Hosted) and the ES8311 codec together with the PA_CTRL of the on-board amplifier (`components/mic_capture`).

## 2. Hardware

### Guition JC4880P443C board

| Block | What it is | How it is used |
|---|---|---|
| ESP32-P4 | 2 × RISC-V 360 MHz, 768 KB SRAM, 32 MB PSRAM (HEX, 200 MHz), 16 MB Boya BY25Q flash (QIO, 80 MHz); chip revision v1.3 (§10) | UI, network stack, decoders, DSP |
| ESP32-C6 | Wi-Fi 6 and BLE, over SDIO | Wi-Fi through ESP-Hosted (§8). The firmware does not enable BLE (`CONFIG_MODESP_BLE_ENABLE=n`) |
| Display | 4.3" IPS 480×800, ST7701S, 2-lane MIPI-DSI, RGB888 | landscape UI 800×480; two frame buffers in PSRAM, 1125 KB each ([PANEL.md](PANEL.md)) |
| Touch | GT911, address 0x5D on ES_I2C; INT and RST not routed | polling |
| microSD slot | SDMMC, slot 0, 4 bits | `/sd`, source `file` (§3a) |
| USB HS, Type-C | OTG HS, wired as a device | UAC2 sound card (§3a) |
| USB FS, Type-C | USB-Serial/JTAG | console and flashing |
| ES8311 + MEMS microphone | codec on ES_I2C and I2S2 | microphone ADC: bass-reflex port measurement (`audio.port_meas`) and recording in a build with `MODESP_MIC_CAPTURE` ([MIC_CAPTURE.md](MIC_CAPTURE.md)). The codec's DAC is not used |
| NS4150 | on-board 3 W amplifier | not used: every time the codec is opened, the firmware sets PA_CTRL (GPIO11) LOW |
| IP5306, TLV62569, MP3202 | power | §6 |

### DAC — PCM5102A

Stereo 32-bit, I2S slave on the P4's I2S0. No MCLK needed: the DAC's internal PLL takes its clock from BCK.
Output 2.1 V RMS (2.97 V peak) single-ended into the TPA3255 input. Two properties of the chip shape
how the audio core works:

- **The zero detector cannot be turned off.** After ~21 ms of exact zeros on DIN the DAC mutes the
  analog output, and coming out of mute is a pop. So the audio core never sends a digital
  zero: in silence the bus carries 1 LSB at 24 bits, −138 dBFS (`DAC_KEEPALIVE`,
  `modules/audio/src/audio_module.cpp`).
- **PLL from BCK.** Retuning the rate stops BCK, and the DAC's PLL locks onto the clock again — an event
  in the analog section that no data mute removes. So the output clock changes only at a
  session boundary (§3).

### Amplifier — TPA3255

Class-D, PVDD 24 V, gain 21.5 dB: it clips when the DAC delivers −9.8 dBFS (2.97 V DAC peak at
full scale). This is the default of `audio.amp_clip_db10` (−98); calibration gives the exact value
([AUDIO.md](AUDIO.md), "Amplifier ceiling"). Inside the DSP, 1.0 is the amplifier ceiling;
the limiter sits 0.5 dB below it and no higher than −1 dBFS. The amplifier's /RESET and the DAC's XSMT are one line,
GPIO34 (§4). The firmware does not read the FAULT and CLIP_OTW outputs. The load is a passive stereo pair,
Denon SC-G101.

### Bluetooth satellite

A separate ESP32 (WROOM) board with its own firmware in a separate repository (ModESP_BT_Satellite).
The ESP32-C6 has no Bluetooth Classic, so A2DP on the panel itself is impossible. The satellite is an A2DP receiver:
it decodes the stream and outputs PCM over I2S. The link codec — SBC, AAC, aptX, aptX HD or LHDC V5 —
is shown in `bt.codec`. AVRCP brings title, artist, album, position, cover art and absolute
volume; transport, pairing and the device list are controlled from the panel and the web interface.

Two connections to the panel:

- **I2S-B** (GPIO33/30/31) — audio. Who clocks it — §3.
- **UART on J5** (GPIO26/27) — everything except audio: COBS frames, protocol 1.10
  ([LINK_PROTOCOL.md](LINK_PROTOCOL.md)), code — `components/sat_link` and `modules/bt`. The panel
  sends PING every 2 s and considers the link lost after 6 s without a single frame from the satellite;
  a satellite with a different `proto_major` is not accepted. LINK_PROTOCOL.md and the shared link headers
  are mirrored in the satellite repository; `tools/check_protocol_mirror.py --satellite <path>`
  compares the copies.

### Encoder

Incremental, A/B and a button. A — GPIO32, B — GPIO28: quadrature PCNT
(`drivers/rotary_encoder`, IDF recipe `examples/peripherals/pcnt/rotary_encoder`), a 1 µs glitch
filter, internal pull-ups. Button — GPIO35 (`drivers/digital_input`, inverted: pressed =
LOW). What the encoder does on the screens — [UI.md](UI.md).

## 3. Signal path and clocking

```
microSD ──── player ───┐
radio, DLNA ── radio ──┤
AirPlay ──── airplay ──┼──► PcmRing (PSRAM) ──┐
computer ─── usb_uac ──┘                      │
BT satellite ── I2S, GPIO31 ──► BT ring ──────┼──► audio task, core 1:
test tone (generator) ────────────────────────┘    envelope → DspChain → output gate
                                                   → I2S0 TX ──► PCM5102A ──► TPA3255 ──► speakers
```

`DspChain` (`components/audio_dsp`): volume leveler (while on) → input gain (section trim, volume, EQ headroom) → subsonic filter
and boost at the port frequency → loudness → bass and treble → PEQ up to 10 bands →
width and mono bass → balance → bass limiter → brickwall with look-ahead. Float inside; 32-bit slots go onto the bus. Details
of each stage — [AUDIO.md](AUDIO.md).

The path is controlled only through SharedState (§9); no sample passes through the message
bus.

### Clock domains

| Domain | What it clocks | Clock source |
|---|---|---|
| I2S0 (`port_main`), master | BCK, WS, DOUT to the PCM5102A; when the satellite is an I2S slave, also the satellite's BCLK and WS | APLL |
| I2S1 (`port_bt`), slave | reception from the satellite, which clocks the link itself | the satellite's clock |
| I2S2 | ES8311 ADC (microphone) | XTAL, independent of the output APLL |
| USB | UAC2 stream from the computer | the pace is set by the panel's clock through the feedback endpoint |

### The output clock follows the source

The producer puts the rate of its frames into PcmRing. The audio task plays out the frames at the old rate, closes
the envelope, retunes I2S0 and everything that depends on the rate (filters, limiter, spectrum bands), and
opens the envelope again.

- **Range 44.1–96 kHz.** The lower bound is the DAC: at 22.05 kHz, BCK in 32-bit slots is only
  1.41 MHz, and the PCM5102A PLL does not lock onto it (`MIN_RATE_HZ`, `drivers/i2s_std`). The upper bound is
  `AudioModule::PLAY_MAX_HZ`: what 192 kHz adds over 96 lies above 48 kHz, which neither the amplifier's
  output filter nor the speakers pass, and DSP at 192 kHz costs twice as much.
  A 176.4 or 192 kHz track plays at 88.2 or 96 kHz: the producer halves the rate.
- **A rate the output does not provide** is converted to a rate of its family (44.1 or 48 kHz and
  multiples) that the output does provide: 22.05 → 44.1, 176.4 → 88.2. The ratio is then simple (here exactly 2), and
  the resampler needs two filters rather than 320, as for 22.05 → 48. The target rate is chosen by
  `audio_core::fallback_rate` (`components/audio_dsp/include/pcm_ring.h`), and converted by
  `components/audio_pipe`.
- **The clock is chosen once per session** — by the first track after a source change. A track at another rate in
  the same session is converted to the session rate with a fixed ratio: retuning on every
  track would make the DAC's PLL click (§2). USB is the exception: when the computer changes the rate, the producer requests
  a new session (`PcmRing::request_new_session`), and the output retunes once, after the
  old frames have played out.

### Bluetooth: who clocks the link

The panel always offers `HOST_CAP_I2S_MASTER` in HELLO. The satellite's reply decides the mode:

| | Satellite is an I2S slave (`DEV_CAP_I2S_SLAVE`, protocol ≥ 1.9) | Satellite clocks the link itself |
|---|---|---|
| BCLK (GPIO33), WS (GPIO30) | P4 outputs: the same I2S0 signals that go to the DAC | P4 inputs, I2S1 |
| DATA (GPIO31) | into I2S0 RX; I2S1 stopped | into I2S1 RX |
| Output rate | stream rate, 32-bit slots | stream rate |
| Drift of the two crystals | none: reception and output on one clock | absorbed by the output clock servo (`audio.bt_clock_follow`, default) or the adaptive ASRC (`components/audio_resampler`) |
| `bt.link_master` | `true` | `false` |

The panel drives GPIO33/30 only while the last HELLO carries `DEV_CAP_I2S_SLAVE`: two drivers on
one wire never meet for longer than one greeting. As a slave the P4 receives too few frames at 96 kHz
(≈ 95,856 of 96,000 per second); frames it clocks itself cannot be lost. Details —
[AUDIO.md](AUDIO.md), "Who clocks the link", and [LINK_PROTOCOL.md](LINK_PROTOCOL.md), "3.5 Who clocks I2S".

### Silence and switching

- The source and the rate change only when both gates are closed: the envelope before the DSP and
  the output gate after it.
- The amplifier follows the source, not the signal: it is on for any source except `off`.
  It switches on into silence, and audio starts after a 300 ms hold at startup and 100 ms afterwards; on a change to
  `off` it switches off once the silence has passed through the whole TX queue. Mute is a fade in the path; it does not touch
  the `amp_enable` line.
- There is never a digital zero on the bus (§2).

## 3a. microSD and USB

### microSD

SDMMC slot 0, 4 bits, 20 MHz: CLK 43, CMD 44, D0–D3 39–42, no card-detect. The pins are the default IOMUX
pins of slot 0 (`soc/sdmmc_pins.h`). The slot's IO is powered from the P4 LDO, channel 4, 3.3 V. The P4 has one
SDMMC controller with two slots, and slot 1 is held by ESP-Hosted for the C6, so the card driver
(`drivers/sdmmc_card`) neither initializes nor deinitializes the controller. FatFs is configured for the
card: 512 B sector, long file names, UTF-8 (`sdkconfig.board`). `modules/storage` mounts it,
`modules/player` plays it.

### USB

- **FS Type-C** — USB-Serial/JTAG: console and flashing. DTR is the P4 reset line.
- **HS Type-C** is wired as a device: CC1/CC2 through 5.1 kΩ to ground (UFP), VBUS is a power
  input. There is no host mode without modifying the board. So USB is a UAC2 device:
  `components/usb_uac`, on TinyUSB 0.21 without `esp_tinyusb`, brings up the UTMI PHY itself and keeps
  the descriptors and `tusb_config.h`. 44.1 / 48 / 88.2 / 96 kHz at the host's choice, 16 or 24 bits,
  asynchronous mode with a feedback endpoint. UAC2 is in the native drivers of Windows 10 1703+, macOS and
  Linux. Source 7 (§7), details — [AUDIO.md](AUDIO.md).
- **The USB interrupt is on core 1** (`usb_audio.core`, default 1): data comes as a packet every microframe,
  and a delay over 125 µs loses the next packet. On core 0 the interrupt is delayed by LVGL and the PPA.
- **The cache is flushed in 1 KB chunks** (`CONFIG_ESP_MM_CACHE_MSYNC_C2M_CHUNKED_OPS`): after every write the I2S driver
  flushes the whole TX buffer under a spinlock with interrupts disabled, and in one
  piece that takes 250–440 µs — longer than USB's 125 µs.
- The VBUS lines of both ports are tied together — §10.

## 4. Pinout

![Panel connections](img/wiring.svg)

### JP1

![JP1 — pins and signals](img/jp1_header.svg)

A 2×13 header. It carries 12 P4 GPIOs — 28–35 and 49–52 (labelled RMII on the module sheet:
that is an alternate function, there is no PHY on the board), plus 3V3, 5V, GND, the ES_I2C bus (pins 21 and
23; GPIO7/8, shared with the codec and the touch controller, 5.1 kΩ pull-ups on the board) and the ESP32-C6 UART0 (pins
18 / 20 / 22 / 24 = U0RXD, U0TXD, IO9 — BOOT, CHIP_PU — reset) — the C6 recovery path
([C6_UPDATE.md](C6_UPDATE.md)).

| GPIO | Signal | Direction | Note |
|---|---|---|---|
| 50 | I2S0 BCK → PCM5102A | output | 64 × fs (32-bit slots) |
| 51 | I2S0 WS → PCM5102A LRCK | output | fs, 44.1–96 kHz |
| 52 | I2S0 DOUT → PCM5102A DIN | output | |
| 49 | I2S0 MCLK | output, LOW | The DAC needs no MCLK. The firmware outputs MCLK only for source 2 (`aux`), otherwise it holds the pin LOW as a GPIO. The pin is not free |
| 29 | I2S0 DIN | input | read only for source 2, which is disabled in this build (§7); while the panel clocks the BT link, I2S0 RX is switched to GPIO31 |
| 33 | I2S-B BCLK | input or output | I2S1 input when the satellite clocks itself; I2S0 BCK output when the satellite is a slave (drive strength 2) |
| 30 | I2S-B WS | input or output | likewise (drive strength 3) |
| 31 | I2S-B DATA ← satellite | input | into I2S1 or I2S0 RX (§3) |
| 32 | encoder A | input | PCNT, pull-up |
| 28 | encoder B | input | PCNT, pull-up; the pin next to A |
| 35 | encoder button | input | strapping (BOOTMODE). On the board, R34 10 kΩ up and the BOOT button (SW1) to ground, in parallel with the encoder button: HIGH at rest |
| 34 | `amp_enable`: TPA3255 /RESET and DAC XSMT | output | strapping. External 10 kΩ pull-down: the amplifier and the DAC stay silent until the firmware starts. HIGH = playing |

- **GPIO34/35 are strapping pins.** Nothing an external chip drives during
  reset is on them; that is why the satellite's clocks are on 33/30/31.
- **Drive strength of the lines to the satellite** (`drivers/i2s_std`, `set_link_master`): BCLK at strength 3
  induces spurious edges on WS — each one gives a shifted slot and a click at the satellite; WS at strength 2
  picks up crosstalk from BCLK, at strength 3 it arrives clean.

### J5 — UART to the satellite

| GPIO | Signal | Far end |
|---|---|---|
| 26 | UART1 TX | satellite RX, GPIO16 |
| 27 | UART1 RX | satellite TX, GPIO17 |

460800 8N1, no flow control, 8 KB receive buffer (`board.json`, `uart_sat`). Both lines
are brought out as TTL on J5: jumpers R70/R75/R76 (0 Ω to the RS485 transceiver) are removed. tx and rx are an
assignment in `board.json`, not a fact of the schematic: if the pair is wired the other way round, two numbers change in
`board.json`; no resoldering is needed.

### On-board pins

| Block | Pins | Where set |
|---|---|---|
| ES_I2C | SDA 7, SCL 8, 400 kHz | `board.json`, `i2c_0` |
| SDIO to C6 | slot 1, 4 bits, 40 MHz: CLK 18, CMD 19, D0–D3 14–17; C6 reset — GPIO54 to CHIP_PU (LOW = reset) | `sdkconfig.board` |
| Display | reset GPIO5, backlight GPIO23 (LEDC 20 kHz), MIPI PHY from LDO, channel 3, 2.5 V | `board.json`, `lcd_0` |
| microSD | §3a | `board.json`, `sd_0` |
| ES8311 | I2S2: MCLK 13, BCLK 12, WS 10, DIN 48; I2C 0x18/0x19 | `components/mic_capture` |
| NS4150 | PA_CTRL GPIO11, LOW while the codec is open | `components/mic_capture` |

## 5. DAC and amplifier connections

PCM5102A:

- SCK → GND: PLL from BCK.
- FMT → GND: I2S format.
- DEMP → GND.
- XSMT ← GPIO34: LOW = mute.
- BCK ← GPIO50, LRCK ← GPIO51, DIN ← GPIO52.
- Output 2.1 V RMS single-ended → TPA3255 input, short analog trace.

TPA3255: /RESET ← GPIO34; input — from the PCM5102A.

## 6. Power

- USB5V_IN → IP5306 → VOUT-BAT → TLV62569 (3V3, 2 A) and MP3202 (backlight boost).
- The IP5306 is a power-bank chip. Without a battery it passes power straight through. With a battery it
  switches the output off at a load below ~45 mA, and a paused player can lose power; so no battery
  is connected.
- The VBUS lines of both USB ports are tied into USB5V_IN without a diode — §10.
- The TPA3255 has its own supply, PVDD 24 V.
- Ground: the analog ground and the board ground meet at one point near the amplifier input; the Class-D power
  ground is separate.

## 7. SharedState contract: sources

Modules are selected independently (`project.json`), and no module depends on another module: shared
code lives in `components/` (the PCM ring in `components/audio_dsp`), and the contract between modules is
SharedState keys. The values of `audio.source` are fixed, because the `audio.sources` bitmask
and the cards of the "Source" screen are indexed by them. They are written in the key's `options` in
`modules/audio/manifest.json`; the generator turns them into `generated/state_enums.h`
(`modesp::gen::AUDIO_SOURCE_*`), and C++ takes the numbers from there (`AudioModule::Source`).

### `audio.source` values

| | id | What plays | Bit in `audio.sources` |
|---|---|---|---|
| 0 | `off` | nothing; the amplifier is off | always |
| 1 | `tone` | internal generator (`audio.tone_*`): through the path, or at an exact DAC level bypassing the path for amplifier calibration | never: it is not something to listen to. Selected by writing `audio.source = 1` |
| 2 | `aux` | line ADC on I2S0 (DIN — GPIO29, MCLK — GPIO49) | only in a build with `CONFIG_AUDIO_LINE_IN_FITTED=y`, off by default (`modules/audio/Kconfig`). Without it a request for 2 is rejected, and the previous source stays |
| 3 | `bt` | BT satellite (§3) | when `port_bt` is bound and the BT ring and the ASRC are allocated |
| 4 | `file` | PcmRing, owner `OWNER_FILE` — `player` (microSD) | when PcmRing is allocated |
| 5 | `radio` | PcmRing, owner `OWNER_RADIO` — `radio`: stations, catalog, an external URL (DLNA) | when PcmRing is allocated |
| 6 | `airplay` | PcmRing, owner `OWNER_AIRPLAY` — `airplay` | when the `airplay` module is in the build (`airplay_offered()`) |
| 7 | `usb` | PcmRing, owner `OWNER_USB` — `usb_audio` | when the USB device has started (`usb_audio_offered()`) |

- `audio.source` is `persist`. A saved source that is not in the mask falls back to `off` at startup.
  A saved ring source waits until the ring is allocated: its owner initializes after the
  `audio` module.
- `audio.state` — what the audio task is playing: the source id, `no-port` (no `port_main` binding) or
  `error`.

### Who switches the source

Whoever produces the audio sets `audio.source` to itself:

| Module | When |
|---|---|
| `player` → 4 | the play or resume command (`player.cmd`) |
| `radio` → 5 | `radio.cmd` play, next, previous; selecting `radio.station`; a station from the catalog; `radio.play_url` |
| `dlna` → 5 | Play from a control point: the module writes the URL to `radio.play_url`, the radio engine plays it |
| `airplay` → 6 | the sender started playing (edge), if `airplay.enabled`. Another source selected on the panel holds until the sender pauses and starts again |
| `usb_audio` → 7 | audio from the computer above −60 dBFS (300 ms hold), and the selected source is not playing. Paused — not playing; a station that is connecting, buffering or reconnecting — playing; the test tone — playing. The computer often keeps the stream open and sends system sounds into it |
| `bt` | never. When BT is not selected and the phone is streaming, the module pauses the phone; on return to BT it lifts only the pause it set itself |
| `audio` → 0 / 1 | port frequency measurement (`audio.port_meas`): silence, then a tone; after the measurement it restores the previous source, if nobody selected another one in the meantime. It switches the tone off itself (→ 0): the calibration tone after 60 s, the through-path tone after 120 s without changes |
| panel, web, MQTT | the user's choice |

### One ring, four producers

`audio_core::PcmRing` (`components/audio_dsp/include/pcm_ring.h`) is an SPSC ring of float stereo in
PSRAM, 24,000 frames (0.5 s at 48 kHz); `player` or `radio` allocates it, and a repeated `init` of the
same size does nothing. Only the owner can write (`claim`/`release`, three owner bits:
`OWNER_USB` = 4); a write from a non-owner is dropped. Before a `claim` that empties the ring, the new
owner asks the consumer to fade out and drop the other owner's queue (`claim_after_fade`); a generation
counter cuts off blocks decoded before the drop. Switching is not a race: the module whose source is
no longer selected sees that in its `on_update`, stops and gives up the ring, and the audio task
drops the previous owner's leftovers while both gates are closed.

### What is playing: `radio.mode`

A track pushed by DLNA plays on the same engine as a station, so `audio.source` stays 5:
this key answers the question "which engine owns the output". The engine publishes what exactly is playing in
`radio.mode`: `station` — a station from the list, `push` — an external URL. Push metadata is in `dlna.*`.
AirPlay has its own number because it is a separate engine (its own RTSP, buffer, decoders, PTP clock); its
card on the "Source" screen, like the Bluetooth card, selects a source that waits for a sender.
Only sources 3–7 have cards, and only those whose bit is in `audio.sources` ([UI.md](UI.md)).

### Source state and commands

| Source | State | Metadata | Commands |
|---|---|---|---|
| `file` | `player.state`: idle, playing, paused, error | `player.title`, `.artist`, `.album`, `.format`, `.pos_s`, `.dur_s` | `player.cmd`: 1 play, 2 pause, 3 stop, 4 next, 5 previous, 6 play/pause; `player.seek_s` |
| `radio` | `radio.state`: idle, connecting, buffering, playing, reconnecting; `radio.mode` | `radio.name`, `.now_playing`, `.format`, `.pos_s`, `.dur_s` | `radio.cmd`: 1 play, 2 stop, 3 next, 4 previous; `radio.station`, `radio.play_url`, `radio.seek_s` |
| DLNA (through `radio`) | `dlna.state` — the UPnP transport | `dlna.title`, `.artist`, `.album`, `.art_uri` | `dlna.cmd`: 1 play, 2 stop, 3 pause, 4 next, 5 previous |
| `bt` | `bt.state` — the connection; `bt.play_state`: idle, playing, paused | `bt.title`, `.artist`, `.album`, `.pos_s`, `.dur_s`, `bt.codec` | `bt.cmd`: 1 play, 2 pause, 3 stop, 4 next, 5 previous, 6 toggle |
| `airplay` | `airplay.state`: off, idle, connected, playing, paused, switching | `airplay.title`, `.artist`, `.album`, `.pos_s`, `.dur_s` | `airplay.cmd` — codes as in `bt.cmd`; acts on the sender that provided the DACP remote (`airplay.remote`) |
| `usb` | `usb_audio.state`: off, waiting, ready, playing | `usb_audio.rate`, `.bits` | — |

- `*.cmd` keys are commands (`"command": true` in the manifest): a write is queued to the module
  (`BaseModule::post_command`, 8 commands), not to the store, and is not broadcast.
- There is one volume: `audio.volume`, 0–100 %, `persist`. `audio.vol_max` (40–100 %) compresses the scale,
  `audio.vol_eff` is what the core applied. Bluetooth, AirPlay and USB translate the volume of the phone or
  computer into `audio.volume` while their source is selected. The volume law — [AUDIO.md](AUDIO.md).

## 7a. Outputs

The firmware plays one stereo 2.0 output: `audio.out_caps` = 1 (`audio_core::OUT_CAPS_NOW`,
`components/audio_dsp/include/out_caps.h`). `eq.json` is sectioned for two outputs
(`MAX_SECTIONS` = 2), has a `mode` field and a `crossover` block; the audio core takes section 0, and
`audio.out_mode` shows what the file asks for (`2.0`, `biamp`, `4.0`, `2.1`). The working file is
`eq.json` in the `userdata` partition (`UserStore`), the factory copy is in the `data` partition. Format —
[AUDIO.md](AUDIO.md).

## 8. Network

Wi-Fi only through the ESP32-C6: `espressif/esp_hosted` 3.0.7 and `esp_wifi_remote` 1.6.4 over SDIO
(slot 1, 4 bits, 40 MHz). Matching the C6 firmware to the host library — §10. The services — HTTP API,
WebSocket, mDNS, radio, DLNA, AirPlay, MQTT — are described in [NETWORK.md](NETWORK.md).

The link's bottleneck is internal DMA-capable SRAM. ESP-Hosted copies every frame into a DMA buffer and
silently drops the frame when the allocation fails; the documented failure threshold of this transport is
6.7 KB of free DMA memory. The I2S descriptors share the same memory. So everything that can lives in
PSRAM:

| What | Setting |
|---|---|
| ESP-Hosted frame copies | `components/hosted_mem`, `CONFIG_HOSTED_MEM_FRAMES_IN_PSRAM=y` |
| ESP-Hosted transport buffers | `CONFIG_EH_HOST_PORT_DMA_PREFER_SPIRAM=y` |
| mbedTLS | `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y`; dynamic buffers, 16 KB input (a server that sends 16 KB records cannot be decrypted with a smaller one), 4 KB output |
| ordinary allocations over 1 KB | `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=1024` |
| Wi-Fi and lwIP buffers | `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y` |
| LVGL heap | `CONFIG_LV_USE_CUSTOM_MALLOC=y`, `modules/ui_lvgl/src/lv_mem_psram.c` |

The ESP-Hosted performance profiles (TCP windows of 32 and 64 KB) take internal RAM that the
C6 receive pool needs, and the link loses packets. The TCP window is 8 × MSS, 11,520 B
(`CONFIG_LWIP_TCP_WND_DEFAULT`): it does not noticeably reduce free DMA memory and pulls a 320 kbit/s stream from
the server in ~200 ms.

## 8a. Board constraints

| Constraint | What follows from it in the firmware |
|---|---|
| Boya BY25Q flash without auto-suspend: every erase or write stops the cache of both cores | `CONFIG_SPI_FLASH_SUPPORT_BOYA_CHIP=y`. The I2S and GDMA interrupts are in IRAM (`CONFIG_I2S_ISR_IRAM_SAFE`, `CONFIG_GDMA_ISR_IRAM_SAFE`): the I2S receive interrupt hands over only the last completed DMA block, and a flash operation holds off a non-IRAM interrupt for longer than one block (1.25 ms at 96 kHz), so the previous block is lost. The DSI interrupt too (`CONFIG_LCD_DSI_ISR_CACHE_SAFE`): the DPI panel's DMA is restarted by this interrupt on every frame, and without IRAM the screen flickers on every flash write. Code and rodata execute from PSRAM (`CONFIG_SPIRAM_XIP_FROM_PSRAM`) |
| A task with its stack in PSRAM crashes on the first NVS write while the cache is disabled | the audio task's stack is in internal SRAM. Tasks that do not work with flash have their stacks in PSRAM: LVGL, radio, cover art, the USB producer |
| Internal DMA SRAM is shared by ESP-Hosted, I2S and everything that asks for DMA | §8. The BT input port is parked with the same slot width, because IDF reallocates the DMA descriptors when the width changes, and a contiguous block may not be found after Wi-Fi |
| The console is the P4's own USB-Serial/JTAG: after a panic the output does not reach the host, DTR resets the P4 | `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`; coredump to flash (`coredump` partition, 64 KB, ELF + CRC32) |
| GT911: INT and RST not routed | touch is polled |
| The DPI panel cannot swap axes | the 270° rotation is done by LVGL through the PPA (`CONFIG_LVGL_PORT_ENABLE_PPA`, [PANEL.md](PANEL.md)) |
| One SDMMC controller for two slots | §3a |

## 8b. Reference projects

| Project | What is taken | Where in the tree |
|---|---|---|
| csvke/esp32_p4_jc4880p433c_bsp (Apache-2.0) | the DSI constants and the ST7701S init table of this panel | `drivers/dsi_st7701` |
| `esp_hosted`, example `examples/ota/coprocessor_ota/mcu_host` | updating the C6 from the P4 over SDIO | `tools/c6_updater` |
| TinyUSB `examples/device/uac2_speaker_fb`; `espressif/usb_device_uac` — TinyUSB without `esp_tinyusb` | UAC2 with a feedback endpoint | `components/usb_uac` |
| ESP-ADF, DLNA example (`release/v2.x`) | the three DLNA renderer services | `components/esp_dlna` (README) |
| rbouteiller/airplay-esp32 | the AirPlay receiver core | `components/airplay2` (UPSTREAM.md) |
| dbry/audio-resampler | the resampler | `components/audio_resampler` |
| IDF `examples/peripherals/pcnt/rotary_encoder` | quadrature encoder | `drivers/rotary_encoder` |

## 9. Firmware architecture

### Control plane — ModESP

The control plane is a fork of [ModESP_Framework](https://github.com/Zapadenec1982/ModESP_Framework)
(remote `upstream`); the edits the P4 needs are kept separate so that they can go upstream.

- **Manifests.** `project.json` lists the modules; each `modules/<name>/manifest.json` declares
  SharedState keys (type, access, default, limits, `options`, `persist`, `command`,
  MQTT), UI pages, priority and the driver roles the module requires. `tools/generate_ui.py`
  validates them against the schemas in `tools/schemas/` and generates `data/ui.json` (`GET /api/ui`) and
  `generated/`: `state_meta.h`, `state_enums.h`, `mqtt_topics.h`, `display_screens.h`,
  `features_config.h`, module registration, the driver list.
- **Hardware.** `board.json` says what is on the board and where; `bindings.json` — which driver serves
  each block and which module gets it in which role. The HAL holds only the config, the drivers
  (`drivers/`) own the hardware, a module receives interfaces by role in `on_bind` (`IAudioPort`,
  `ILcdPanel`, sensors, actuators, a byte link). Each of the two files must be
  smaller than 4096 bytes (`ConfigService::MAX_JSON_SIZE`): on the device both are read into one
  buffer of that size, and `config` is a critical module. The build rejects a larger file.
- **Startup** (`main/main.cpp`): NVS → system services (errors, watchdog, log, config,
  persist, system monitor) → `ConfigService` reads `board.json` and `bindings.json` → HAL →
  `DriverManager` creates the drivers from the bindings → product modules take the hardware (`on_bind`) →
  Wi-Fi and product modules start (`on_init`) → HTTP and WebSocket → main loop. A failure of a
  critical step means safe mode: product modules do not start, the network and HTTP stay up for
  repair, `system.safe_mode` = true; an image still awaiting confirmation after OTA restarts
  instead, and the bootloader rolls back to the previous one.
- **Main loop** — 100 Hz (`MODESP_MAIN_LOOP_HZ`): drivers → modules' `on_update` → watchdog
  (5 s, panic). A new image after OTA is confirmed once Wi-Fi and HTTP are up,
  `audio.state` ≠ `error` and `ui_lvgl` is running; if that does not happen within 60 s — a restart, and the bootloader
  rolls back to the previous image.
- **SharedState** (`components/modesp_core`) — a fixed-size "key → value" store
  (the number of entries is generated from the manifests): key up to 32 bytes; value int32, float, bool
  or a string up to 64 bytes (keys with `"long": true` — up to 256). Access is under a mutex; changed keys
  go as a delta over WebSocket. `PersistService` writes the `persist` keys to NVS (namespace `persist`) with
  a 5 s delay and restores them at startup.
- **Who reads and writes the keys.** The panel (`ui_lvgl`), the web interface (Svelte, `data/www/`) over HTTP
  REST and WebSocket `/ws`, MQTT with Home Assistant discovery (`components/modesp_mqtt`, off by
  default) — all work with the same keys; a write is validated against `state_meta.h`.
  [NETWORK.md](NETWORK.md), [UI.md](UI.md).
- **Files.** The `data` partition (LittleFS) carries the build output: `board.json`, `bindings.json`,
  `ui.json`, the web interface, the factory `eq.json`. User files — `eq.json`,
  `eq_custom.json`, `stations.json`, `last_track`, `devices.json` — live in the
  `userdata` partition, which `idf.py flash` does not rewrite; access goes through `UserStore`
  (`components/user_store`).

### Product modules

| Module | What it does |
|---|---|
| `ui_lvgl` | LVGL 9.3 on MIPI-DSI, touch, encoder ([UI.md](UI.md)) |
| `audio` | the audio core ([AUDIO.md](AUDIO.md)) |
| `storage` | mounting the microSD and its state |
| `player` | files from microSD: queue, tags, cover art, decoding into PcmRing |
| `radio` | internet radio, the Radio Browser catalog, the stream engine for external URLs |
| `dlna` | DLNA renderer and media server browsing ([NETWORK.md](NETWORK.md)) |
| `bt` | the link with the satellite ([LINK_PROTOCOL.md](LINK_PROTOCOL.md)) |
| `airplay` | AirPlay receiver ([NETWORK.md](NETWORK.md)) |
| `usb_audio` | USB sound card ([AUDIO.md](AUDIO.md)) |

### The audio core on core 1

`modules/audio` has its own task, `audio`: core 1, priority 22, 8 KB stack in internal SRAM,
a block of 240 frames (5 ms at 48 kHz). The module owns the `audio.*` keys; from `on_update` it sends the task
commands through a FreeRTOS queue (16 POD commands), and the volume through a `LevelLatch` (the latest value, not
a queue). The task returns only slow state — levels, underruns, the measured rate — through a
POD under a spinlock. The task itself does not write to the log: a log call can block for longer than one block,
so its measurements are printed by the main loop. BT reception is run by the `audio_bt` task (core 1, priority 23): from
I2S1 or, when the panel clocks the link, from I2S0 RX — into the BT ring in PSRAM (1 s at 48 kHz).

### Cores and priorities

| Core | Tasks |
|---|---|
| 1 | `audio` (22), `audio_bt` (23), AirPlay playback `ap_play` (9), USB: `usb_pcm` (23), `usb_dev` (20) and the controller interrupt (`usb_audio.core` = 1) |
| 0 | the ModESP main loop, lwIP `tiT` (`CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU0`), LVGL (1), PPA composition `ui_compose` (6), `player` (5), the radio and DLNA tasks, the cover art tasks of the player, radio and AirPlay, the satellite UART `sat_uart` |
| unpinned | four ESP-Hosted SDIO tasks, priority 21 (`CONFIG_ESP_HOSTED_HOST_DEFLT_TASK_PRIORITY`): above lwIP (18) and system events (20), so as to drain the link, and below audio (22) — the only task with a hard deadline |

The FreeRTOS tick is 1 kHz (`CONFIG_FREERTOS_HZ=1000`): a 5 ms block is not quantized by a 10 ms tick.

### Pinned dependencies

| Component | Version | Why pinned |
|---|---|---|
| `espressif/esp_hosted` | 3.0.7 | the C6 firmware is built from the same release (§10) |
| `espressif/esp_wifi_remote` | 1.6.4 | paired with `esp_hosted` |
| `espressif/esp_audio_codec` | ≥ 2.0, < 2.6 (2.5.0) | from 2.6 on it does not build for P4 revisions below 3.0 (§10). Decoders: MP3, AAC with HE-AAC (SBR, PS), FLAC; WAV and M4A containers |
| `espressif/esp_media_protocols` | 1.0.1 | SSDP, UPnP, SOAP, GENA for DLNA; an archive without `xespv2p2` instructions |
| `espressif/tinyusb` | 0.21.0~2 | `components/usb_uac` keeps its own descriptors and `tusb_config.h` |
| `espressif/libsodium` | 1.0.21 | the version the AirPlay upstream uses |
| `lvgl/lvgl` | ~9.3.0 | |

## 10. Hard constraints

### ESP32-P4 revision v1.3

- The panel's chip is v1.3. IDF 5.5 builds for v3.1+ by default, and such a bootloader does not
  start on v1.x, so `sdkconfig.board`, `tools/c6_updater` and `tools/sdio_soak` set
  `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` and `CONFIG_ESP32P4_REV_MIN_100=y`.
- The component registry does not record revision requirements. A new component is checked against its changelog
  (its own and that of all its dependencies) and its ISA: a prebuilt archive with `xespv2p2` in `Tag_RISCV_arch`
  (`riscv32-esp-elf-readelf -A`) contains instructions that v1.3 does not have. `esp_audio_codec` from 2.6 on
  refuses to build for revisions below 3.0, so it is pinned below 2.6. Code built from
  source (TinyUSB, libsodium) has no such gate.

### Tied VBUS

The VBUS lines of both USB ports are tied into USB5V_IN without a diode. If the panel is powered from its own 5 V and
a computer is connected to the HS port, there are two 5 V sources on one rail. So with a computer on the HS port
the panel is powered either only from that port or through a Schottky diode in the VBUS line of the HS port. While
the panel is powered through the other port, a pulled HS cable does not remove VBUS: the controller sees not a
disconnect but only silence on the bus (suspend, `components/usb_uac/src/uac_usb.c`).

### C6 firmware paired with the host library

- The ESP-Hosted host library on the P4 and the C6 firmware must come from one release. ESP-Hosted compares
  versions at startup: a difference in major — ERROR `eh_init_evt: major version mismatch`, in minor — WARN
  `minor version mismatch`, in patch — compatible. The line with both versions is INFO, and at the shipping log
  level (WARN) the player does not print it ([C6_UPDATE.md](C6_UPDATE.md), section 1).
- The versions are pinned in `components/modesp_net/idf_component.yml` and
  `tools/c6_updater/main/idf_component.yml` and must match.
- The player does not update the C6 itself. The C6 image lives in the `slave_fw` partition (0x630000, 2 MB); it is pushed
  to the C6 over SDIO by a separate project, `tools/c6_updater`, whose partition table repeats the player's,
  so flashing the player after it does not touch `slave_fw`. The procedure and recovery through the C6 UART —
  [C6_UPDATE.md](C6_UPDATE.md).

### Licenses

`esp_audio_codec`, `esp_media_protocols` and `components/esp_dlna` are permitted only with
Espressif products; the AirPlay core (`components/airplay2`) — only non-commercially. The list —
`THIRD_PARTY_NOTICES.md`.
