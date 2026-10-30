**English** · [Українська](UI.uk.md)

# Interface: panel and web

Two interfaces over one state. The panel is LVGL on the 4.3" glass (`modules/ui_lvgl`), the web is
a Svelte application (`webui/`, built into `data/www/`). Both read and write SharedState and share
the string catalogues (`webui/src/i18n/<lang>.json`), the colour schemes
(`modules/ui_lvgl/src/ui_m3.cpp`) and the icon font (Material Symbols Rounded).

Related: the panel hardware, `board.json`, orientation and touch — [PANEL.md](PANEL.md); the HTTP
API, WebSocket and authorization — [NETWORK.md](NETWORK.md); the `audio.*` keys, EQ and volume —
[AUDIO.md](AUDIO.md); the `audio.source` values and the SharedState contract — [CONCEPT.md](CONCEPT.md).

## Panel (`modules/ui_lvgl`)

<table>
<tr>
<td><img src="img/screens/panel-player.png" width="260" alt="Player"></td>
<td><img src="img/screens/panel-source.png" width="260" alt="Source"></td>
<td><img src="img/screens/panel-tone.png" width="260" alt="Tone"></td>
</tr>
<tr>
<td><img src="img/screens/panel-equalizer.png" width="260" alt="Equalizer"></td>
<td><img src="img/screens/panel-speakers.png" width="260" alt="Speakers"></td>
<td><img src="img/screens/panel-crossover.png" width="260" alt="Crossover"></td>
</tr>
<tr>
<td><img src="img/screens/panel-stations.png" width="260" alt="Stations"></td>
<td><img src="img/screens/panel-settings.png" width="260" alt="Settings"></td>
<td><img src="img/screens/panel-screen.png" width="260" alt="Screen"></td>
</tr>
</table>


### Module and bindings

Roles from `boards/p4_player/bindings.json`:

| role | driver | required |
|---|---|---|
| `lcd_main` | `dsi_st7701` (`lcd_0`) | yes |
| `touch_main` | `gt911` (`touch_0`) | no |
| `encoder_main` | `rotary_encoder` (`enc_0`: A GPIO32, B GPIO28) | no |
| `encoder_sw` | `digital_input` (`enc_sw`: GPIO35, pressed = LOW) | no |

The module takes the panel and the touch from `DriverRegistry` (category `lcd`, seam
`components/modesp_hal/include/modesp/hal/lcd_port.h`) and sees only esp_lcd handles; the encoder
and the button come from `DriverManager`. Without `lcd_main`, or if the panel did not initialize,
the module runs without a screen: turning the encoder changes `audio.volume` by 2 % per click, a
press toggles `audio.mute`. A touch controller that did not initialize is simply turned off.

### Threads and exchange

- **The main loop** (100 Hz) calls `on_update()`. It is the only thread that writes SharedState on
  the UI's behalf: it reads the encoder, the button, the clock and the state into the POD snapshot
  `UiLvglModule::Snapshot` under a spinlock, publishes `ui.*`, handles the requests from LVGL, drives
  the backlight and standby. It takes the LVGL lock only with a 5–50 ms timeout; if it does not get
  it, it tries again on the next tick.
- **The LVGL task** (`esp_lvgl_port`) makes every LVGL call: the refresh timer copies the snapshot
  and draws the pages, the event callbacks queue requests.
- **Requests** (`UiLvglModule::Req`: source, volume, mute, EQ, theme, language, transport, station,
  seek, brightness, services and so on) go through a 16-entry ring under the same spinlock and are
  dropped on overflow. The Wi-Fi SSID/password pair is passed in separate buffers
  (`lvgl_wifi_connect`); the password is wiped once it has been handed to `WiFiService`.
- Nothing reads SharedState inside a critical section: `read_*` takes a FreeRTOS mutex, and with
  interrupts disabled that is a fault, not a delay.

| what | period |
|---|---|
| page refresh timer (LVGL) | 40 ms |
| encoder, button, touch → `ui.*` | every main-loop tick |
| audio, player, radio, Wi-Fi → snapshot (`mirror_state`) | 50 ms |
| diagnostics, Settings pages, `ui.theme`/`ui.lang` from outside (`mirror_diag`) | 500 ms |
| clock | 1 s |
| LVGL heartbeat for the `lvgl` watchdog | 1 s, alarm after 5 s of silence |

Besides SharedState, the pages read the components' shared objects directly: the EQ document
(`components/audio_dsp/include/eq_store.h`), the card index (`playlist.h`), the station list
(`stations.h`), the DLNA browser snapshot (`dlna_browser.h`), the satellite's settings
(`modules/bt/include/bt_settings.h`), the radio catalogue pages (`radio_catalog_ui.h`), the spectrum
(`audio_core::spectrum_bus()`), the cover (`components/cover`).

### Drawing

- LVGL 9.3.0, `esp_lvgl_port` 2.9.0. The LVGL task: core 0, priority 1 (same as the main loop,
  time-sliced), a 16 KB stack in PSRAM, sleep up to 500 ms, a 5 ms timer tick. The LVGL heap is in
  PSRAM (`CONFIG_LV_USE_CUSTOM_MALLOC` + `src/lv_mem_psram.c`, the component is linked
  `WHOLE_ARCHIVE`).
- Panel 480×800, `color_bits` 24, `rotation` 270 → a logical 800×480 landscape screen,
  `LV_COLOR_DEPTH` 24 (RGB888). All UI geometry is in logical pixels.
- Two draw buffers of 480×100 RGB888 pixels in PSRAM (`buff_dma` off: the port accepts DMA buffers
  only for RGB565).
- LVGL does the rotation (`sw_rotate` + `lv_display_set_rotation`): a DPI panel cannot swap axes.
  The port's flush is replaced by a direct flush, `src/ppa_flush.cpp`: the PPA rotates each block
  straight into the DPI frame buffer, without blocking, and LVGL meanwhile draws the next block into
  the second buffer. If there is no frame buffer or PPA client (WARN `UiFlush`), the `esp_lvgl_port`
  flush is kept.
- Blocks for which this chip's PPA (v1.3) raises no completion interrupt (esp-idf#19023) go in
  strips of safe geometry (`include/ppa_geometry.h`, test `tests/host/test_ppa_geometry.cpp`); the
  CPU rotates those that are too narrow.
