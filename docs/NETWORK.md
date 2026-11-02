**English** · [Українська](NETWORK.uk.md)

# Network

Wi-Fi through the ESP32-C6, the HTTP server with REST API and WebSocket, device name and mDNS,
internet radio, the DLNA renderer and media server browser, the AirPlay receiver, MQTT.

Related: the values of `audio.source` and the SharedState contract — [CONCEPT.md](CONCEPT.md), §7;
board constraints and DMA memory — [CONCEPT.md](CONCEPT.md), §8, §8a and §10; the PCM ring, rates,
the rate converter — [AUDIO.md](AUDIO.md); panel and web pages — [UI.md](UI.md); C6 firmware
update — [C6_UPDATE.md](C6_UPDATE.md).

| Part | Code |
|---|---|
| Wi-Fi, HTTP, WebSocket, mDNS | `components/modesp_net` |
| ESP-Hosted, frame copies in PSRAM | `espressif/esp_hosted`, `espressif/esp_wifi_remote`, `components/hosted_mem` |
| DNS for SNTP | `components/lwip_dns_compat` |
| Radio: stream, list, catalog, covers | `modules/radio`, `components/stations`, `components/radio_catalog`, `components/audio_pipe` |
| DLNA renderer | `modules/dlna`, `components/esp_dlna`, `espressif/esp_media_protocols` |
| Media server browser | `components/dlna_client` |
| AirPlay | `modules/airplay`, `components/airplay2` |
| MQTT | `components/modesp_mqtt` |
| Device name | `modules/bt/src/device_name.cpp` |
| User files from the web | `main/userdata_http.cpp` |

Networking is enabled by `CONFIG_MODESP_NET_ENABLE` (default `y`); without it `modesp_net` builds
with no source files, and main.cpp does not bring up Wi-Fi, HTTP or WebSocket.

## Wi-Fi through the ESP32-C6

### Link

The ESP32-P4 has no radio. Wi-Fi is on the ESP32-C6, connected over SDIO; on the P4,
`esp_wifi_remote` on top of `esp_hosted` provides the ordinary `esp_wifi_*` API.

| Parameter | Value (`boards/p4_player/sdkconfig.board`) |
|---|---|
| Bus | SDIO slot 1, 4 bits, 40 MHz, RX streaming mode |
| P4 pins | CLK 18, CMD 19, D0–D3 14–17 |
| C6 reset | GPIO54 → CHIP_PU, active LOW (`CONFIG_ESP_HOSTED_SDIO_RESET_ACTIVE_LOW`) |
| ESP-Hosted tasks | priority 21 (`CONFIG_ESP_HOSTED_HOST_DEFLT_TASK_PRIORITY`): above lwIP (18) and events (20), below the audio tasks (22) |
| Versions | `esp_hosted` 3.0.7, `esp_wifi_remote` 1.6.4, both `==` |

The C6 firmware is built from the same ESP-Hosted release as the host library; the image sits in
the `slave_fw` partition (0x630000, 2 MB), and `tools/c6_updater` transfers it to the C6. The log
shows a version mismatch as `eh_init_evt: major version mismatch — OTA coprocessor from host`
(ERROR) or `minor version mismatch` (WARN). The procedure and recovery over the C6's UART —
[C6_UPDATE.md](C6_UPDATE.md). The version is recorded in several places that change together:

- `components/modesp_net/idf_component.yml`, `tools/c6_updater/main/idf_component.yml`,
  `tools/sdio_soak/main/idf_component.yml` — both packages;
- the `C6 3.0.7` caption in the "Service" row of the panel's settings — a constant in
  `modules/ui_lvgl/src/screen_set.cpp`, not a version read from the C6;
- the default of `--hosted` in `tools/c6_image.py`;
- `components/hosted_mem/CMakeLists.txt` checks the prototype of `eh_host_port_dma_alloc()` in the
  ESP-Hosted sources and stops the build when it has changed.

### Memory

Internal DMA-capable SRAM is the scarcest resource: it is shared by the ESP-Hosted receive pool,
the I2S descriptors and everything that asks for DMA ([CONCEPT.md](CONCEPT.md), §8).

- `components/hosted_mem` (`CONFIG_HOSTED_MEM_FRAMES_IN_PSRAM`, default `y`) wraps
  `eh_host_port_dma_alloc()` (`-Wl,--wrap`): ESP-Hosted allocates the copies of frames that cross
  SDIO in PSRAM. Without it, a frame for which no internal DMA memory was found is lost silently
  (on receive) or as `ESP_ERR_NO_MEM` to lwIP (on transmit). Counters —
  `hosted_mem_get_stats()`.
- `CONFIG_EH_HOST_PORT_DMA_PREFER_SPIRAM`: the ESP-Hosted transport buffers are in PSRAM.
- `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP`, `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=1024`: lwIP buffers
  and ordinary allocations over 1 KB (codecs, HTTP) are in PSRAM.
- `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`: the mbedTLS arena is in PSRAM. TLS input buffer 16 KB
  (`MBEDTLS_SSL_IN_CONTENT_LEN`): station servers send 16 KB records and ignore
  `max_fragment_length`, and a record that does not fit in the buffer cannot be decrypted. Output
  buffer — 4 KB.
- The task stacks of the radio, DLNA, the media server browser and covers are in PSRAM. An internal
  stack belongs to the tasks that write to flash (`ap_start`: HAP keys in NVS) and to the SSDP
  task: a task with its stack in PSRAM cannot run while the flash cache is disabled.
- The HTTP routes' buffers of a kilobyte and more are in PSRAM, allocated at a route's first
  request and kept (`http_routes::scratch()`), and so are the two 80 KB buffers the state is
  written to as JSON, one for the httpd task and one for the main task
  (`modesp/net/state_json.h`). mDNS keeps its records in PSRAM
  (`CONFIG_MDNS_MEMORY_ALLOC_SPIRAM`).

### lwIP

