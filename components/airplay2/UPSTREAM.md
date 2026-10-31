**English** · [Українська](UPSTREAM.uk.md)

# airplay2 — code provenance

The AirPlay 2 receiver in this component is **not this project's code**. It is the core of
[rbouteiller/airplay-esp32](https://github.com/rbouteiller/airplay-esp32), taken at

| | |
|---|---|
| commit | `811d5f8af750096683dcec7c95d1cbc08c2f7601` (v0.2.1, "Merge pull request #163 … release-v0.2.1") |
| licence | Non-Commercial License, Copyright (c) 2026 Remi Bouteiller — [`LICENSE`](LICENSE), verbatim |

## The licence and what follows from it

The licence allows use, copying, modification and distribution **for non-commercial purposes only**
and requires its notice in every copy. The firmware runs on one unit, for its owner's personal
use — only on that condition can the component be used.

- `LICENSE` stays in this directory, the upstream files stay as they are, notices included.
- Do not relicense this code and do not present it as the project's code.
- **Whoever makes a product out of the firmware** must either remove this component (and
  `modules/airplay`, its only user) or first obtain a commercial licence from the copyright holder.
  The rest of the firmware does not depend on it: without the module, `audio.source = 6` is simply
  not offered.

## Layout

| | |
|---|---|
| `upstream/` | upstream files, **byte-identical** to the commit above, in upstream's own layout (without `main/`). `upstream.sha256` lists every one with its hash; `tools/tests/test_airplay2_upstream.py` fails if a file differs, is missing or is not in the list. |
| `port/` | the project's code: what binds the core to this player instead of to upstream's boards. |

A change to upstream behaviour goes in `port/`. When an upstream file cannot stay unchanged, it is
copied whole into `port/`, `CMakeLists.txt` compiles the copy instead of the original, and the original
stays untouched in `upstream/` (and in `upstream.sha256`); the change is described below. So
`upstream/` always means exactly "as published". Adapted copies: `rtsp_server.c`, `rtsp_message.c`,
`rtsp_handlers.c`, `bplist_builder.c`, `bplist_parser.c`.

## What was taken

The receiver itself: the RTSP server and handlers, HAP pairing (SRP-6a, Ed25519 via libsodium),
FairPlay setup, plist/bplist, the PTP and NTP clocks, the realtime (ALAC over UDP) and buffered (AAC
over TCP) receivers, the timing engine, the decoder glue (on top of `esp_audio_codec`), the DACP
client, the ALAC magic cookie.

## What was not taken, and what replaces it

| upstream | why not | here instead |
|---|---|---|
| `main.c`, `network/wifi.c`, `ethernet.c`, `dns_server.c`, `web_server.c`, `ota.c`, `log_stream.c` | the player has its own network stack, web UI and OTA | `modesp_net`, `modules/airplay` |
| `audio/audio_output.c` (+ `_spdif`, `_usb`) | they own an I2S port; the ports here belong to the audio core | `port/` — writes into `pcm_ring` |
| `audio/audio_resample.c`, `components/audio-resampler` | the output clock follows the source, and where it cannot, the player has its own converter | — |
| `settings.c` | upstream keeps its settings in NVS, the player in SharedState | `port/settings.h` |
| `playback_control.c`, `buttons.c`, `led.c`, `display`, `boards`, `dac*`, `a2dp_sink.c`, `bt_coex.c`, `audio/eq_events.c` | features of upstream's hardware; Bluetooth here is the satellite's job | — |
| `network/mdns_airplay.c` | calls `mdns_init()` and sets the hostname — both belong to `modesp_net` | `port/mdns_airplay.c` — only adds the services |
| `settings.h`, `components/dac/include/dac.h` | headers of files not taken, still included by files that were taken | `port/settings.h` (the core's four calls), `port/dac.h` (declared, never called) |

## Third-party material inside upstream files

- `rtsp/rtsp_fairplay.c` — the FairPlay setup replies are fixed byte tables. Upstream does not name
  their source; the same tables appear in other open-source AirPlay receivers. They are data answering
  Apple's handshake, carried as upstream ships them.
- Comments in several files describe behaviour "like shairport-sync"; the file headers claim no code
  from it.

## Dependencies

As in upstream's `dependencies.lock` at that commit, except what the player already pins itself
(the component's `idf_component.yml`, versions from the project's `dependencies.lock`):

| | upstream lock | here |
|---|---|---|
| `espressif/libsodium` | 1.0.21 | `==1.0.21` (built from source; there is no prebuilt archive to check for `xespv2p2`) |
| `espressif/esp_audio_codec` | 2.5.0 | `>=2.0.0,<2.6.0`, as in `modules/player` and `modules/radio`: 2.6 requires chip revision 3.0, and the board is v1.3; the build has 2.5.0 |
| `espressif/mdns` | 1.11.1 | `>=1.3.2`, as in `modesp_net`; the build has 1.12.0 |

## Local changes

No file in `upstream/` is edited. How the behaviour differs from upstream:

1. **`esp_read_mac` is redirected for upstream files only** (`port/redirect.h`, force-included by
   `CMakeLists.txt` into the upstream sources and their adapted copies). Upstream takes the device id
   from the Wi-Fi STA MAC in `rtsp_get_device_id()`, in the AirPlay 1 Apple-Challenge response and in
   the pair-verify accessory info. The ESP32-P4 has no Wi-Fi of its own (`SOC_WIFI_SUPPORTED` is not
   defined for it): `esp_read_mac(…, ESP_MAC_WIFI_STA)` returns `ESP_ERR_NOT_SUPPORTED` and leaves the
   buffer untouched, so the id would be uninitialised stack, which pair-verify signs. Here that one
   type answers with the MAC the module passes to `airplay2_start()` (the base MAC,
   `modesp::device_mac()`); every other type goes to the real function.
2. **`network/mdns_airplay.c` is replaced** by `port/mdns_airplay.c`, derived from it: the same
   TXT records, without `mdns_init()` / `mdns_hostname_set()`; callable again after the network stack
   has restarted mDNS; one MAC for `deviceid` and the RAOP name.
3. **The settings the core reads come from the module**, not from NVS: the name from
   `airplay2_start()`, the volume as the phone last set it (it becomes `audio.volume`, which the player
   persists). Upstream's NVS namespace `airplay` is used only by `hap.c` — for the pairing keys.
4. **The audio output is the player's ring** (`port/audio_output_ring.cpp`), not an I2S port.
5. **Cover art is taken, not dropped.** Upstream's `handle_set_parameter` sees an image body and at
   most sets `has_artwork`; after that the bytes are gone, and there is no hook in between.
   `rtsp_dispatch()` — the one function that sees every request whole, called from `rtsp_server.c` —
   is wrapped at link time (`-Wl,--wrap=rtsp_dispatch`, `port/artwork.c`): an `image/*` body is handed
   to the module on the way past, and every request then goes to upstream unchanged. Whether senders
   are asked for covers (`md` in the RAOP record) is decided by `airplay2_config_t::artwork` at start;
   upstream's `CONFIG_ENABLE_AIRPLAY_ARTWORK` stays off, since all it adds is that flag.

   `md` is read by an AirPlay 1 sender. An AirPlay 2 sender is asked by the features bits — 15 artwork,
   16 progress, 17 text, as shairport-sync sets them; upstream advertises none of the three. The
   AirPlay 2 profile's features here carry 16 and 17, and 15 when covers are asked for
   (`port/mode_policy.h`, `airplay_mode_features()`); the classic profile's features are upstream's.

6. **Opt-in negotiation trace** (`port/diagnostics.c`): the dispatch wrapper logs a bounded request
   line and allowlisted headers. Link-time wrappers record the response status, body length,
   encryption state and send result, including for `rtsp_send_ok` (its internal call to
   `rtsp_send_response` bypasses the link-time wrapper). Bodies, authentication headers and key values
   are not logged. To turn it on: the `airplay_trace` tag at INFO through `/api/loglevel`, plus
   `rtsp_server` for connection boundaries. By default both are at WARN. The trace changes neither the
   replies nor the upstream files.

7. **Protocol mode at runtime** (`port/mode_policy.h`): the persisted `airplay.mode` selects
   AirPlay 2 (0, default) or classic RAOP (1, port 5000, `AirTunes/105.1`). The module removes mDNS,
   runs `airplay2_set_legacy_mode()` on a worker with an internal stack and advertises the services only
   once the listener is ready. A mode change drops AirPlay sessions without restarting the P4; the HAP
   identity, the allocated audio resources and the shared output worker stay initialised. The UI shows
   the switch, the active profile, the status and errors; rapid changes are serialised, the latest
   selection wins. A failure attempts a rollback, and if that fails too, leaves an explicit unavailable
   state.

   Four adapted copies replace upstream sources: `rtsp_handlers.c` changes features / version /
   model in `/info`, `rtsp_message.c` the `Server` headers, `bplist_builder.c` model / source version
   in `/info`, `rtsp_server.c` the listener port and lifecycle. A checked stop waits for both client
   tasks and the server and keeps the stop flags on timeout. Start waits for bind/listen to succeed;
   sockets are closed in their owner task, cleanup is serialised, connection access is guarded. A
   profile change happens only after the old tasks have finished. The originals stay in `upstream/`,
   and so does the MAC redirect.

   `modesp::MdnsGuard` (`components/modesp_net/include/modesp/net/mdns_guard.h`) serialises Wi-Fi's
   mDNS init/free with the registration/removal of the AirPlay services, between the ESP event task and
   the module loop. No mDNS lock is held while waiting for the RTSP tasks. Tests: profile replies,
   lifecycle failures and rollback, the selection queue in the module (`tools/tests/test_airplay*.py`).

8. **ANNOUNCE is held to the buffers' shape** (`port/rtsp_handlers.c`, `parse_sdp`). Upstream
   takes the channel count, frame length and bit depth from the SDP as sent, while the decoder buffer
   and every PCM ring slot are sized for `AUDIO_MAX_CHANNELS` 16-bit channels and
   `MAX_SAMPLES_PER_FRAME` frames (`audio/audio_buffer.h`); three or more channels overrun both. An
   rtpmap outside 1..2 channels, or an ALAC fmtp outside 1..2 channels, 1..4096 frames or 16 bits, gets
   400 and changes nothing. Standard RAOP SDPs (stereo, 16-bit, 352) are unaffected
   (`tools/tests/test_airplay_sdp.py`).

9. **The binary plist parser is bounds-checked** (`port/bplist_parser.c`, an adapted copy that
   replaces `plist/bplist_parser.c` in the build). Upstream trusts the sender's trailer, object
   indices, offsets and counts: SETUP, TEARDOWN and the other bplist handlers reachable before pairing
   can be made to read far past the request buffer or to loop forever. The copy validates the trailer
   (object count, top object, offset table between header and trailer), refuses an index past the
   table or an offset that is not an object, checks every offset before dereferencing it, does its
   range arithmetic in 64 bits with lengths and counts bounded by the plist's size, bounds the work of
   the recursive search and refuses more than eight streams. Well-formed plists parse exactly as in
   upstream (`tools/tests/test_airplay_bplist.py`, under ASan/UBSan and in a 32-bit build where the
   host has them).

10. **RTSP receive buffers have room for the terminator** (`port/rtsp_server.c`). Each message is
    NUL-terminated at `buffer[total_len]` while it is dispatched; with a message exactly as long as the
    buffer (4096 B, or 256 KB after growing), upstream writes that byte past the allocation. The buffers
    are allocated one byte over their capacity (`tools/tests/test_airplay_rtsp_buffer.py`).

11. **The decoder is not swapped under a live stream.** `audio_receiver_set_format()` frees and
    recreates the one decoder the receive task decodes with, without a lock. `port/rtsp_handlers.c`
    answers 455 to an ANNOUNCE, or to an AirPlay 1 SETUP (a Transport header, no streams), after SETUP
    (until TEARDOWN), changing nothing (`tools/tests/test_airplay_sdp.py`,
    `tools/tests/test_airplay_setup.py`). `port/rtsp_server.c`: a replaced session stops the receiver
    in its own cleanup, which the server does not wait for, so a new client holds its requests (up to
    3 s) until the replaced slot has stopped the receiver, the decoder and the NTP clock.
    Not protected: an AirPlay 2 stream SETUP on a live connection, realtime or buffered. `handle_setup`
    calls `audio_receiver_set_format()` in its loop over streams before anything stops the stream:
    `audio_receiver_start()` stops and restarts a realtime stream only after the decoder has already
    been freed, and `audio_receiver_start_buffered()` returns at once while the buffered task runs.

12. **DACP's mDNS queries take `modesp::MdnsGuard`.** `dacp_client.c` looks up `_dacp._tcp` from
    the client's RTSP task (the AirPlay 1 grace period) and from its `dacp_wk` worker without
    protection, while Wi-Fi, under that same guard, may call `mdns_free()` and delete the semaphore the
    query waits on. `mdns_query_ptr` is wrapped at link time by `modules/airplay`
    (`-Wl,--wrap=mdns_query_ptr`, `src/mdns_query_guard.cpp`), which depends on `modesp_net` anyway;
    the component gains no new dependencies.

13. **A message too big for the buffer is skipped instead of breaking the connection**
    (`port/rtsp_server.c`). The receive loops keep 1024 B free for the next read (one encrypted
    block), so nothing over `RTSP_MESSAGE_MAX` (256 KB less that) can be assembled; upstream then closes
    the connection, and the AirPlay session with it — for example, over a cover that is too big
    (item 5). Here its bytes are dropped as they arrive, the buffer does not grow for it, and the sender
    gets 200 with its CSeq once the last byte is in; the request reaches no handler, and the next
    message is framed as always (`tools/tests/test_airplay_rtsp_buffer.py`).

14. **A buffered packet with SSRC 0 is not decoded** (`port/buffered_audio.c`). Bytes 8–11 of each
    AirPlay 2 buffered packet name its encoding; shairport-sync decodes only a known SSRC and never 0,
    while upstream decodes every packet with the decoder SETUP created.
    `audio_crypto_decrypt_buffered()` — the one call that sees the packet whole — is wrapped at link
    time: SSRC 0 is refused before decryption (upstream counts and drops it as a packet that failed to
    decrypt), and an unknown SSRC or a switch between known formats is decoded as in upstream and
    reported at WARN once per run. `audio_decoder_decode()` is wrapped too: a frame the decoder refused
    is reported with its length, the last packet's SSRC and its first bytes
    (`tools/tests/test_airplay_buffered_ssrc.py`).

15. **GET /info announces the room's volume** (`port/rtsp_handlers.c`, `port/bplist_builder.c`,
    `port/airplay2.c`). Upstream's `/info` has no `initialVolume`, so a sender, as it starts, restates
    the level it last used with this receiver, and the room jumps to it. Both answers carry
    `initialVolume`: the binary one through `bplist_build_info_response_vol()` (two more objects;
    upstream's `bplist_build_info_response()` and its declaration in `upstream/plist/plist.h` stay, as
    a wrapper), the XML one through `plist_dict_real()` on the port side, because
    `upstream/plist/plist_xml.c` has no `<real>`. The value is the port's volume, rounded to 0.01 dB so
    that both answers carry the same number; the module keeps it at the room's level through
    `airplay2_report_volume()` (without `AIRPLAY2_EV_VOLUME`). The client takes its slider from this key
    and sends no level until somebody moves it (`tools/tests/test_airplay_mode.py`).

16. **An `ekey` is decrypted only when its key fits** (`port/rtsp_handlers.c`, `handle_setup`).
    Upstream decrypts an `ekey` of up to 64 bytes into a 32-byte key on the RTSP task's stack.
    libsodium writes the whole plaintext, and zeroes as much when the tag does not verify, so up to
    48 bytes land in 32 — from the network, after the raw pair-verify, which checks no signature.
    An `ekey` longer than a 32-byte key and its tag (48 bytes) is not decrypted: like one that does
    not decrypt, it leaves the session's derived key (`tools/tests/test_airplay_setup.py`).

17. **A stream's rate is held to 8–192 kHz** (`port/airplay2_port.h`, `port/rtsp_handlers.c`,
    `port/audio_output_ring.cpp`). Upstream takes the rate from ANNOUNCE's SDP (rtpmap, ALAC fmtp)
    and from an AirPlay 2 stream's `sr` as sent, and the timing engine and the output divide by
    it. While another source plays, the output paces its reads by the rate's frame period
    (`pace_unheard`); a rate in the hundreds of MHz rounds that period to nothing, `ap_play`
    never sleeps, and the task watchdog restarts the panel. A rate outside 8–192 kHz gets 400:
    ANNOUNCE changes nothing, SETUP stops before the receiver's format is set. The output ignores
    such a rate as well (`tools/tests/test_airplay_sdp.py`, `tools/tests/test_airplay_setup.py`).