- `CONFIG_UI_DOUBLE_FB` (default `y`, needs two DPI frame buffers): the frame is composed in a
  buffer that is not on the glass, and the DPI switches to it at the end of the frame, so there is no
  tearing; before the next frame the changes are copied into the other buffer by DMA2D
  (`esp_async_fbcpy`). A scrolled list (`VList`) is not redrawn but shifted by DMA2D inside the
  buffer, and LVGL draws only the strip that came into view. In this mode the display refresh and
  touch polling period is 15 ms (a panel frame ≈ 16.5 ms), and for shift frames the LVGL task is
  raised to `CONFIG_UI_SHIFT_PRIORITY` (default 2).
- `CONFIG_LCD_DSI_ISR_CACHE_SAFE` requires the DPI callbacks in IRAM, so the module registers its
  own IRAM callback `on_color_trans_done` instead of the port's; `avoid_tearing` = 0.
- Long lists are `m3w::VList` (`src/ui_m3_widgets.h`): a pool of 10 rows over an invisible spacer as
  tall as the whole list; mapping entries to rows is `include/vlist_window.h`
  (test `tests/host/test_vlist_window.cpp`).
- The spectrum is one object that draws all 28 bars itself: 28 separate objects overflow LVGL's list
  of invalidated areas, and it redraws the whole screen.

The module's Kconfig (`modules/ui_lvgl/Kconfig`):

| option | default | what |
|---|---|---|
| `UI_DOUBLE_FB` | y | frame composed in the second buffer, list shifting |
| `UI_SHIFT_PRIORITY` | 2 | LVGL priority on shift frames (0 — leave unchanged) |
| `UI_SCREENSHOT_OPEN` | n | bench routes without login, `tap=`/`hold=` in the screenshot |
| `UI_SCROLL_BENCH` | n | `GET /api/ui/scrollbench` without login |
| `UI_M3_TEST_SCREEN` | n | the `m3test` page with every component |

The bench options (`UI_SCREENSHOT_OPEN`, `UI_SCROLL_BENCH`, `UI_M3_TEST_SCREEN`) are set only in a
local `sdkconfig`; `boards/*/sdkconfig.board` does not have them. With `UI_SCREENSHOT_OPEN` or
`UI_SCROLL_BENCH` the firmware logs a WARN on every boot that the route is open without login.

### Shell and navigation (`src/ui_shell.cpp`)

The 800×480 grid (`uig` in `src/ui_common.h`): a 48 px header at y = 16, content 752×384 from
(24, 74), a 340 px drawer, an invisible 18 px strip along the left edge. There are no permanent side
panels.

All pages are built once and shown/hidden. Each page is a `UiScreenDef` (`id`, `build`,
`refresh`, optional `shown` and `back`); `id` is what is published in `ui.screen`. The `PAGES`
table in `ui_shell.cpp` defines three kinds of page:

- **HOME** — the Player: ≡ in the header, no title;
- **SECTION** — a menu section: ≡ and a title;
- **LIST** — a page opened from another: ← and a title.

Header: ≡ or ← (a 64 px button), the title, the Wi-Fi icon (dimmer without a connection) and the
clock `HH:MM` (`--:--` while the year is < 2020, i.e. until SNTP or a manual time).

**Menu** — a single modal drawer with three sections: Player · Source · Settings; at the top, the
device name (`device_name.h`); at the foot, "Web interface" over the panel's address (`wifi.ip`) and
beside it a QR code of the web's home page (`http://<address>/`), dark on a white square. Without
an address there is neither the address nor the code. It opens with ≡, a press on the left edge or a hold of the encoder
button; it closes with a tap outside it, its own button, picking a row or another hold. The
highlighted row is the current page's section, or the one the list was opened from.

The QR code exists only in a build with `CONFIG_LV_USE_QRCODE=y` (`boards/p4_player/sdkconfig.board`;
an existing `sdkconfig` with the line `# CONFIG_LV_USE_QRCODE is not set` takes the option only once
that line is deleted and `idf.py reconfigure` is run), otherwise only the address.

**Back.** A list remembers the non-list page it was opened from (`from`) and the list that led to it
(`parent`). ← first calls the page's own `back()` (DLNA, the catalogue, Bluetooth and Wi-Fi have a
hierarchy of their own), then goes to `parent`, then to `from`. The station tabs (`radio` and
`catalog`) are one page; moving between them creates no `parent`.

**Overlays**, bottom to top: the volume pop-up, the menu, the "Sound" side sheet (384 px, on the
right), the pick list (`src/ui_pick.cpp`, up to 9 items), the "Undo" snackbar, the standby screen,
the Bluetooth pairing dialog; above everything, the touch ring (30 px, follows the finger).

- **Volume** — a 480 px slider at the bottom; it appears on any change of `audio.volume`/`audio.mute`
  (encoder, web, MQTT) and disappears 2 s after the last one; the speaker button on the Player calls
  it the same way. The first value after boot does not call the slider.
- **Pick list** (presets, EQ band type, language, AirPlay mode) — a dialog: the title and "Cancel"
  on one line, 64 px rows under them, the current one filled and with a check mark. A short list's
  card fits its size; a long one shows four rows and half of the fifth and scrolls. A row picks and
  closes; "Cancel" or a tap outside the card closes without picking.
- **"Undo"** — a 5 s snackbar (`touch::UNDO_MS`) after a finger moves Bass, Treble, balance or an EQ
  band, or after "Custom" is reset; a new one replaces the previous one.
- **Pairing** — a satellite request (protocol 1.8, [LINK_PROTOCOL.md](LINK_PROTOCOL.md)) shows over
  any page and wakes the screen: a numeric code with "Connect"/"Reject", consent without a code, or a
  code to enter on the phone with "Hide". The result shows for 2.5 s. An answer given in a web tab
  removes the buttons and says where it was answered; the first answer counts. Only once the shell
  is built does the panel tell the satellite that it can confirm.

### Panel pages