| Parameter | Value | Reason |
|---|---|---|
| `LWIP_TCP_WND_DEFAULT` | 11 520 (8 × MSS) | a connection carries at most one window per RTT, and a smaller window cannot pull 320 kbit/s from a server ~200 ms away; a window of 10 × MSS or more already takes DMA memory the receive pool needs |
| `LWIP_TCP_RECVMBOX_SIZE` | 10 | the segments of one window and two more |
| `LWIP_UDP_RECVMBOX_SIZE`, `LWIP_SO_RCVBUF` | 64, `y` | AirPlay streams: realtime ALAC — 125 packets/s |
| `LWIP_MAX_SOCKETS` | 36 | HTTP (10 sessions + httpd's 3 own sockets), SSDP, MQTT, radio with next-track preparation, covers, catalog, DLNA browser, NOTIFY, OTA, AirPlay session (up to 10 sockets) |
| `LWIP_MAX_ACTIVE_TCP` | 32 | a hard limit of ESP-lwIP: `socket()` for a TCP client fails when it is exhausted |
| `LWIP_DNS_MAX_HOST_IP` | 4 | radio moves to another address of the station's host |
| `LWIP_TCPIP_TASK_AFFINITY_CPU0` | `y` | core 1 is for the audio tasks |

`components/lwip_dns_compat`: with several addresses per name, lwIP on a cache hit copies them all
into the caller's buffer, while lwIP's SNTP client provides room for one. A wrapper of
`dns_gethostbyname()` (`-Wl,--wrap`) resolves into an array and hands back the first address.

### WiFiService

`components/modesp_net/src/wifi_service.cpp`. With `ssid` in the `wifi` NVS namespace — STA mode;
without it — access point.

**STA.**

- Security threshold — WPA2-PSK when there is a password; without a password — open network (with a
  WPA2 threshold an open network cannot be joined). The panel and the web join `open`, `WPA2`,
  `WPA/WPA2`, `WPA3`, `WPA2/WPA3`.
- Country UA: channels 1–13, up to 20 dBm, after `esp_wifi_start()`.
- `esp_netif_set_default_netif()` on STA (`CONFIG_MODESP_NET_SET_DEFAULT_NETIF`, default for P4):
  without it, unbound UDP sockets (SNTP) do not go out through the ESP-Hosted interface.
- Disconnect: three retries after 2, 4 and 8 s; another 16 s without an address — access point.
- No restart for lost Wi-Fi time (`CONFIG_MODESP_WIFI_RESTART_ON_LONG_DISCONNECT=n`; the
  framework's default restarts after 10 min without a connection in total). A router that is off
  sends the panel to its access point, and the panel goes back when the router returns. A C6 that
  stops answering on SDIO restarts the panel through ESP-Hosted
  (`CONFIG_ESP_HOSTED_HOST_TRANSPORT_RESTART_ON_FAILURE`), and the restart resets the C6
  (GPIO 54).

**Access point.** SSID `ModESP-XXXX` (the last two bytes of the base MAC) or `wifi/ap_ssid`;
password `wifi/ap_pass` of 8–63 characters → WPA2 (a longer stored one is cut to its first 63,
with an ERROR in the log), otherwise open; channel `wifi/ap_chan` (1–13,
default 1); up to 2 clients; address 192.168.4.1. With a saved network, the AP tries STA (APSTA,
15 s per attempt): the first attempt after 30 s, then the interval doubles up to 5 min; an attempt
is skipped when free internal RAM is < 50 KB. A successful attempt switches to pure STA.

**Joining** from the panel and the web — `WiFiService::try_connect()`: the old pair stays in
memory, the new one is written to NVS only after an address is obtained. A refusal by the network
(`AUTH_FAIL`, `AUTH_EXPIRE`, handshake timeout, no AP with compatible security or within the
threshold) ends the attempt at once; 25 s without an address or three failed retries — the old pair
comes back, and when there was none — the access point.

**Scanning.** Active, 120–500 ms per channel, no hidden networks, up to 20 results (records in
PSRAM); needs ≥ 50 000 B of free internal RAM; in AP mode — APSTA for the duration of the scan.
Scanning blocks its caller: for `GET /api/wifi/scan`, the HTTP server task. One scan at a time,
and none during a join or an AP → STA attempt: such a request gets no list (`{"ok":false}` on the
web, a failed scan on the panel), while a join or an attempt that comes during a scan waits for it.

**RSSI is not read.** `esp_wifi_sta_get_ap_info()` here is a synchronous RPC to the C6 with a 5 s
timeout, while the main loop feeds a 5-second watchdog; besides, ESP-Hosted 3.0.7 returns rssi 0
in it. `wifi.rssi` is not written; the networks' signal levels come from scanning.

**Modem sleep:** `WIFI_PS_NONE` while the radio plays a station or a URL (from tuning to stop or to
the radio task stopping), `WIFI_PS_MIN_MODEM` otherwise (`modules/radio`, `set_modem_sleep()`):
with the modem asleep, the C6 link loses packets under a stream.

| Key | Value |
|---|---|
| `wifi.connected` | there is an address in STA |
| `wifi.mode` | `sta` / `ap` |
| `wifi.ssid` | the STA network or the access point SSID |
| `wifi.ip` | address; in AP mode — 192.168.4.1 |
| `wifi.reason` | reason code of the last disconnect (`wifi_err_reason_t`) |
| `wifi.try`, `wifi.try_ssid` | join attempt: `connecting` / `ok` / `fail`, empty — none |
| `system.mdns_hostname` | mDNS name |

NVS namespace `wifi`: `ssid`, `pass`, `ap_ssid`, `ap_pass`, `ap_chan`.

### Time

SNTP, `pool.ntp.org`, poll mode. NVS `time/ntp_enabled` (default true), `time/timezone` (POSIX TZ,
default `EET-2EEST,M3.5.0/3,M10.5.0/4`). Manual time and zone — `POST /api/time`. Until
synchronization `system.time` and `system.date` show `--:--:--` and `--.--.----`.

## Device name and mDNS

**The device name** is shared by Bluetooth, AirPlay, DLNA, USB audio and the header of the panel's
drawer (`modules/bt/include/device_name.h`):

- NVS `devname/name`, 1–32 bytes of UTF-8 — the satellite's rule (`BT_CONFIG_SET`,
  [LINK_PROTOCOL.md](LINK_PROTOCOL.md)); while there is no name — `P4 Player` (`device_name::FALLBACK`,
  the same as `system.device_name` in `project.json`). A stored name that breaks the rule counts as
  none.
- Set by the "Name" field on the Bluetooth page of the panel or the web (`POST /api/bt`,
  `{"name":…}`, a name that is not UTF-8 gets `400`): the name goes to the satellite and is saved in
  NVS. Needs a satellite that exposes its settings (`DEV_CAP_BT_SETTINGS`); without one
  `POST /api/bt` answers `409`.
- The first name the satellite reports is saved when the panel has none of its own yet; to a
  satellite with a different name the panel passes its own, once per link: a satellite that still
  reports its own after that keeps it.
- Bluetooth takes a new name at once; AirPlay, DLNA and USB audio read it at startup — after a
  restart.
- DLNA announces it as friendlyName (`dlna.name`), AirPlay as the name in the list (`airplay.name`)
  and the RAOP service `<MAC>@<name>`.

**mDNS** (`espressif/mdns`): hostname `modesp-xxxx` (the last two bytes of the MAC, lowercase hex) —
not the device name; instance `ModESP Controller`; service `_http._tcp`, port 80, TXT `path=/`.
Starts after an address is obtained in STA, and in AP mode; on an AP → STA transition it restarts,
and AirPlay announces its services again. mDNS initialization and teardown and service
registration are serialized by `MdnsGuard` (`components/modesp_net/src/mdns_guard.cpp`): the Wi-Fi
event task rebuilds mDNS in parallel with the main loop.

## HTTP server

`HttpService` (`components/modesp_net/src/http_service.cpp`) over `esp_http_server`.

| Parameter | Value |
|---|---|
| Port | 80 |
| Sessions | `CONFIG_MODESP_HTTPD_MAX_OPEN_SOCKETS` = 10 (`sdkconfig.board`), WebSocket and HTTP together; with httpd's 3 own sockets they must fit in `LWIP_MAX_SOCKETS` — otherwise the build stops (`static_assert`) |
| Handlers | `max_uri_handlers` = 88 |
| Task | one, 8 KB stack; handlers run one at a time |
| Timeouts | receive 30 s, send 10 s |
| All sessions busy | `lru_purge_enable`: a new session closes the one not heard from the longest |

10 sessions are three WebSocket clients, up to six browser connections per host and one for a
control point or curl: a WebSocket that only listens is heard once every 5 s (PONG), so when
sessions run short, a page load closes a live WebSocket.

One task for the whole server: a slow handler (Wi-Fi scan, OTA, a large POST) holds up both HTTP
and the WebSocket broadcast. Handler buffers over a few kilobytes are in PSRAM or static, not on the
8 KB stack.

### Static files

The wildcard `/*` serves `/data/www/` from the `data` partition — the web bundle sits in
`data.bin`, not in the app image. `<path>.gz` takes precedence (`Content-Encoding: gzip`),
otherwise the file as is; an unknown path — `index.html` (SPA routes). A URI longer than 51
characters — 404, `..` in the path — 400. `/radio.html` redirects to `/#/stations/top`.

Every file from the partition (`send_file()`, `GET /api/ui` too) goes with `Cache-Control:
no-cache` and an `ETag`, the CRC-32 of the file, worked out at its first request after boot (up to
64 files). A request with that tag in `If-None-Match` gets `304` with no body, so a reload costs a
round trip per file, and a flash of the web reaches an open tab at its next load. Files go out in
4 KB pieces from one PSRAM buffer.

### Route order

httpd returns the first pattern that matches, and checks for duplicates at registration with the
same matcher (`httpd_uri_match_wildcard`): while `/*` is registered, every new GET registration is
rejected as `already registered`. Hence the order:

1. `HttpService::on_init()` — the framework's `/api/*` routes;
2. main.cpp — `/ws`, `/api/mqtt`, then `modesp::http_routes::run_all()`: the routes of the modules,
   which they recorded through `http_routes::add()` in `on_init()` (up to 8 registrars,
   `modesp/net/http_routes.h`);
3. last — `/*`.

A subsystem that registers later (the DLNA renderer once the network is up) removes the wildcard
(`http_routes::wildcard(false)`) and puts it back. In between, the server's task is held between two
requests: httpd changes its handler table with no lock against its own lookups, so a request that
comes meanwhile waits for the routes to be whole rather than getting a 404 or a half-built slot.
If `/*` did not fit in the handler table, no page opens, but the API answers; log
`HTTP: Static file handler NOT registered`.

### Authentication

HTTP Basic, realm `ModESP`. NVS namespace `auth`: `user`, `pass`, `enabled`; default `admin` /
`modesp`, enabled. After 5 failed attempts in a row, every checked request gets `429` for 30 s
(one counter per device). Module routes check the same login through
`http_routes::authorized()`; with no check configured they answer `403`.

| Without login | With login |
|---|---|
| static files; `/ws`; `GET /api/ui`, `GET /api/auth`, `GET /api/wifi/ap`, `GET /api/ota`, `GET /api/time`; UPnP routes | the rest of `/api/*` |

- `/ws` gives out the whole state without login. The UPnP routes need no login because control
  points do not send Basic auth.
- Development builds: `CONFIG_UI_SCREENSHOT_OPEN` (default `n`) removes the login from
  `/api/screenshot`, `/api/ui/frames`, `/api/i18n/overflow`; `CONFIG_UI_SCROLL_BENCH` (default `n`)
  adds `/api/ui/scrollbench` without login. Both log a WARN about themselves at every boot.
- CORS: there is no `Access-Control-Allow-Origin` header and no `OPTIONS` route — a page from
  another origin cannot read a response, and a preflight it needs for a JSON POST fails.
- The web keeps the credentials in the tab's `sessionStorage` and asks for them on `401`.

`POST /api/auth`: `{"username","enabled","current_pass","new_pass","reset"}`. `new_pass` (4–63
bytes) and `reset` (back to `admin` / `modesp`, enabled) require `current_pass`, otherwise
`403 {"error":"wrong_password"}`; `username` — 1–31 bytes without `:`. Everything is validated
before writing.

### REST API

For every route:
- A `POST` must carry `Content-Type: application/json` (`application/octet-stream` for the OTA
  upload); anything else, or none, gets `415 {"error":"content_type"}`. A page on another site can
  make a browser send only a form's content types without asking first, and these two make it ask
  with a preflight, which the server never grants. UPnP control URLs are not under `/api/` and
  are not affected. With curl: `-H 'Content-Type: application/json'`.
- A body that does not fit the route's buffer is refused with `413` before it is read; a body is
  read whole, however many TCP segments it comes in. Its length must come as `Content-Length`:
  httpd does not read a chunked body (`Transfer-Encoding: chunked`), and such a POST arrives empty
  — `400` (dart:io sends chunked unless the length is set).
- A `GET` of an `/api/` path nothing serves gets `404 {"error":"not_found"}` (`application/json`);
  the web's own paths still get `index.html`. Another method on such a path gets httpd's `405`:
  the static files' wildcard takes every `GET` path, so httpd counts the path as known.

Framework routes (`http_service.cpp`):

| Route | What |
|---|---|
| `GET /api/state` | the whole SharedState as one JSON object (80 KB buffer in PSRAM; does not fit — 500) |
| `POST /api/settings` | `{key: value,…}`: only known writable keys from `generated/state_meta.h`, each with a value of its manifest type (a string, a number, `true`/`false`) — anything else is refused, and the request gets `400` when nothing in it was taken; a number outside the manifest's range is clamped with a WARN in the log; a command key (`"command": true`) goes to the owning module's queue, not to the store. Body ≤ 1023 B, up to 15 pairs |
| `GET /api/ui` | `ui.json` gzip-encoded: the build packs `data/ui.json.gz` beside it (~12 KB for ~86 KB); `ETag`, `304` |
| `GET`/`POST /api/bindings` | the board's bindings |
| `GET /api/board`, `GET`/`POST /api/devices`, `GET /api/modules` | ModESP board, device registry and modules — only with `CONFIG_MODESP_HTTP_FRAMEWORK_ROUTES` (`n` on the player) |
| `POST /api/wifi` | `{"ssid","password"}`, SSID 1–32 bytes, password ≤ 64 (otherwise `400`) — save only (takes effect after a restart); with `"join":true` — `try_connect()` (`409 {"error":"busy"}` during another attempt); `{"forget":true}` — forget the network, access point. Body ≤ 255 B |
| `GET /api/wifi/scan` | `{"ok","networks":[{"ssid","rssi","auth"}]}` |
| `GET`/`POST /api/wifi/ap` | `{"ssid","channel","password_set"}` / `{"ssid","password","channel"}`: SSID 1–32 bytes, password empty or 8–63; takes effect after a restart |
| `GET`/`POST /api/time` | `{"time","date","ntp_enabled","timezone","synced","unix"}` / `{"mode":"ntp"\|"manual","time":<unix>,"timezone"}` |
| `GET`/`POST /api/auth` | `{"enabled","username"}` / see "Authentication" |
| `GET`/`POST /api/loglevel` | log levels: `{"tag","level"}`, `level` — `none`, `error`, `warn` or `info`, not above the build's ceiling (INFO); not persisted |
| `GET /api/log`, `GET /api/log/summary` | ModESP journal history — only with `CONFIG_MODESP_HTTP_FRAMEWORK_ROUTES`; the player has no journal source |
| `GET /api/backup`, `POST /api/restore` | persist keys (numbers and bool), `{"modesp_backup":1,"keys":{…}}`, ≤ 2 KB; a restore skips a key that is not a persist key or whose value is not of its manifest type, answers `{"ok":true,"restored":N,"skipped":M}` and restarts the panel |
| `POST /api/restart` | saving settings, stopping the modules in the main loop (≤ 2 s), restart |
| `POST /api/factory-reset` | erases NVS (Wi-Fi, login, settings, AirPlay keys) and restarts; does not touch the `userdata` partition |
| `GET`/`POST /api/ota` | see "OTA" |
| `GET /api/onewire/scan`, `GET /api/drivers/scan`, `GET /api/ble/scan` | ModESP bus scanners — only with `CONFIG_MODESP_HTTP_FRAMEWORK_ROUTES` |

Module routes (`http_routes`):

| Route | What |
|---|---|
| `GET`/`POST /api/stations` | station list — "Station list" |
| `GET /api/dlna/browser` | media server browser snapshot — "Media server browser" |
| `GET`/`POST /api/userdata` | user files (station list, EQ and "Custom", `devices.json`, `last_track`) as one JSON; POST puts them alongside as `<name>.restore`, and `UserStore` moves them into place at the next boot (the web restarts the panel right away). Body ≤ 192 KB; all files are validated before writing |
| `GET`/`POST /api/eq` | equalizer — [AUDIO.md](AUDIO.md), "PEQ and `eq.json`" |
| `GET`/`POST /api/bt` | satellite Bluetooth, device name — [AUDIO.md](AUDIO.md), "Control from the panel and the phone" |
| `GET /api/screenshot`, `/api/cover`, `/api/library`, `/api/i18n/overflow`, `/api/ui/frames` | panel routes — [UI.md](UI.md), "Module HTTP routes" |
| `GET`/`POST /api/mqtt` | MQTT — "MQTT" |
| UPnP | `/rootDesc.xml`, `/<Service>.xml` and routes under `/<Service>/` — "DLNA" |

### OTA

`POST /api/ota`, body — the raw app `.bin`.

- Size from 256 B to the size of the next OTA slot (4 MB); first byte `0xE9`; `project_name` in
  `esp_app_desc_t` matches the running image's, otherwise
  `400 {"error":"board_mismatch","running","incoming"}`.
- Three receive timeouts in a row (~90 s) — `408`: a client that vanished without FIN would
  otherwise hold the only httpd task.
- One update at a time, shared with the MQTT `_ota` command where it is built: while one is being
  written the other is refused (here `409`), and a written image keeps the slot until the restart.
- Writes to the next slot (`ota_0` / `ota_1`), makes it bootable, stops the modules,
  restarts.
- Within 60 s the new image must get Wi-Fi and the HTTP server, `audio.state` ≠ `error` and a
  running `ui_lvgl`; then `esp_ota_mark_app_valid_cancel_rollback()`, otherwise a restart, and the
  bootloader returns to the previous image (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`).
- OTA updates only the app: the web bundle, `board.json` and `bindings.json` are in the `data`
  partition.
- `GET /api/ota` (without login): `{"partition","version","idf","date","time","board"}`.

### WebSocket `/ws`

`WsService` (`components/modesp_net/src/ws_service.cpp`, slot rules — `ws_slots.h`, test
`tests/host/test_ws_slots.cpp`).

- A server message is a text JSON object `{key: value,…}` in the `/api/state` format. After the
  handshake the client receives the whole state (needs ≥ 32 000 B of free internal RAM), then
  the changed keys as soon as they change, at most one message every 200 ms.
- Keys starting with `_` are reserved for service messages and are never state keys. Every 5 s
  the server sends each client a PING and a text frame `{"_hb":n}`: a page cannot see PINGs, and
  without a message it cannot tell a quiet panel from a connection that is gone. The web drops a
  socket that has carried a heartbeat and then stays silent for 15 s, and connects again.
- Keys that modules write without a change flag (diagnostics: CPU, heap, limiter, satellite
  counters) do not get into the deltas; the web page with diagnostics, while visible, polls
  `/api/state` every 2 s (`webui/src/lib/live.js`).
- Up to 3 clients. A client not heard from for 15 s (any frame, PONG too), or one that missed a
  message, is closed. A frame to a client goes out whole within 3 s, or the session is closed: the
  httpd task serves all of HTTP and every client, and a slow phone must not hold it. A fourth
  client takes the slot of one already due to be closed, or receives CLOSE 1013 `busy`.
- Client text frames (up to 127 B) are read and discarded; commands go through
  `POST /api/settings`. A longer frame of any type closes the session.
- Sending goes through `httpd_queue_work` in the httpd task: `httpd_ws_send_frame_async()` from
  another task races with its `recv()`.
- The client is registered by `ws_post_handshake_cb`: IDF 5.5.5 does not call the `/ws` handler for
  the handshake, so `CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT=y` (`sdkconfig.defaults`) is
  mandatory — without it, `#error`.

## Internet radio

`modules/radio`. The module works only while `audio.source` = 5: buffers, the filter bank and the
tasks are created when radio becomes the source and freed when it stops being one. The catalog works
with any source, and a catalog row (from the panel or the web) sets `audio.source` = 5 itself; DLNA
sets it before `radio.play_url`. The other inputs are handled only with source 5: `radio.cmd`
commands that arrive with another source are discarded, while `radio.station` and `radio.play_url`
wait until the source becomes 5. The same engine plays stations and external URLs; `radio.mode` distinguishes
`station` and `push` ([CONCEPT.md](CONCEPT.md), §7).

### Stream

- A custom HTTP client over `esp_transport` (TCP or TLS with the certificate bundle): ICY metadata is
  interleaved with the audio every `icy-metaint` bytes, and Espressif's stack does not extract it.
- Request: `GET`, `Icy-MetaData: 1`, `User-Agent: modesp-player/1.0`; response head ≤ 4080 B;
  up to 4 redirects (`Location`); `Range` — for seeking and resuming a track.
- Read: `Content-Type`, `Content-Length`, `Content-Range`, `icy-metaint` (≤ 64 KB), `icy-br`,
  `icy-name`. `StreamTitle='…'` → `radio.now_playing`.
- Playlist (Content-Type `mpegurl` / `scpls` / `x-pls`, or a URL ending in `.m3u`, `.m3u8`, `.pls` whose
  Content-Type is not `audio/…`): the first http URL is taken. HLS is not supported.
- Decoder — by Content-Type: `mpeg`/`mp3` → MP3, `aac`/`aacp` → AAC, `flac` → FLAC (checked before
  `ogg`, because there is `audio/ogg; codecs=flac`), `ogg`/`opus` → Ogg, `wav`/`wave` → WAV,
  `mp4`/`m4a` → M4A; without a usable type — by the URL extension (`.aac`, `.wav`, `.flac`,
  `.m4a`/`.mp4`); otherwise MP3.
- Decoders in the build (`sdkconfig.board`): MP3, AAC with HE-AAC (SBR, PS: `aac_plus_enable`), FLAC,
  WAV, M4A. Ogg (Vorbis, Opus) is disabled — such streams do not play.
- The compressed stream buffer is 128 KB in PSRAM (~8 s at 128 kbit/s), playback starts at 32 KB;
  `radio.buffer` is its fill, %. PCM ring — 0.5 s at 48 kHz ([AUDIO.md](AUDIO.md), "The PCM ring
  and its owners").
- Timeouts: connection (DNS, TCP, TLS handshake) — 8 s, one read — 1 s; 8 timeouts in a row —
  the stream is lost.
- Rate: the stream asks the ring for its own rate and, if the output settled on another, converts
  ([AUDIO.md](AUDIO.md), "Output rate and sessions"). Stream start and end — a 480-frame fade.
- Tasks `radio` (decoder, 12 KB) and `radio_net` (socket reader, 6 KB): stacks in PSRAM, core 0,
  the main loop's priority with time slicing between them.
- `esp_audio_codec` 2.5.0 packs a mono AAC frame in place in the output buffer and reaches up to
  12 KB from its start, so the decoder's PCM buffer is ≥ 12 KB (`static_assert`).

**A station** reconnects with no limit on attempts: pauses of 0.5, 1, 2, 4, 8, then 15 s. Without a
network (`wifi.connected` = false) it waits without counting attempts; a station selected before
the network appeared starts as soon as the network appears.

**Slow server** (`modules/radio/include/stream_watch.h`, test `tests/host/test_stream_watch.cpp`):
two 5-second reports in a row in which the network delivers < 95 % of what the stream needs and the
buffer is < 10 % — reconnection to another address of the same host (connection by IP, the name in
`Host` and SNI), no more often than once per 35 s. Stations only.

**TLS.** A station can play over HTTPS; the catalog and track covers go over HTTP only, the logos
over HTTPS only while the station itself plays over HTTP — its address and, after a redirect or a
playlist, its connection: there is one TLS session, and while a station plays, the station holds
it. One HTTPS fetch of a logo at a time (`radio_http::get_url`).

### Station list

`components/stations` (test `tests/host/test_stations.cpp`): up to 64 stations (name ≤ 63 B, URL
≤ 223 B, Radio Browser `uuid`), 6 presets. The file is `/userdata/stations.json` through
`UserStore`, on a panel without the `userdata` partition — `/data/stations.json`; with no file or an
empty one — the built-in set.

```json
{"stations":[{"name":"…","url":"…","uuid":"…"}],"presets":[0,3,-1,-1,-1,-1]}
```

Unreadable entries are discarded, the rest remain; file ≤ 24 KB. A name is one line: a control
character in it (a tab, a line break) becomes a space and the spaces at its ends go, whichever way
it came — the web, the catalog, the file.

- `GET /api/stations` → `{"rev":N,"list":{"stations":[…],"presets":[…]}}`.
- `POST /api/stations` ← `{"rev":N,"stations":[…],"presets":[…]}`, `rev` as the first field. `202` —
  accepted, the main loop applies it; `409 {"error":"changed","rev":M}` — the list has changed since
  that generation; `400 {"error":"rev_first"|"no_stations"}` (an empty list is not accepted: at the
  next boot it would read as missing); `413`; `503 {"error":"busy"|"no_memory"}`.
- A playing station keeps playing even after it is deleted from the list.
- `radio.list_rev` — the list generation; changes with every edit from the panel, the catalog or the
  web.
- A preset written from the screen is saved to the file by `radio.save` = 1.

### Radio Browser catalog

**Panel** (`modules/radio/src/catalog.cpp`, task `radio_cat`; requests, parsing and filters —
`components/radio_catalog`, test `tests/host/test_radio_catalog.cpp`).

- HTTP only, port 80, User-Agent `P4Player/<app version>`.
- Servers — from `/json/servers`, once a day; fallback name `all.api.radio-browser.info`.
- Query `radio.cat_query`: `top:UA`, `country:DE`, `tag:jazz`, `tag:jazz@UA`, `name:kiss`,
  `name:kiss@UA`; ranking is within a country or genre, because Radio Browser's global votes are
  inflated. Order `radio.cat_sort`: 0 clicks per day, 1 votes, 2 bitrate, 3 name. Threshold
  `radio.cat_quality`: 0 all, 1 from 128, 2 from 192, 3 320 kbit/s, 4 FLAC only.
- A page is 12 rows out of 24 requested from the server: the device discards codecs other than MP3,
  AAC, AAC+ and FLAC, HLS, URLs longer than 223 B and repeats of one stream. Up to 64 pages ahead;
  a cache of 6 pages for 10 min; no more than two requests per second; body ≤ 64 KB.
- Countries — `/json/countrycodes`, once a day; genres — `/data/www/radio_genres.json` (names in uk,
  en, de, pl and the query tag).
- `radio.cat_play` = N — play a row as a station (reconnects, has a name), `radio.cat_fav` = N
  — add to the list, `radio.fav_add` — add the playing catalog station to the list. A catalog
  station outside the list: `radio.station` = −1, `radio.uuid` — its uuid.
- State — `radio.cat_state` (`idle|loading|ready|error`), `radio.cat_page`, `radio.cat_count`,
  `radio.cat_more`, `radio.cat_list`, `radio.cat_error`.

**Web** (Stations → catalog, `webui/src/lib/radiobrowser.js`): the browser itself goes to Radio
Browser over HTTPS (the API gives CORS `*`) with the same filters. Only `radio.web_name`,
`radio.web_url`, `radio.web_uuid` are passed to the panel, then `radio.web_act`: 0 play, 1 to the
list, 2–7 to preset 1–6. The URL must be `http(s)://` and ≤ 223 B. The Player stores what plays on
a preset through the same keys, and asks `/json/stations/byuuid?uuids=…` for its stations' logos
([UI.md](UI.md), "Pages and what they read/write").

**Clicks.** When a station with a `uuid` (from the list or from the catalog) has started playing, the
module makes `GET /json/url/<uuid>` once per tuning — this is how Radio Browser counts what is
popular. Turned off by `radio.report_clicks`.

### Covers and logos

`modules/radio/src/art.cpp`, task `radio_art` (16 KB, PSRAM); parsing the title, the request and
choosing the result — `components/radio_catalog/include/radio_art.h`, test
`tests/host/test_radio_art.cpp`.

- **Track cover.** ICY title `Artist - Title` (also with `–` or `—`) → Deezer search
  `http://api.deezer.com/search?q=…&limit=10`; the first result with the same title and the same
  artist is taken (directly, when one starts with the other, or through transliteration of
  Cyrillic); `album.cover_big` (500 px) over HTTP, ≤ 256 KB. `radio.covers` = false — the title is
  not sent to Deezer.
- **Station logo.** The `favicon` of the catalog row or from `/json/stations/byuuid?uuids=<uuid>`;
  first over HTTP (for an `https://` address too), over HTTPS only when the station itself plays
  over HTTP; ≤ 128 KB, JPEG or PNG.
- Tile `CoverSource::Radio`: the track cover, without it — the logo, without that — nothing, and the
  panel draws the station's stand-in ([UI.md](UI.md), "Panel pages"). A track pushed through DLNA
  shows the control point's cover (`CoverSource::Network`), without it — the Radio tile.

**Preset logos** (`modules/radio/src/logos.cpp`; the rules — `components/radio_catalog/include/
station_logo.h`, test `tests/host/test_station_logo.cpp`; fitting into the disc —
`components/cover/include/cover_thumb.h`, test `tests/host/test_cover_thumb.cpp`). The panel keeps
the logo of every preset station that has a Radio Browser `uuid`, so its preset button shows it
while an HTTPS station holds the TLS session and after a restart ([UI.md](UI.md), "Panel pages").

- **Storage**: `logos/<uuid>.565` in the `UserStore` folder (`/userdata`; on a panel without that
  partition, `/data`) — 40×40, the size of the preset's disc, drawn unscaled: an 8-byte header
  (`SLG1`, the side twice) and RGB565, 3208 B a file. Read at start and when the presets change.
  A logo that arrives or changes is on its preset at once, but files are written (through
  `safe_file`) and deleted only while nothing plays: after 4 s of silence at the output
  (`PcmRing::output_quiet` — the output gate shut and the TX queue played out, whatever the
  source), one file a 500 ms pass, since a LittleFS write stops the cache of both cores and with
  music playing the speakers click. Then the file of a station no longer on a preset goes, and so
  does a candidate an interrupted save left. A logo not yet written when the power goes is
  fetched again after the restart. Only a well-formed uuid names a file.
- **Fetching**: task `radio_logo` (12 KB, PSRAM, idle priority), one at a time, made for one batch
  and gone after it; on the main task's 500 ms pass, when the network has been up for 20 s, at
  least 30 s after the last batch, and only while the TLS session is free — the radio is not the
  source, is stopped, or plays a station over plain HTTP past its buffering (not a pushed track).
  One `/json/stations/byuuid?uuids=a,b,…` for every station without a logo, then each favicon as
  the station logo above: HTTP first, ≤ 128 KB, JPEG or PNG by the first bytes. Before each
  favicon the task looks again whether the TLS session is free; when it is not, the rest wait for
  the next batch.
- **Decoding**: `cover_thumb()` (`components/cover`) — the JPEG engine for baseline JPEG, the
  software decoder at the task's idle priority for PNG and progressive JPEG, with the limits of a
  cover. The picture is fitted whole, not cropped, and averaged down; transparent pixels and the
  margins go on a ground: the colour of the picture's edge when it has no transparency, otherwise
  a light neutral under dark ink and a dark one under light ink.
- The playing station's logo, fetched for the tile, becomes its preset logo without a second
  download.
- **Failures**: a station whose logo cannot be had — none listed, the address does not answer, the
  file is not JPEG or PNG (ICO, SVG, WebP) or does not decode — is asked for again after 6 h; when
  Radio Browser does not answer the lookup, after 10 min. The waits are not kept over a restart.
  A station without a `uuid` (typed in by hand) has no logo source and keeps its stand-in.

### External URLs: position, seek, next track

`radio.play_url` (from the web or from DLNA) plays a URL once: `radio.mode` = `push`, `radio.name`
empty, `radio.url` — the URL itself, the title is held by the sender (`dlna.title`). The URL is up
to 256 B against 223 for a station: a media server puts the item id and a session token into the
URL, and a truncated URL fails as 404.

- There is no endless reconnection: a failed first connection stops the track; a break mid-track —
  continuation from the same position through `Range`, up to 20 attempts per track (3 connections
  per episode with a 0.5 s pause).
- **Duration** `radio.dur_s` = `Content-Length` × 8 / bitrate; 0 — unknown (station). The MP3
  decoder reports the bitrate in kbit/s, so a value < 10 000 is read as kbit/s
  (`components/audio_pipe`, test `tests/host/test_stream_pos.cpp`).
- **Position** `radio.pos_s` — the frames at the decoder output divided by the source rate: received
  bytes would push it ahead by the whole buffer.
- **Seek** `radio.seek_s` = second (−1 — nothing): the same fraction of bytes as of seconds, a new
  `GET` with `Range: bytes=N-`, the length from the `Content-Range` of the 206 response.
  The ring's contents fade out (`request_flush`); the decoder is not reopened — it synchronizes on
  the next frame header. On VBR the seek is approximate.
- `radio.seekable` = a pushed track, not WAV, known duration. In WAV the byte fraction may land in
  the RIFF/JUNK headers or between PCM frames, so WAV is not seekable and is not resumed after a
  break.
- **Next track** — `radio.next_url` (DLNA writes the next-track slot there). ≤ 12 s before the end
  of the pushed track, the `radio_next` task (12 KB, PSRAM) opens it (`esp_http_client`, a 200
  response and `Content-Length` are required), picks the MP3, FLAC or WAV decoder by the file
  signature (another format is not prepared) and decodes the first PCM block (prefix ≤ 128 KB). At
  the end of the track, with the same source rate, the prepared stream becomes current without
  draining the ring (log `prepared handoff`); another rate, an unknown length or an unready prefix —
  an ordinary transition after draining. The MP3 encoder's delay and padding are not trimmed: the
  MP3 join is not sample-accurate.
- **End of track:** `radio.completed` + 1, `radio.completed_uri` — the track that ended,
  `radio.promoted_uri` — the next one picked up (empty when the track played to the end without one,
  log `track drained to end`).

### `radio.*` keys

| Key | Value |
|---|---|
| `radio.state` | `idle` / `connecting` / `buffering` / `playing` / `reconnecting`; `idle` while another source plays |
| `radio.cmd` | command: 1 play, 2 stop, 3 next, 4 previous |
| `radio.station` | index in the list (0–63), −1 — a catalog station; persist |
| `radio.name`, `radio.url`, `radio.uuid`, `radio.now_playing`, `radio.format`, `radio.buffer` | what is playing |
| `radio.count`, `radio.list_rev`, `radio.save` | station list |
| `radio.mode` | `station` / `push` |
| `radio.play_url`, `radio.next_url` | external URL; next track |
| `radio.pos_s`, `radio.dur_s`, `radio.seek_s`, `radio.seekable` | position and seek |
| `radio.completed`, `radio.completed_uri`, `radio.promoted_uri` | end of track |
| `radio.cat_*`, `radio.fav_add`, `radio.web_*` | catalog |
| `radio.covers`, `radio.report_clicks` | persist, default true |

## DLNA

Renderer (DMR) and control point (media server browser); there is no media server. Through the
renderer the panel is visible in Music Assistant (DLNA provider), BubbleUPnP, mConnect, foobar2000
and Windows' "Cast to Device".

| Layer | Source |
|---|---|
| SSDP, device description, SOAP, GENA | `espressif/esp_media_protocols` 1.0.1 — a pinned prebuilt archive; ISA `xespv2p1`, without the `xespv2p2` instructions, which the P4 v1.3 does not have |
| AVTransport, RenderingControl, ConnectionManager | `components/esp_dlna`, vendored from ESP-ADF ([README](../components/esp_dlna/README.md)) |
| SOAP AVTransport | `avt_soap`, `modules/dlna/src/dlna_soap.inc` |
| Link to SharedState | `modules/dlna/src/dlna_module.cpp` |
| Transport model | `components/dlna_client/include/dlna_transport.h`, shared with the browser |

The module does not play sound: the track is handed to the radio engine through `radio.play_url`
with `audio.source` = 5, the state is read from `radio.*`. The modules do not include each other —
the contract is keys only.

### Renderer startup

- Starts once, when `dlna.enabled` and `wifi.connected`, on the `dlna` task (6 KB PSRAM,
  priority 3, core 0): `esp_dlna_*_register_service()` build the service tables as arrays on the
  stack.
- Before the start `dlna/starting` is set in NVS, after it — cleared. A mark found at boot means the
  previous start did not return: the renderer is disabled for this run, the mark is erased, the
  next run tries again.
- Route registration: the wildcard `/*` is removed; for the duration of the call
  `components/esp_dlna/httpd_uri_compat.c` is switched in. The library is built without
  `CONFIG_HTTPD_WS_SUPPORT` and passes a 16-byte `httpd_uri_t` against 24 bytes in this build — httpd
  would read `supported_subprotocol` past the end of the object. `-Wl,--wrap=httpd_register_uri_handler`
  acts only while the library registers: it rebuilds the structure from the four filled fields and
  for `POST /AVTransport/*` installs the `avt_soap` handler. Then the wildcard comes back.
- Routes: `/rootDesc.xml`; for each service (`AVTransport`, `RenderingControl`,
  `ConnectionManager`) — `/<Service>.xml` and the control and event routes under `/<Service>/`, seven
  httpd handlers in all (registered by `esp_media_protocols`); SOAP AVTransport arrives at
  `/AVTransport/Control`. The module adds `/api/dlna/browser` (in the `http_routes` window at
  startup). The web does not occupy these paths.
- SSDP — while `dlna.enabled` and there is a network: 239.255.255.250:1900, `LOCATION:
  http://<ip>/rootDesc.xml`, announcements `upnp:rootdevice`, `MediaRenderer:1`,
  `ConnectionManager:1`, `RenderingControl:1`, `AVTransport:1`. The SSDP task stack is in internal
  RAM, contrary to the component's default (`stack_in_ext = false`): a task with its stack in PSRAM
  cannot run while the flash cache is disabled, and NVS and LittleFS are written by other tasks.
  Core 0. Disabling sends `ssdp:byebye`; the renderer routes stay registered.
- Identity: UUID `50344453-0000-1000-8000-<MAC>` (stable across restarts, control points remember
  the device by it), UDN `P4_DMR_<device_id>`, friendlyName — the device name, manufacturer
  `ModESP`, model `P4 Player`. No icon.

### Threads

1. The renderer callback (`renderer_request`) and `avt_soap` run in the httpd task inside the SOAP
   request: only reading and writing SharedState under the model's mutex (a wait of 50 ms in
   `avt_soap`, 1 s in the callback).
2. Actions that start or stop sound are put in the model's queue (8 commands); the main loop
   executes them — one per turn and only once the engine has taken the previous `radio.play_url`.
3. GENA NOTIFY — HTTP requests to subscribers; only the `dlna` task sends them (queue of 8): a
   subscriber that vanished without unsubscribing blocks until the connection timeout, and the
   main loop feeds the watchdog.

### AVTransport

| Action | Behavior |
|---|---|
| `SetAVTransportURI` | URI ≤ 256 B, DIDL-Lite ≤ 2048 B (otherwise `402`); not `http(s)://` — `716`; empty URI — Stop. Clears the next and the previous track. When the transport is playing, the new track plays at once; otherwise it is only loaded (`STOPPED` or `PAUSED_PLAYBACK`) |
| `SetNextAVTransportURI` | the next-track slot; the same limits and codes as in `SetAVTransportURI` |
| `Play` | `Speed` only `1` (`717`); with no track — `701`. Paused with the same URI — resume; otherwise a start: `audio.source` = 5, `radio.play_url`, state `TRANSITIONING` |
| `Pause` | pause of the ring's consumer (`pcm_ring().set_paused`): the socket stays, TCP holds back the server, resuming is instant. Not its own track, or `STOPPED` — `701` |
| `Stop` | its own track — `radio.cmd` = 2; state `STOPPED` |
| `Next` | the next track; none — `711` |
| `Previous` | the previous track; none — seek to 0 (works only with `radio.seekable`) |
| `Seek` | `REL_TIME` / `ABS_TIME` (otherwise `710`); target < duration (`711`); not its own track — `701`. Executed only with `radio.seekable`; otherwise the response is OK and the main loop discards the command |
| `GetTransportInfo`, `GetPositionInfo`, `GetMediaInfo`, `GetDeviceCapabilities`, `GetTransportSettings`, `GetCurrentTransportActions` | reads; time in the `H:MM:SS` format |
| `SetPlayMode` | only `NORMAL` (`712`) |

- Common: `InstanceID` ≠ 0 — `718`; body ≥ 16 KB — `402`; queue full or mutex busy —
  `715`; response over 32 KB — `501`. An error is a real UPnP SOAP fault, HTTP 500.
- The SOAP response of `GetCurrentTransportActions` always contains `Seek`; the event variable
  `CurrentTransportActions` — only with `radio.seekable`.
- Position and duration — `radio.pos_s` / `radio.dur_s`, which the module mirrors every 250 ms:
  a control point asks `GetPositionInfo` every second per subscriber.
- **Ownership.** A control point's action applies only to the track it sent (`audio.source` = 5,
  `radio.mode` = `push`, `radio.url` = its URI): pause or stop from the phone does not stop a
  station or a track from the card. `Play` with a new URI is itself the request to take over the
  output.
- **Start.** After a push the module waits up to 15 s for the engine's first sign of life
  (`connecting`, `buffering`, `playing`, `reconnecting`): for the first ~500 ms `radio.state` is still
  `idle`, because the command is in the queue. After it, `idle` means the end of the track.
- **End of track.** `radio.completed` with `radio.completed_uri` = its own track → the model's next
  track: `Adopt` when the engine has already picked it up (`radio.promoted_uri`), or `Play`; state
  `TRANSITIONING`. Without a next one — `STOPPED`. Log
  `Dlna: track completion: <uri>, next adopted|scheduled|empty`.

### RenderingControl, ConnectionManager

- `GetVolume` / `SetVolume` ↔ `audio.volume` (0–100), mute ↔ `audio.mute`. A volume change on the
  panel sends a RenderingControl event — the slider in the app follows.
- The `GetProtocolInfo` sink list: MP3 (`audio/mpeg`, `audio/mp3`, `audio/x-mpeg`, `audio/mpeg3`),
  FLAC (`audio/flac`, `audio/x-flac`), WAV (`audio/wav`, `audio/x-wav`, `audio/wave`), playlists
  (`audio/mpegurl`, `audio/x-mpegurl`, `audio/x-scpls`). Music Assistant uses it to decide what to
  transcode on the server. The engine also decodes AAC and M4A, but neither the sink list nor the
  browser offers them.

### Media server browser

`components/dlna_client`, task `dlna_client` (12 KB PSRAM, priority 1). Pages — "DLNA" on the panel
(`modules/ui_lvgl/src/screen_dlna.cpp`) and "DLNA media servers" in the web ([UI.md](UI.md)).

- Discovery: `M-SEARCH`, `ST: urn:schemas-upnp-org:device:MediaServer:1`, `MX: 2`, responses for
  2.5 s; up to 8 servers, no repeated `LOCATION`. From the description — `friendlyName`, `URLBase`,
  the ContentDirectory `controlURL`.
- `Browse` `BrowseDirectChildren` 12 rows at a time, up to 16 levels deep, XML ≤ 64 KB. HTTP or
  HTTPS: 4 s per operation, 12 s per request, up to 3 redirects.
- A track is playable when it has a `<res>` with `protocolInfo` `http-get:*:<mime>` and a MIME from
  the sink list (MP3, FLAC, WAV). The DIDL-Lite for the renderer is built anew with known namespaces.
- Command — `dlna.browse_cmd` = `revision << 12 | row << 4 | command`; commands: 1 search, 2 open,
  3 back, 4 next page, 5 previous, 6 queue, 7 cancel. A command with a revision other than the one
  shown is ignored: a tap on a row that has meanwhile changed does nothing.
- *Open* on a track — the track becomes current and plays; *queue* — the next-track slot.
- Cover: `albumArtURI` relative to the server's address, ≤ 512 KB → `CoverSource::Network`.
- The snapshot's `status` and the server list's `title` are translation catalog keys `@dlna.*`
  (arguments after `\x1f`): the panel and the web show them in their own language; in a folder,
  `title` is its name.
- `GET /api/dlna/browser` → `{"revision","busy","servers","can_back","count","next","previous",
  "title","status","rows":[{"title","artist","folder","playable"}]}`.

### `dlna.*` keys

| Key | Value |
|---|---|
| `dlna.enabled` | SSDP announcement; persist, default true |
| `dlna.state` | `NO_MEDIA_PRESENT` / `STOPPED` / `TRANSITIONING` / `PLAYING` / `PAUSED_PLAYBACK` — the same as the control point receives |
| `dlna.name` | friendlyName |
| `dlna.uri`, `dlna.title`, `dlna.artist`, `dlna.album`, `dlna.art_uri` | track and DIDL-Lite |
| `dlna.cmd` | transport from the panel or the web: 1 play/resume, 2 stop, 3 pause, 4 next, 5 previous |
| `dlna.browse_cmd` | browser command |

### Verification

```
curl -u <user>:<pass> -X POST -H 'Content-Type: application/json' http://<panel>/api/loglevel -d '{"tag":"Dlna","level":"info"}'
curl http://<panel>/rootDesc.xml     # the description XML; the web's HTML means the routes went in after the wildcard
```

- `dlna.name` and `dlna.state` — in `GET /api/state`.
- A control point on the same network shows the renderer within a few seconds; volume slider ↔
  `audio.volume` both ways.
- Track push: `Radio: playing pushed URL …`, `dlna.state` `TRANSITIONING` → `PLAYING`.
- End of track: `Radio: track drained to end` or `prepared handoff: …`, then
  `Dlna: track completion: …`; `dlna.state` = `STOPPED` without a next track, `TRANSITIONING` with
  one.
- `tools/dlna_fixture_server.py --host <PC IP>` (a MediaServer with test WAV and PNG) and
  `tools/dlna_acceptance.py --panel <URL> --fixture <URL>` — bounded checks of the renderer and the
  browser on the panel; host tests `tests/host/test_dlna_*.cpp`.

## AirPlay

`modules/airplay` over `components/airplay2` — the core of
[rbouteiller/airplay-esp32](https://github.com/rbouteiller/airplay-esp32) under a non-commercial
license; origin, license and local changes — [UPSTREAM.md](../components/airplay2/UPSTREAM.md). A
build without the module does not offer `audio.source` = 6.

Receiver: realtime ALAC over UDP and buffered AAC over TCP, HAP pairing (keys in the `airplay` NVS
namespace), PTP and NTP clocks, DACP client. Sound goes into the PCM ring as `OWNER_AIRPLAY`
([AUDIO.md](AUDIO.md)).

### Profiles

| `airplay.mode` | Profile | mDNS | RTSP port | `Server` | model |
|---|---|---|---|---|---|
| 0 (default) | AirPlay 2 | `_airplay._tcp` and `_raop._tcp` | 7000 | `AirTunes/377.40.00` | `AudioAccessory5,1` |
| 1 | AirPlay 1 · Windows compatible | only `_raop._tcp` | 5000 | `AirTunes/105.1` | `Shairport` |

- Shared policy — `components/airplay2/port/mode_policy.h`: announcement, port, version, features,
  the `/info` response (binary plist over RTSP, XML over HTTP).
- Profile 1 is classic RAOP only: without AirPlay 2 features and without AirPlay 2 multiroom.
- One profile is announced at a time.

### Enabling and switching

- The receiver starts once, when `airplay.enabled` and Wi-Fi is connected in STA mode, on the
  `ap_start` task with an 8 KB internal stack: on the first start HAP writes keys to flash. The
  `airplay_mod/starting` mark in NVS works as in DLNA: a start that did not return disables the
  receiver for the next run.
- The name is taken at module start, the identity is the base MAC; a new name — after a restart.
- `airplay.mode` is saved and applied without restarting the P4: the mDNS records are withdrawn, the
  `ap_mode` task restarts only the RTSP listener with the new profile, the current AirPlay session
  drops, the other services keep working; then the announcement comes back. Without Wi-Fi the switch
  waits with the status `@airplay.st_wifi`.
- `airplay.enabled` = false: the mDNS records are withdrawn, all clients are disconnected, the RTSP
  listener stops. true — the listener starts with the chosen profile.
- Web: the "Settings" page, the "Services" group — the AirPlay row with a switch, below it, while
  AirPlay is enabled, the "AirPlay mode" row with a list; the caption shows an error, or the state
  while switching is in progress. The active profile is shown on the "Player" page while AirPlay
  plays. Panel: "Settings → Services", a tap on the AirPlay row (not on the switch) opens a list of
  the two modes.
- The sender reads features when the receiver is selected. After firmware that changes features,
  select another output on the phone and then the panel again.

### Source, volume, remote

- A sender that started playing switches `audio.source` to 6 — only at the start of playback and
  only with `airplay.enabled`. Choosing another source on the panel: an AirPlay 1 sender is asked to
  pause through DACP; an AirPlay 2 sender (MRP is not implemented) keeps sending, the receiver holds
  the session and the clock and discards the sound, so returning to source 6 resumes in sync.
- Volume: the phone's slider −30…0 dB ↔ `audio.volume` 0–100 by position (−144, mute → 0).
  The room volume is announced to senders as `initialVolume` in `/info`. The phone's level moves the
  room only while AirPlay is the source and not in the first 3 s after connecting or the start of
  playback (`components/audio_dsp/include/volume_follow.h`).
- Remote: a sender that gave `DACP-ID` and `Active-Remote` (AirPlay 1: TuneBlade, Music Assistant,
  iTunes, an iPhone or a Mac in mode 1) — `airplay.remote` = true; `airplay.cmd` (1 play, 2 pause,
  3 stop, 4 next, 5 previous, 6 toggle) goes to it through DACP. An AirPlay 2 sender gives no
  remote.

### Covers

Requested while the cover task `ap_cover` (6 KB PSRAM) runs: the RAOP record `md=0,1,2` for an
AirPlay 1 sender and features bit 15 for AirPlay 2 — an AirPlay 2 sender asks for the cover by
features, not by `md`. Bits 16 (progress) and 17 (text) are always set by the AirPlay 2 profile.
Cover ≤ 256 KB (the RTSP buffer limit), JPEG or PNG → `CoverSource::AirPlay`; `image/none`
clears it; a new title without a cover within 5 s removes the old one.

### `airplay.*` keys

| Key | Value |
|---|---|
| `airplay.enabled` | persist, default true |
| `airplay.mode` | the chosen profile; persist |
| `airplay.active_mode`, `airplay.active_mode_label` | the active profile: `AirPlay 2` / `AirPlay 1 · Windows` / `@airplay.st_stopped` |
| `airplay.switching` | switching is in progress |
| `airplay.mode_status` | `@airplay.st_starting`, `st_switching`, `st_applied`, `st_not_applied`, `st_stopped`, `st_wifi` — translation catalog keys |
| `airplay.mode_error` | the `esp_err_t` name; empty — no error |
| `airplay.state` | `off` / `idle` / `connected` / `playing` / `paused` / `switching` |
| `airplay.name` | the name in the AirPlay list |
| `airplay.title`, `airplay.artist`, `airplay.album`, `airplay.pos_s`, `airplay.dur_s` | the sender's metadata |
| `airplay.remote`, `airplay.cmd` | DACP remote |

Kconfig `components/airplay2/Kconfig`: `AIRPLAY_TIMING_THRESHOLD_MS` 25 (buffered stream),
`AIRPLAY_RT_TIMING_THRESHOLD_MS` 50 (realtime) — how far a frame may be early or late before it is
held back or dropped.

### Checks

```
python tools/tests/test_airplay_mode.py
python tools/tests/test_airplay_mode_module.py
python tools/tests/test_airplay_reconfigure.py
python tools/airplay_send.py <panel ip>     # an AirPlay 1 (RAOP) sender with a test signal
```

The tests run both profiles with simulated transport and mDNS (they need Python and a native C
compiler). `airplay_send.py` does not reach the AirPlay 2 half (HAP, PTP, buffered AAC).

## MQTT

`components/modesp_mqtt`. Compiled together with networking (`CONFIG_MODESP_CLOUD_MQTT` — the
default choice with `MODESP_NET_ENABLE`), disabled at runtime: NVS `mqtt/enabled` defaults to false.

- Settings: `GET /api/mqtt` → `{"enabled","connected","broker","port","user","prefix",
  "tenant","device_id","status"}`, `POST /api/mqtt` ← `{"enabled","broker","port","user",
  "password","prefix","tenant"}`; or the web, "Service → Other settings". NVS namespace `mqtt`:
  `broker`, `port` (default 1883), `user`, `pass`, `prefix`, `tenant`, `enabled`, `broker_ca` (PEM).
- TLS: `mqtts://` in `broker`, or port 8883 with an address without a scheme → TLS with the
  `broker_ca` certificate or the built-in bundle; otherwise plain text, `mqtt.status` = `insecure`.
- Prefix: `player/v1/<tenant>/<device_id>`, without a tenant — `player/v1/pending/<device_id>`
  (`mqtt_topic_root` in `project.json`; `device_id` — the last three bytes of the MAC, hex).
- `<prefix>/status` — `online` / `offline` (LWT, QoS 1, retain).
- `<prefix>/state/<key>` — every 1 s, the changed keys from the `mqtt.publish` sections of the
  manifests: `audio.source`, `audio.volume`, `audio.mute`, `audio.state`, `audio.underruns`,
  `ui.backlight`, `ui.encoder_pos`, and also `_ota.status`, `_ota.progress`, `_ota.error`.
- `<prefix>/heartbeat` — every 30 s `{"proto":1,"fw","up","heap","rssi":0}`.
- Commands — subscription `<prefix>/cmd/+`: any writable key of type int, float or bool from
  `state_meta` (the range is clamped, a command key goes to the module's queue). String keys
  (`radio.play_url`) are not applied through MQTT. Retained commands are ignored. Service command:
  `_request_full_state`. The framework's broker administration — `_ota`
  (`{"url","version","checksum":"sha256:…"}`: a firmware download, verified and written to an OTA
  slot), `_set_mqtt_creds`, `_set_tenant` — is off on the player
  (`CONFIG_MODESP_MQTT_REMOTE_ADMIN=n`): anyone who can publish to the broker could use it, with
  no login, and Home Assistant needs none of it.
- Reconnection: 5 s, doubling up to 5 min. The client's network steps have a 3 s timeout, and the
  old client is stopped by the `mqtt_stop` task: `esp_mqtt_client_stop()` can wait out a whole
  connection attempt, while the main loop feeds a 5-second watchdog.
- Home Assistant discovery: the manifests declare no entities, nothing is published.
- `mqtt.status`: `disabled`, `no_broker`, `waiting_wifi`, `connecting`, `connected`, `insecure`,
  `disconnected`, `error`; also `mqtt.connected`, `mqtt.broker`.

## Diagnostics

- The default log level is WARN; of the network tags only `Radio` is at INFO
  (`components/modesp_services/src/log_policy.cpp`). The rest — `WiFi`, `HTTP`, `WS`, `Dlna`,
  `NextStream`, `AirPlay`, `airplay2`, `mdns_airplay`, `MQTT`, `UserData` — are opened by
  `POST /api/loglevel` until a restart; lines printed during boot cannot be seen this way.
- `Radio` every 5 s while playing: `stream: buffer P%, net N B/s (need M), audio X.XXx` and a line
  with the load of the pump, the decoder and the resampler.
- Tasks `radio`, `radio_net` (30 s) and `dlna` (10 s) are supervised by `modesp::liveness`.
- Soak test of the SDIO link under a stream — [SOAK_TEST.md](SOAK_TEST.md).