18. **A session without DACP drops the one a replaced slot left** (`port/rtsp_handlers.c`,
    `rtsp_dispatch`). The DACP session is global. An AirPlay 1 slot waiting out its grace period
    (paused, its DACP service still advertised) takes any new connection for its sender
    reconnecting and leaves the session set. An AirPlay 2 sender names no `DACP-ID` or
    `Active-Remote`, so nothing replaced it, and the panel's pause and next went to the previous
    device. A SETUP on a connection without both identifiers clears the session; a sender that
    names them sets its own, as before (`tools/tests/test_airplay_dacp.py`).

19. **An AirPlay 2 session starts PTP itself** (`port/rtsp_handlers.c`, `handle_setup`). An
    AirPlay 1 SETUP stops the PTP clock for its sockets, and only the cleanup of a slot that was
    not replaced starts it again, on the word that the new session's SETUP manages PTP; upstream's
    AirPlay 2 SETUP never starts it. An AirPlay 2 sender that replaced an AirPlay 1 session played
    all of it without a clock lock, on a local anchor. The AirPlay 2 initial SETUP starts PTP when
    it is not running (`tools/tests/test_airplay_setup.py`).

20. **The binary /info carries any name** (`port/bplist_builder.c`). Upstream writes the device
    name as an ASCII string object (marker 0x5X) whatever it holds. The name is the owner's own
    text ([NETWORK.md](../../docs/NETWORK.md), "Device name and mDNS"), and in Cyrillic it made an
    object CoreFoundation reads as ASCII, so the plist was not valid. A name with a byte over 0x7F
    is written as UTF-16 big-endian (marker 0x6X, counted in code units, surrogate pairs above
    U+FFFF, U+FFFD for a byte that starts no well-formed UTF-8 sequence); an ASCII name is written
    as before (`tools/tests/test_airplay_mode.py`).

## Updating

Clone upstream at the new commit, compare it with `upstream/` file by file, carry the changes over,
regenerate `upstream.sha256`, update the commit at the top and re-check every row of both tables above
against the new tree — a file that moved between "taken" and "not taken" will not show in a plain
diff. Check the adapted copies in `port/` against the new originals by hand.