| `ui.screen` | page | kind | opened from |
|---|---|---|---|
| `now` | Player | HOME | menu; after picking a source, station, track |
| `src` | Source | SECTION | menu; the source line on the Player |
| `set` | Settings | SECTION | menu |
| `radio` | Stations: Favourites | LIST | the list button on the Player (radio) |
| `catalog` | Stations: Top · Countries · Genres · Search | LIST | the station tabs |
| `library` | Library | LIST | the list button on the Player (card) |
| `dlna` | DLNA (media servers) | LIST | the list button (DLNA track), the "Media servers" card |
| `sound` | Tone | LIST | Settings › Sound |
| `eq` | Equalizer | LIST | "Sound" (sheet or page) |
| `speakers` | Speakers | LIST | Settings › Sound |
| `xover` | Crossover | LIST | Settings › Sound |
| `display` | Screen | LIST | Settings › Device |
| `svc` | Service | LIST | Settings › Device |
| `wifi` | Wi-Fi | LIST | Settings › Connections |
| `bt` | Bluetooth | LIST | Settings › Connections |
| `m3test` | M3 components | LIST | only with `UI_M3_TEST_SCREEN`, `?screen=m3test` |

Two more `ui.screen` values: `boot` until the screen is built (and for good without a panel) and
`standby` for the duration of standby.

**Player** (`screen_now.cpp`). One layout for every source: the cover tile on the left, the track
block on the right, the spectrum strip at the bottom; above the block, where the sound comes from
(source and device) and the "Sound" button; the background is the cover colour (`ui.cover_rgb`)
fading into the surface. What does not apply to the source is greyed out, not hidden. The title
takes up to two lines; a one-line title sits right above the line under it (the artist), the free
space goes above the title. The codec badge and the USB line are in the language's units and decimal
separator (`i18n::badge`): "44.1 kHz 128 kbit/s", "LDAC 96 kHz"; two channels are not written, a
whole number goes without ".0". Transport:

- radio — `radio.cmd` (1 play, 2 stop, 3 next, 4 previous);
- card — `player.cmd`; "Play" with no track open starts the track at the queue cursor;
- a DLNA track, Bluetooth, AirPlay — `Req::TRANSPORT`, which the main loop routes to `dlna.cmd`,
  `bt.cmd` or `airplay.cmd`;
- Bluetooth without a phone and AirPlay without a DACP remote (`airplay.remote`) — the buttons are
  inactive; Bluetooth without a phone shows "Turn on visibility" in place of the transport if new
  phones cannot see the panel;
- tone, AUX, USB, output off — there is no transport.

Seeking — `player.seek_s` (card) or `radio.seek_s` (a DLNA track, when `radio.seekable`). Repeat and
shuffle — `player.repeat`, `player.shuffle`. The list button leads to the list of the source that is
playing. The moon after "Sound" switches "Volume leveling" (`audio.leveler`, `Req::LEVELER`;
[AUDIO.md](AUDIO.md)): filled in the accent colour while it is on; the web's Player has the same
moon beside its "Sound" button.

**Source** (`screen_src.cpp`). A grid of cards in the order Bluetooth, AirPlay, Radio / Card,
Media servers, USB; a card is there only when its bit is set in `audio.sources`. Each card says what
it gives now (phone, station, number of tracks). A tap sets `audio.source` and opens the Player;
"Radio" also starts a station; "Media servers" with no track opens the browser. The cards cover
`audio.source` values 3–7; the other values are set only by the API and MQTT.

**Settings** (`screen_set.cpp`) — four groups of 64 px rows:

| group | rows |
|---|---|
| Sound | Tone (preset on the second line), Speakers, Crossover |
| Device | Screen, Language (a dialog of four languages), Service (firmware version) |
| Connections | Wi-Fi (network · address), Bluetooth (phone · codec) |
| Services | AirPlay (announcement switch; a tap picks the mode), DLNA (switch) |

**Lists.** Stations — six buttons (presets) at the top (a tap tunes, a long press saves the current
station, a tap on an empty one while playing saves too) and the whole list, where a station on a
button has "button N" in its caption; a tap tunes (`radio.station`) and returns to the Player. The
catalogue reads what the radio module got from Radio Browser (`radio_catalog_ui.h`), in pages of up
to 12 rows; search uses the keyboard `src/ui_search_kb.cpp` (the panel language's letters and a
second alphabet). Library — the whole card index, opens at the queue cursor. Media servers —
servers › folders › tracks; "+" queues a track next; the `dlna.browse_cmd` commands carry the
snapshot revision, so a tap on a row that has changed does nothing. Server discovery starts when the
page opens and there are no servers yet.

**Sound and EQ.** The "Sound" sheet and the Tone page are the same controls (`sound_sheet.cpp`): the
preset field (pick list), Bass and Treble (−6…+6 dB), loudness compensation (the `audio.loudness`
switch), the way to the equalizer. The equalizer (`screen_eq.cpp`) draws the frequency response with
the same function, `audio_core::eq_magnitude_db()`, that the audio core computes the biquads with;
the band points are dragged with a finger. At the top: the PEQ switch captioned "On" (the row takes
a finger anywhere), the preset field and, while "Custom" plays, "Reset “Custom”". Edits go to a local
copy of the document, the main loop puts it into `audio_core::eq_store()`, and the `audio` module
does not write `eq.json` while `ui.screen` = `eq` (a LittleFS write stops the cache of both cores).
An edit from the web reaches the page through the same document ([AUDIO.md](AUDIO.md)).

The bands on the glass and on the web are one model. Every band of the set is shown at its place,
on or off: "Band 4" is point 4 on both; an off band is a ring on the graph and, in the web's list, a
dimmed row with "Off". "On" in the band's panel turns it on and off, the numbers stay; moving a point
or stepping a parameter on an off band turns it on. Bands are not deleted; "+ Add a band" exists on
the web only for a set shorter than 10 (import). The third parameter's caption is "Slope" for a
named LPF/HPF slope (LR 12, BW 12), otherwise "Width (Q)". The minus in the dB grid labels is "−"
(U+2212).

**Speakers** (`screen_speakers.cpp`) — two cards. The first is the state of the speaker setup (as
`lib/speakers.js` on the web) and "Set them up in the web interface" with the panel's address
(`wifi.ip`) under it; the web's QR code is in the menu. The second is balance −6…+6 dB in 0.5 dB
steps (`audio.balance_db10`).
The speaker setup itself is a wizard on the web. **Crossover** — output schemes; only those in
`audio.out_caps` are available; the board has one stereo pair. **Screen** — brightness, standby
brightness, auto sleep (0, 5, 10, 15, 20, 30, 45, 60 min), scheme.

**Service** (`screen_svc.cpp`) — three big numbers (audio CPU, the longest main-loop pass, free
internal SRAM) and three columns of detail (audio and scheduler, heap, satellite link). A number's
colour is a verdict by the thresholds in `include/service_readout.h` (test
`tests/host/test_service_readout.cpp`); a row is redrawn only when the value or the verdict changes.

**Wi-Fi** — the current network (or the panel's own access point, or an attempt), "Forget", the list
of networks from a scan (a separate task, `net_scan`: a scan over the C6 RPC takes seconds) and a
password panel with a keyboard (Latin letters, digits, ASCII symbols). Connecting is
`WiFiService::try_connect()`: the old pair stays until the new one gets an address. The signal comes
from the scan: the firmware does not write `wifi.rssi` ([NETWORK.md](NETWORK.md)). **Bluetooth** —
the phone and a disconnect button, the satellite's settings (radio, visibility, auto-connect, name),
codecs, paired phones; a switch shows the satellite's answer, not the press. A satellite without
protocol 1.8 gives no settings — the phone and disconnecting remain.

### Input

**The encoder** (EC11 on PCNT, 4 edges per click — [PANEL.md](PANEL.md)) does the same thing on
every page, over sheets, lists and dialogs (`ui_shell_encoder`):

- rotation — `audio.volume` ±2 % per click (0…100); if mute is on, it is cleared;
- a press shorter than 650 ms — the Player's play/pause button for the current source
  (`ui_now_play_pause`);
- a 650 ms hold — the menu (open/close), fires while the finger is still down; over a pick list, the
  "Sound" sheet or a dialog it does nothing;
- in standby, a rotation or a press wakes the screen.

Selection on the glass is by touch only.

**Touch.** GT911 through `lvgl_port_add_touch`; LVGL rotates the point for the display rotation. A
touch writes `ui.touch_x/y` and `ui.touch_down` and resets the idle timer. The finger rules are
`include/touch_gesture.h` (test `tests/host/test_touch_gesture.cpp`):

- a slider takes only a finger on its knob (±32 px from its centre); a gesture becomes a slider
  gesture after 8 px along (more than across), and the slider moves from where it is rather than
  jumping to the finger; a press, a tap or a finger beside the slider changes nothing;
- an EQ point: a tap selects the band; the point moves after a 300 ms hold or 8 px of movement;
- scrolling a page never sets anything.

**Modality.** A dialog or a keyboard is marked modal (`m3w::mark_modal`); a UI rebuild from outside
waits for them. Dialogs are at screen level, not page level (the pick list, pairing, "Forget “…”?"
on Wi-Fi and Bluetooth): their scrim covers the header with ← as well.

### Standby

- Turned on by the idle timer after `ui.standby_min` minutes (0 — never); the encoder, a touch, a tap
  on the standby screen or a pairing request wakes the screen.
- Two layouts: while music plays — the clock (160 px digits), the date, one line, *artist · title* or
  *station · what is playing* (scrolls if wider than the 640 px window) and the 28-bar spectrum;
  without music — only the clock and the date, centred.
- The playing state turns on at once and goes off after 4 s of silence, so that a track change or a
  radio rebuffer does not switch the layout.
- Backlight: outside standby — `ui.backlight`; in standby with music — `ui.standby_bl`, without
  music — 5 %. A change of `ui.standby_bl` outside standby is shown for 1.5 s. Standby dims the
  backlight rather than turning it off: a zero PWM duty cannot be told from a dead panel.
- The `audio` module turns the spectrum analyser on only for `ui.screen` = `now`, `radio` or
  `standby`.

### Colour schemes

The Material 3 tokens are `src/ui_m3.h` (colour roles, radii, font roles) and `src/ui_m3.cpp`
(the `THEMES` table). Four dark schemes, `ui.theme`:

| `ui.theme` | scheme | primary |
|---|---|---|
| 0 | Amber | `#FFB95F` |
| 1 | Blue | `#7A82C0` |
| 2 | Green | `#9A9973` |
| 3 | Red | `#C45D4A` |

Pages name a role, not a colour or a font. The `UiTheme` set (`include/ui_theme.h`), which the
`src/ui_helpers.cpp` helpers, the Service page and the touch ring read, is filled from the same
roles (`src/ui_theme.cpp`). Blue, Green and Red are the output of `tools/ui/m3_scheme.py` from seed
colours: `python tools/ui/m3_scheme.py 7A82C0 9A9973 771910@52` prints the lines for `ui_m3.cpp`
(`@52` is the primary tone for a seed too dark to be primary itself).

Widgets bake colours and texts in when they are created, so a change of scheme or language rebuilds
the whole tree (`rebuild_look`) under the LVGL lock: the same page, the same `from`/`parent`, and the
standby screen comes back if it was up. The rebuild waits while a dialog or a keyboard is open
(except in standby) and tries again on the 2 Hz tick. The duration goes to the log as
`UI rebuilt in N ms` — at WARN level if longer than 100 ms.

### State `ui.*`

The keys have the prefix `ui.`, not `ui_lvgl.` (API/MQTT path `ui/…`); the generator warns about
this.

| key | type | access | default | range | persist | MQTT |
|---|---|---|---|---|---|---|
| `ui.screen` | string | read | — | page id, `boot`, `standby` | | |
| `ui.cover` | int | read | 0 | 0 — none; changes with every new cover | | |
| `ui.cover_rgb` | int | read | 0 | Player background, `0xRRGGBB`; 0 — none | | |
| `ui.backlight` | int | rw | 80 | 0…100 %, step 5 | yes | pub, sub |
| `ui.standby_bl` | int | rw | 25 | 5…100 %, step 5 | yes | sub |
| `ui.theme` | int | rw | 0 | 0…3 | yes | sub |
| `ui.lang` | int | rw | 0 | 0 uk, 1 en, 2 de, 3 pl | yes | sub |
| `ui.standby_min` | int | rw | 5 | 0…60 min, step 5; 0 — never | yes | sub |
| `ui.encoder_pos` | int | read | 0 | clicks since boot | | pub |
| `ui.encoder_sw` | bool | read | false | | | |
| `ui.touch_down` | bool | read | false | | | |
| `ui.touch_x`, `ui.touch_y` | int | read | 0 | px, logical | | |

Persist is NVS through `PersistService` (written 5 s after the last change, no later than
60 s). MQTT is off by default
([NETWORK.md](NETWORK.md)). The panel picks up `ui.theme` and `ui.lang` changed from the web or MQTT
on the 2 Hz tick.

### Module HTTP routes

Registered through `modesp::http_routes::add` and behind the web interface login unless stated
otherwise.

| route | what |
|---|---|
| `GET /api/screenshot` | a 24-bit BMP of what is on the glass, from the frame buffer (`src/ui_screenshot.cpp`) |
| `GET /api/cover?px=` | the Player's cover as a BMP thumbnail (default 140 px), 404 without a cover |
| `GET /api/library?from=&n=` | the card index, one page (`n` ≤ 200, default 100), JSON `{gen, busy, count, cursor, root, from, rows}` |
| `GET /api/i18n/overflow[?all=1]` | texts on the glass that do not fit (see "Width check") |
| `GET /api/ui/frames?ms=&plain=` | frame times of what the screen draws by itself (1–20 s, default 5) |
| `GET /api/ui/scrollbench` | a scripted scroll of a long list with timings; only with `UI_SCROLL_BENCH`, without login |

Parameters of `GET /api/screenshot`:

- `screen=<id>` — show this page first (404 for an unknown one);
- `peek=drawer|sound|pick|standby|pair|bt18` — put an overlay up for the shot: the menu, the
  "Sound" sheet, the preset list, standby, the pairing dialog on a made-up request, the Bluetooth
  page on a made-up 1.8 satellite (together with `screen=bt`); nothing goes onto the link, and
  everything is removed after the shot;
- `tap=x,y` / `hold=x,y` — a press (80 / 900 ms) at a point of the shot through LVGL's own pointer
  device, by the same path as a finger; only with `UI_SCREENSHOT_OPEN`.

With two frame buffers the shot is first copied from the buffer on the glass under the LVGL lock
(≈ 25 ms); otherwise a moving screen comes out stitched together from two frames.

### Diagnostics

The `UiLvgl` and `UiShell` lines (`Initialized: panel 480x800, UI 800x480 (rotation 270), touch …,
theme …`, `heap before/after …`, `touch x,y`, `backlight N%`, `standby on/off`) are at INFO level,
and the panel ships at WARN; `components/modesp_services/src/log_policy.cpp` does not list these tags.
To open them without reflashing (`-u <user>:<password>` while the HTTP API's Basic auth is on; until
a reboot, and only for later events):

```
curl -u <user>:<password> -X POST http://<panel>/api/loglevel -d '{"tag":"UiLvgl","level":"info"}'
```

## Web interface (`webui/`)

### Build and delivery

- Svelte 4, rollup 3, ES modules. `bundle.js` loads every time; rarely opened pages are separate
  `chunk-<Name>.js` files that the browser fetches on first open. The names carry no hashes: the
  device serves everything with `no-store`. The styles of all components are one `bundle.css`.
- `npm run deploy` (= `rollup -c` + `scripts/deploy.js`) gzips the bundle, the chunks and the
  `en/de/pl` catalogues into `data/www/` (`*.gz`, `lang/<lang>.json.gz`), copies `index.html` and
  writes `data/www/build.txt` — a hash of every file. The static server serves `<path>.gz` with
  `Content-Encoding: gzip`. `data.bin` depends on `build.txt`, so a new chunk rebuilds the image.
  The built web is committed.
- The web lives in the `data` partition (`data.bin`), not in the application image: it is updated by
  a full `idf.py flash` or by writing `data.bin`; an application OTA does not change it.
- A tab that was open while the web was reflashed compares `/build.txt` every time the WebSocket
  opens and offers to reload (`lib/update.js`).
- `npm run dev` — `dev-server.js` on `http://localhost:5000`: a fake API over the state in
  `dev/state.json`, WebSocket, stations, EQ (presets are read from
  `components/audio_dsp/src/eq_presets.cpp`), the library, the DLNA browser, a cover. It runs
  `rollup -c -w`, which means an unminified `dist/`; deploy only through `npm run deploy`.

### Shell and routes

Routes live in the hash (`stores/router.js`): the hash does not reach the static server, and the
browser's back button and bookmarks work.

| route | page |
|---|---|
| `#/player` | Player (`pages/PlayerPage.svelte`) |
| `#/source` | Source (`SourcePage.svelte`) |
| `#/system` | Settings (`SystemPage.svelte`) |
| `#/stations[/top\|countries\|genres\|search]` | Stations: Favourites and the catalogue (`StationsPage.svelte`, `stations/*`) |
| `#/library` | card library (`LibraryPage.svelte`) |
| `#/dlna` | media servers (`DlnaPage.svelte`) |
| `#/system/sound` | Tone and volume (`system/SoundPage.svelte`) |
| `#/system/sound/eq` | equalizer (`DspPage.svelte`) |
| `#/system/speakers[/setup\|details]` | Speakers, the wizard, details |
| `#/system/xover`, `display`, `tuning` | Crossover, Screen, Fine tuning |
| `#/system/wifi`, `bt`, `time`, `access`, `firmware`, `backup`, `language` | Settings pages |
| `#/system/service[/calibration[/port]\|screenshot\|log\|hardware\|more]` | Service and its tools |
| `#/components` | all of the web's M3 components; not in the menu |

The `#/dsp/…` and `#/service/…` addresses (bookmarks) are rewritten to `#/system/sound/eq/…` and
`#/system/service/…` without a history step (`lib/routes.js`). The web's own navigation history makes
← go back to where one came from; on a cold link, to the parent page. An open bottom sheet
(`m3/BottomSheet.svelte`) takes a history step with the same address, so the phone's back button
closes the sheet, not the page under it; closed any other way (Esc, the scrim, ×), the sheet gives
that step back.

`shell/Shell.svelte`: the top bar (← on a non-section page, title, Wi-Fi), three sections — a bottom
bar on a phone, a rail from 600 px (`lib/nav.js`, the same icons as in the panel's menu), the
mini-player above them, a test-tone bar while the tone plays (`ToneBar.svelte`), a banner after 5 s
without a WebSocket (connection lost) or when the panel is busy. The mini-player is on every page
except the Player when the source is playing (tone and AUX — always) or can be resumed from the
mini-player: card, radio, DLNA, Bluetooth with a phone, AirPlay with a remote
(`np.playing || np.toggle && np.live`, `stores/now.js`). Output off, Bluetooth without a phone, USB
without a stream and AirPlay without a remote that is not playing do not have it. The Player has no
top bar and is tinted with `ui.cover_rgb`. Above everything: login (`LoginModal`), the snackbar and
the pairing dialog (`PairDialog.svelte`).

### Pages and what they read/write

| page | state | own endpoints |
|---|---|---|
| Player | `stores/now.js` (the source's face, as in `screen_now.cpp`), `audio.volume`, `audio.mute`, `audio.leveler`, transport `*.cmd`, `player.repeat/shuffle`, `*.seek_s`, `radio.fav_add` | `GET /api/cover?px=280` following `ui.cover` |
| Source | `audio.sources`, `audio.source` (the same cards and order as on the panel) | — |
| Settings | `airplay.enabled/mode`, `dlna.enabled`, `radio.covers`, captions | `POST /api/restart` |
| Tone and volume | `audio.bass_db`, `treble_db`, `loudness`, `eq_enabled`, `vol_max`, `vol_start`; "Volume leveling": `lev_sched` and, while it is on, `lev_from`/`lev_to` as time fields (5-minute step) | `/api/eq` (presets, `lib/eq.js`) |
| Equalizer | `audio.eq_enabled`, `audio.eq_rev` | `GET/POST /api/eq` |
| Speakers, wizard, calibration | `audio.spk_*`, `audio.port_*`, `audio.balance_db10`, `audio.tone_*` | — |
| Screen | `ui.backlight`, `ui.standby_bl`, `ui.standby_min`, `ui.theme` | — |
| Language | `ui.lang` | — |
| Wi-Fi | `wifi.*` | `/api/wifi`, `/api/wifi/scan`, `/api/wifi/ap` |
| Bluetooth | `bt.cfg_rev`, `bt.pair_*` | `GET/POST /api/bt` |
| Stations | `radio.list_rev`, `radio.web_*` (catalogue) | `GET/POST /api/stations`; the catalogue queries Radio Browser from the browser |
| Library | `player.track` + `player.cmd`=1 | `GET /api/library` |
| Media servers | `dlna.browse_cmd` | `GET /api/dlna/browser` (0.4 s while an action is running, otherwise 4 s) |
| Time, Access, Firmware, Backup | — | `/api/time`, `/api/auth`, `/api/ota`, `/api/backup`, `/api/restore`, `/api/userdata` |
| Service | diagnostics by polling | `/api/loglevel`, `/api/bindings`, `/api/screenshot`, `/api/factory-reset` |

The endpoint formats are in [NETWORK.md](NETWORK.md); the EQ document and calibration in
[AUDIO.md](AUDIO.md).

### State: reading and writing

**Reading.**

1. Start: `GET /api/ui` (the generated `ui.json`), `GET /api/state` (the whole state).
2. WebSocket `/ws`: a full snapshot on connect, then every 1.5 s a delta of the changed keys
   (`stores/state.js` merges them into the `state` store). The server holds up to three clients; a
   fourth, while three are alive, is closed with code 1013, and the web shows that the panel is busy
   and tries every 5 s. Otherwise it reconnects with a 1 → 30 s delay. A hidden tab lets go of the
   socket and reconnects as soon as it is shown.
3. Keys that modules write without a change mark (`track_change = false`: levels, CPU and the audio
   limiter, heap, satellite link counters) do not get into the deltas. Pages that show them poll
   `GET /api/state` every 2 s while visible (`lib/live.js`); settings come from the WebSocket,
   because polling could return a stale value.

**Writing** — `POST /api/settings` with `{key: value}`. The server accepts only the keys that
`generated/state_meta.h` marks writable, clamps numbers to the manifest's `min`/`max`, and hands
`*.cmd` to the module's queue without storing it (`lib/cmd.js` sends an integer).

- `createSettingSender(key, {debounceMs, live})` (`lib/settings.js`) shows the value at once.
  For `live` (sliders) the first value goes at once, then no more often than one per `debounceMs`,
  the newest; otherwise, one after a `debounceMs` pause. There is never more than one request in
  flight. A refusal puts the device's value back on the page; `cleanup()` on leaving the page sends
  the delayed one.
- `writeNow(keys)` — a write without delay for actions that close the page; on a refusal the page
  shows the previous value and gets an error.

**Documents with a revision** have endpoints of their own: EQ (`/api/eq`, an edit carries the
revision, a stale one gets 409 and the page rereads; `audio.eq_rev` says that the panel changed the
document), stations (`/api/stations`, generation, 409; `radio.list_rev`), Bluetooth settings
(`/api/bt`; `bt.cfg_rev`), the DLNA browser (a snapshot with a revision).

**401** opens the login (except for a background image request, `apiBlob(…, {ask: false})`); the
credentials are Basic, in the tab's `sessionStorage`, encoded from UTF-8 (`lib/api.js`).

**Errors**: the snackbar states them in words, in the page's language (`toastError` in
`stores/toast.js` → `humanError` in `lib/errors.js`; an `Error` from `lib/api.js` keeps the raw
response for pages that look for a code of their own):

- the browser's message about a dropped connection (`Failed to fetch` and its Firefox and Safari
  equivalents, the upload's `Connection error`) → "Cannot reach the panel" (`app.error`);
- the `lib/api.js` status words (`Bad request`, `Invalid value`, `Not found`, `Too large`,
  `Server error`) and the panel's codes `{"error": code}` (`busy`, `LVGL busy`, `no_memory`,
  `too_large`, `size`, `bad_request`, `write`) → `err.*` and `common.no_memory`;
- any other code, or an HTTP status without a word → "The panel answered: {code}" (`err.code`),
  503 → "The panel is busy — try again in a moment" (`err.busy`);
- a text the page has already put into words — shown as is.

**The long tail.** The generated `ui.json` pages are not shown as such. `lib/longtail.js` keeps three
lists: the keys a hand-written page shows (`SHOWN`), the widgets replaced by an endpoint of their own
(`VIA`), and the ones deliberately left out (`DROPPED`). The remaining widgets are "Other settings" in
Service (`service/MorePage.svelte` through `DynamicPage.svelte`), so a module's new key with a widget
shows up there by itself. The tail is drawn by the generic widgets of
`components/WidgetRenderer.svelte`: `value`, `slider`, `number_input`, `indicator`, `status_text`,
`toggle`, `text_input`, `password_input`, `button`, `select`, `mqtt_save`; an unknown widget is
shown as `value`. `tools/tests/test_webui_service.py` keeps the lists in agreement with the pages.

### Scheme and language on the web

- The scheme follows the panel: `ui.theme` sets `data-scheme` on `<html>`, the CSS variables are
  `styles/m3_schemes.css`, generated by `tools/ui/web_tokens.py` from `ui_m3.h`/`ui_m3.cpp`
  (`--check`; test `tools/tests/test_webui_tokens.py`). The last scheme is cached in
  `localStorage`. The colour of the phone browser's bar is `--surface`.
- Language: the browser's own choice, if there is one; otherwise the panel language `ui.lang` (the
  last one seen is kept in `localStorage`); without it, the browser's language (`lib/lang.js`). The
  Language page has two settings: the panel language (`ui.lang`: the screen speaks it, and so does
  every browser without a choice of its own) and this browser's language.
- Gestures (`lib/gesture.js`): a slider is taken after 8 px along (more than across), a tap changes
  nothing, a mouse takes it at once; an EQ point moves after 300 ms or 8 px; "Undo" for 5 s
  (`lib/undo.js`). Test `tools/tests/test_webui_gesture.py`.
- Texts are in the system's `Roboto, system-ui, …` font; the web carries no text font file.
- The title and the subtitle of an `m3/Row` take up to two lines each, then "…". Below 420 px the
  `m3/Tabs` tabs are 14 px with 10 px padding (the five Stations tabs on one line), and the top bar's
  title is 20 px.

## Strings and translations

### Catalogues

`webui/src/i18n/{uk,en,de,pl}.json` — flat keys, the same set in each; this is the single source of
translations for both the web and the panel. Ukrainian is the base: it is in `bundle.js`, it lies
under every language, and a key without a translation shows in Ukrainian. The web fetches the other
three as `/lang/<lang>.json`. In the web's code a key passed to a component is wrapped in `K()`, so
that the test sees it used.

`tools/tests/test_webui_i18n.py`: one key set, no empty strings, the same placeholders, every used
key exists, every key is used (by the web or the panel), the icons are in the font, every firmware
message is in all four.

Separately, `tools/generate_ui.py` builds the packs `data/www/i18n/<lang>.json` from
`data/i18n/system_<lang>.json` for the captions of the generated pages (the long tail, `stores/ui.js`).

### Panel tables

`tools/i18n/gen_panel_strings.py` takes the keys that the `modules/ui_lvgl` code names (a dot is
written `_`) and writes:

- `modules/ui_lvgl/include/i18n_keys.h` — `enum class S` (a string), `SU` (the same string in capital
  letters, made by the generator with `str.upper()`), `P` (plurals: `.one/.few/.many/.other`);
- `modules/ui_lvgl/src/i18n_tables.cpp` — for each language one `char` array of NUL-separated
  strings and `uint16_t` offsets: all `const`, in flash; in RAM only the byte of the current language.

The generator replaces an empty or missing translation with the Ukrainian one. Both files are
committed; `--check` returns 1 if they are stale, and `tools/tests/test_panel_i18n.py` fails.

```
python tools/i18n/gen_panel_strings.py           # rewrite
python tools/i18n/gen_panel_strings.py --check
```

Runtime (`include/i18n.h`, `src/i18n.cpp`, without IDF and LVGL — test `tests/host/test_i18n.cpp`):

- `tr(S|SU)`, `plural(P, n)` — the form by the CLDR rules: uk and pl — one/few/many, en and de —
  one/other;
- `fmt(out, cap, pattern, {{"name", value}})` — substitutes `{name}`, truncates at a UTF-8 character
  boundary; a placeholder without a value stays visible;
- `num()` and `decimal_sep()` — a comma in uk/de/pl, a point in en; the minus is "−" (U+2212);
- `badge()` — a technical line (codec badge, USB format: `44.1 kHz 2 ch 128 kbps`, `16/44.1 kHz`,
  `48.0 kHz 24 bit`) in the language's units and decimal separator: "44.1 kHz 128 kbit/s"; `2 ch` is
  not written, a whole number loses its ".0", a word without a number stays as it is;
- `lang_name()` — the name of the language in that language.

Units are catalogue strings too (`unit.hz`, `unit.khz`, `unit.db`, `unit.dbfs`, `unit.kbps`,
`unit.bit`, `unit.ch`, `unit.ms`, `unit.kb`, `unit.mb`, …): Ukrainian writes "Гц", "кГц", "дБ",
"кбіт/с", the other languages use Latin letters. The web has the same rules in `lib/numbers.js`:
`decimal()` (decimal separator and minus, as `num()`; a value that rounds to zero goes without a
minus) and `badge()` (as `i18n::badge`; codec badge, USB line, tone frequency).

### Module messages

A status text that both the panel and the web show is published by the module not as a string but as
a message: `"@<key>"`, followed by `name=value` arguments, each after U+001F (`I18N_MSG_ARG` /
`MSG_ARG`):

```cpp
state_set("audio.port_meas_status", "@cal.m_done" MSG_ARG "hz=63");
```

The generator finds such literals in `modules/airplay`, `modules/audio`,
`components/dlna_client`, `modules/ui_lvgl`, checks the key and the argument names against the
catalogue and gives the panel a sorted table. The panel renders with `i18n::render()`, the web with
`msg()` (`lib/msg.js`). A string that is not a known catalogue key is shown as is.

### Width check

The rule of `tools/i18n/fit.py`: a translation no wider than its Ukrainian fits wherever the
Ukrainian fits. The wider ones are listed in `modules/ui_lvgl/i18n_fit.json` with their width in
pixels: `checked` — seen on the glass and fits, `unchecked` — wider, not yet checked. The width is
computed by `tools/i18n/font_metrics.py` from the advance and kerning tables of the LVGL fonts
themselves (reference `ui_font_400_20`, tolerance 0.5 px). The test fails if a wider string is not
recorded, if it has become wider than recorded, or if an entry remains for a string the panel no
longer uses.

```
python tools/i18n/fit.py                    # report
python tools/i18n/fit.py --record           # new wider ones — into unchecked
python tools/i18n/fit.py --accept @seen.txt # seen ones (label:lang per line) — into checked
```

The check on the glass is `GET /api/i18n/overflow` (`src/ui_fit.cpp`): a walk of the whole LVGL tree
in the current language, hidden pages and dialogs included. Types: `cut` (truncated with "…"),
`lines` (more lines than the height holds), `word` (a word wider than the label), `outside` (goes
outside the parent), `overlap` (runs into a control or another text). `?all=1` adds every label that
fits (`ok`) — that is also the list for `--accept`. Texts that a page sets in `refresh` are visible
only for the open page, in its current state.

`test_panel_i18n.py` also checks that every character of the panel strings is in every text font, and
that the characters of the catalogue's country and genre names are in `ui_font_400_20`.

## Fonts and icons

### Panel

`tools/fonts/build_fonts.py` writes `modules/ui_lvgl/src/fonts/*.c` and `ui_fonts.h`; the output is
committed, so the firmware build needs no node, pip or network. The module's `CMakeLists.txt` picks
up `fonts/*.c` by glob.

Text is Roboto (`@fontsource/roboto`, SIL OFL 1.1), one face per `ui_m3.h` role, 4 bpp:

| file | weight/px | role |
|---|---|---|
| `ui_font_500_16` | 500/16 | label_s |
| `ui_font_500_18` | 500/18 | label_m |
| `ui_font_400_18` | 400/18 | body_m |
| `ui_font_400_20` | 400/20 | body_l, title_s |
| `ui_font_400_24` | 400/24 | title_m |
| `ui_font_500_32` | 500/32 | headline |
| `ui_font_600_48` | 600/48 | display (track title) |
| `ui_font_300_160` | 300/160 | clock — only space, `-`, digits, `:` |

Coverage of the text faces: ASCII, Latin-1, selected punctuation (quotes, dashes, `…`, `›`, `€`,
`™`, minus), Latin Extended-A, Romanian `ș ț`, `†`, Cyrillic 0x400–0x45F, `Ґ ґ`, `№`. There is no
Greek, Vietnamese, CJK, emoji, Arabic or Hebrew — such characters will be boxes on the glass.

Icons are Material Symbols Rounded (Apache 2.0). The variable font is not in the repository: the
script downloads it into `tools/fonts/build/` by the URL and SHA-256 in `tools/fonts/symbols.lock`
(a different hash is a refusal), pins the FILL axis at 0 and 1 with fontTools and converts with
`lv_font_conv`. The `ICONS` table maps a `UI_SYM_*` name to a Material name and is resolved through
the font's `.codepoints`, so a name that does not exist fails at build time. Sizes: 20, 24, 28,
40 px outlined; 24, 28, 40 px filled for the `FILLED` set (transport, volume, sources, active menu
items). For glyphs that Material does not have there is the `OWN_ICONS` mechanism (SVG from
`tools/fonts/glyphs/<name>.svg` through `build_glyphs.js`, U+F800–F8FF); the table is empty, so there
is no `glyphs/` folder.

```
cd tools/fonts && npm install                  # @fontsource/roboto, lv_font_conv, svg2ttf
python tools/fonts/build_fonts.py              # everything
python tools/fonts/build_fonts.py --only text  # text only: no icon download, ui_fonts.h unchanged
python tools/fonts/build_fonts.py --only icons
```

It needs Python with fontTools. A face that disappears from `FACES` is deleted from the tree.

### Web

`tools/fonts/build_web_icons.py` takes `webui/src/lib/icon_names.json` (Material names, sorted, no
duplicates) and the same pinned font, pins wght 400, GRAD 0, opsz 24, leaves FILL a live axis and
writes:

- `webui/src/lib/icon_codepoints.js` — name → code point (`m3/Icon.svelte` draws by code point,
  because a subset with ligatures would have to keep every icon);
- `webui/src/styles/icons.css` — `@font-face` "P4 Symbols" with the woff2 inlined as base64.

It needs fontTools and brotli. The output is committed; rerun after changing `icon_names.json`.

## Generator `tools/generate_ui.py`

The ModESP generator: it reads `project.json`, the module and driver manifests, `data/board.json` and
`data/bindings.json`, validates them against `tools/schemas/*.schema.json` and the domain rules, and
writes:

- `data/ui.json` — pages from the manifests' `ui` sections and `tools/system_pages.json`, plus
  `state_meta`; served as `GET /api/ui`;
- `generated/state_meta.h` (types, ranges, writable, persist, `STATE_COMMANDS` — against it
  `POST /api/settings` checks a write), `state_enums.h`, `mqtt_topics.h`, `display_screens.h`,
  `features_config.h`;
- `generated/module_includes.h`, `module_instances.h`, `module_register.h`, `modules.cmake`,
  `drivers.cmake`, `required_drivers.cmake`, `driver_register_all.h`, `datalogger_*.h`;
- `components/modesp_hal/Kconfig`, `main/Kconfig.boards`;
- `data/www/i18n/<lang>.json` and `manifest.json` — the caption packs of the generated pages.

It runs from the root `CMakeLists.txt` on every configure; a change to a manifest, `project.json`,
the schemas, `system_pages.json` or `data/i18n/*.json` reruns configure. This is an upstream file;
local changes are tagged `# player-fork:`. A state key without the `<module>.` prefix is a warning,
one longer than 32 characters an error (SharedState keys are `etl::string<32>`).

## Bench test tools

- `GET /api/screenshot` with `screen=`, `peek=`, `tap=`/`hold=` — a walk through the shell without a
  hand on the panel; on the web, Service › Screenshot (without `screen=`, because that switches the
  panel's page).
- `GET /api/i18n/overflow?all=1` together with `tools/i18n/fit.py --accept`.
- `GET /api/ui/frames?ms=5000&plain=1` — the Player's frame time without the cover gradient against
  with it.
- `GET /api/ui/scrollbench` (`UI_SCROLL_BENCH`) — 600 synthetic rows of a real list at a fixed
  speed; the frame is broken down into render, flush (PPA), the DMA2D wait and row recycling.
- `UI_M3_TEST_SCREEN` (the `m3test` page) and `#/components` on the web — every component of the
  design system in the current scheme.
