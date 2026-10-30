**English** · [Українська](AUDIO.uk.md)

# Audio core

The audio core is the `modules/audio` module (class `AudioModule`) and the `components/audio_dsp`
library. It selects the source, runs it through the DSP chain and hands it to I2S0 → PCM5102A →
TPA3255. Control and status are SharedState `audio.*` keys only
([`modules/audio/manifest.json`](../modules/audio/manifest.json)); no sample passes through the
message bus. The source contract for other modules: `docs/CONCEPT.md`, "SharedState contract:
sources"; where each key is set on the glass and in the web: `docs/UI.md`.

```
source ──┬─ I2S input (BT, AUX) ───────┐
         ├─ PCM ring (file, radio/DLNA, AirPlay, USB)
         └─ test tone ─────────────────┤
                                       ▼
        envelope → DSP chain → output gate → int32 → I2S0 TX → PCM5102A → TPA3255
```

## I2S ports

The ports are the `drivers/i2s_std` driver, set up from the `i2s_buses` entries in
[`boards/p4_player/board.json`](../boards/p4_player/board.json) and the roles in
[`bindings.json`](../boards/p4_player/bindings.json). The module sees them only through the seam
`components/modesp_hal/include/modesp/hal/audio_sink.h` (`IAudioSink::as_port()` →
`IAudioPort`: stereo int32, `read`/`write`, `set_rate`, `set_format`, `set_link_master`,
`set_line_input`). The JP1 header pinout: `docs/CONCEPT.md`, "Pinout".

| Port | Entry | Role in `audio` | Pins (GPIO) | Clock |
|---|---|---|---|---|
| I2S0 | `i2s_a`, master, 32-bit slots, MCLK 256·fs | `port_main`: TX → PCM5102A; RX — the BT link (protocol 1.9) or AUX | MCLK 49, BCLK 50, WS 51, DOUT 52, DIN 29 | APLL |
| I2S1 | `i2s_b`, slave, opens at 32 kHz / 32 bits | `port_bt`: RX from a satellite that clocks the link itself | BCLK 33, WS 30, DIN 31 | XTAL 40 MHz; APLL only when the format does not fit otherwise |
| I2S2 | not in `board.json` | the panel's microphone (ES8311), opened for the duration of a measurement | — `docs/MIC_CAPTURE.md` | XTAL, 48 kHz |

- **Master on APLL**: synthesises MCLK exactly (12.288 MHz at 48 kHz); APLL = 2·MCLK. From XTAL
  such frequencies come only out of a fractional divider, with jitter on BCLK.
- **Slave on XTAL**: the clock comes from the wire, APLL gives it nothing, and a second APLL owner
  freezes its frequency and blocks the output retune.
- **PCM5102A synthesises its clock from BCLK**; MCLK on GPIO49 is output only with the AUX source,
  for the rest the pin is held low (`set_line_input`).
- **DMA**: a descriptor is 480 frames. TX — 8 descriptors = 3840 frames (80 ms at 48 kHz, 40 ms
  at 96 kHz); RX of the duplex port — the same. The RX-only slave port — 6 × 120 frames (the DMA
  memory budget is fixed, because the port opens at the widest slot). I2S interrupt — priority 3:
  IDF passes on only the last completed RX block per interrupt, so late servicing loses the
  previous one.
- **TX starved** → the DMA sends zeros by itself (`auto_clear_after_cb`), log `TX DMA ran dry`.
- **Never a digital zero on the bus**: sample 0 is replaced with 256 (1 LSB of 24 bits, −138 dBFS).
  After ~21 ms of exact zeros PCM5102A turns its analogue output off, and coming out of that mute
  is a click.
- **DMA cache write-back in 1 KB chunks** (`CONFIG_ESP_MM_CACHE_MSYNC_C2M_CHUNKED_OPS`,
  `boards/p4_player/sdkconfig.board`): after every `write` the I2S driver writes the TX buffer back
  from the cache under a spinlock with core 1's interrupts disabled (both the audio task and the
  USB interrupt are there), and USB audio has 125 µs until the next packet.
- **`amp_enable`** (`gpio_outputs`, GPIO34, HIGH = playing): TPA3255 /RESET + PCM5102A XSMT. The
  HAL keeps the line off until the module starts; from then on only the audio task drives it
  (section "Edges of sound").

## Output rate and sessions

- The output clock follows the source: the port accepts 44.1…192 kHz, the core sets no higher
  than **96 kHz** (`PLAY_MAX_HZ`, `modules/audio/include/audio_module.h`). Above 48 kHz the sound
  lies beyond the amplifier's output filter and the speakers, and DSP at 192 kHz costs twice as
  much as at 96.
- **One rate per source session.** Selecting a source opens a session; the first track picks the
  rate (the producer asks `PcmRing::ask_rate`, the core retunes the port and answers
  `set_out_rate`). The producer converts later tracks at another rate to it with a fixed ratio, so
  a mixed 44.1/48 playlist plays without gaps. A retune stops BCLK, the DAC's PLL re-locks, and
  that is audible; hence once per session.
- A new session within a source only when the producer asks for one itself
  (`PcmRing::request_new_session`): USB — on every new stream, because the computer chooses the
  rate. Bluetooth retunes the output on every adopted format (section "Bluetooth via the
  satellite").
- A retune happens only in silence: both gates closed, the old rate's frames played out
  (`PcmRing::rate_change_due`: the ring is empty or waiting for a flush). After a retune everything
  that depends on fs is redesigned (bands, limiters, ramps, spectrum, tone), the chain is reset, and
  the output is held closed for 100 ms.
- A rate above 96 kHz or one the port refuses → `audio_core::fallback_rate()`: the nearest member
  of the same family (44.1·2ⁿ or 48·2ⁿ) — above the ceiling, the highest one not above 96 kHz
  (176.4 → 88.2, 192 → 96, ratio 2); refused inside the range, the family's base (44.1 or 48 kHz).
  One attempt; if the port does not take that either, the clock stays where it is and the producer
  converts into it.
- The producers' fixed-ratio converter is `audio_core::RateConverter` (`components/audio_pipe`) on
  the resampler `components/audio_resampler` (dbry, BSD-3); the filter bank is built per pair of
  rates, not per track.

## The PCM ring and its owners

`audio_core::PcmRing` (`components/audio_dsp/include/pcm_ring.h`) is one SPSC ring of
interleaved stereo float between the producers and the audio task; lock-free on both sides.

- Capacity 24,000 frames (0.5 s at 48 kHz), PSRAM. `player` and `radio` allocate it at start (a
  repeated `init` of the same size does nothing); once the ring exists, the file and radio sources
  appear in `audio.sources`.
- Owners (`Owner`): `OWNER_FILE` — `modules/player`; `OWNER_RADIO` — `modules/radio` (the DLNA
  renderer plays through it, `docs/NETWORK.md`); `OWNER_AIRPLAY` —
  `components/airplay2/port/audio_output_ring.cpp`; `OWNER_USB` — `components/usb_uac`. A write
  not from the owner is discarded.
- `claim()` empties the ring at once. `claim_after_fade()` first hands the other owner's queue
  that is still audible to the consumer to fade out and drop, and waits up to `FLUSH_WAIT_NAPS` =
  60 × ~5 ms (~300 ms); if the wait runs out, it takes the ring anyway.
- `apply_source()` resets the ring if its owner is not the new source's producer: the previous
  producer's tail does not prime the new source.
- Control from the producer's side: `set_paused` (consumer pause, the queue is kept),
  `request_flush` (fade out and drop), `set_eos` (the track has ended, rather than the producer
  falling behind), `generation()` (grows with every reset; a write of an old generation is
  discarded).
- Rate handshake: `ask_rate` / `out_rate` (0 — the question is open), `set_clock_locked` (the
  session's rate is chosen), `clock_moved()` (a producer that is already playing sees a late
  answer and rebuilds its converter).
- `set_consumer(prime, downstream)`: the cushion after a reset (100 ms) and the frames between the
  ring and the pins (TX DMA + block + limiter look-ahead). From these AirPlay computes when what it
  wrote will be heard, and USB its fill target for feedback. `set_consumer_live` — whether the
  ring is heard right now.
- After a reset the consumer waits for the 100 ms cushion (`PRIME_S`); `eos` lifts the wait. While
  the amplifier settles, the ring is not read — except for AirPlay and USB, which pace by the fill
  (`paces_by_fill`).
- A short block is padded with silence; `audio.underruns` grows only when there is no `eos` and no
  retune is pending.

## The audio task

- Task `audio`: core 1, priority 22, 8 KB stack in internal SRAM (a stack in PSRAM crashes while
  the cache is disabled by a flash write). A block is 240 frames (`DspChain::BLOCK`, 5 ms at
  48 kHz).
- Every block: commands from the queue (16 entries) and the volume from `LevelLatch` → deferred
  source change → source block → source counters → `amp_enable` → envelope → DSP → output gate →
  spectrum → int32 → `write` to I2S0 (up to 200 ms). The TX DMA sets the pace; for AUX, the RX
  read.
- Volume and "Maximum" go not through the queue but through a last-value register
  (`components/audio_dsp/include/level_latch.h`): the task reads it every block, and no value is
  lost. `audio.vol_eff` is the level the task has already applied.
- Status every 0.5 s through a structure under a spinlock; the main loop publishes the keys and
  writes the log. The task does not log measurements and counters itself: a console write blocks
  for longer than a block. Its own lines are only for events in silence (source change, retune, BT
  format adoption).
- The task is subscribed to the task watchdog and to `modesp::liveness` (3 s).

## Sources

| `audio.source` | `audio.state` | What the core does |
|---|---|---|
| 0 | `off` | silence, amplifier off |
| 1 | `tone` | test tone (section "Test tone") |
| 2 | `aux` | reserved: only in a build with `CONFIG_AUDIO_LINE_IN_FITTED=y` (default `n`, `modules/audio/Kconfig`) — reads I2S0 RX (DIN GPIO29) on the same clock and outputs MCLK; without the option the request is refused |
| 3 | `bt` | Bluetooth via the satellite (section "Bluetooth via the satellite") |
| 4 | `file` | ring, owner `OWNER_FILE` |
| 5 | `radio` | ring, owner `OWNER_RADIO` (radio and DLNA) |
| 6 | `airplay` | ring, owner `OWNER_AIRPLAY`; only when the `airplay` module is in the build |
| 7 | `usb` | ring, owner `OWNER_USB`; only when the USB device has started |

- `audio.state` also: `no-port` (no `port_main` binding), `error` (the port did not come up).
- `audio.sources` is a bit mask of what can be selected (bit = value): `off` always; `aux` with the
  option above; `bt` if `port_bt` came up and the BT ring was allocated; `file`/`radio` when the
  ring exists; `airplay`, `usb` — by the weak symbols `airplay_offered()` / `usb_audio_offered()`.
  The tone is not in the mask, but `audio.source = 1` always works.
- A saved source that is not in the mask becomes `off` at start; a ring source whose owner has not
  initialised yet waits for the ring.
- An unavailable source is refused before the transition begins — what is playing does not go
  quiet. Switching goes through the gates: the old source plays until both gates are closed, and
  only in silence does `apply_source()` switch the routes and reset the chain. Selecting the same
  source again before the transition ends cancels it.
- Who switches `audio.source` on their own: radio, DLNA, the player, AirPlay — when they are given
  something to play; USB — when the computer started playing and the panel did not (section "USB
  audio"). Bluetooth does not take the source on its own.

### Test tone

| Key | Value |
|---|---|
| `audio.tone_mode` | 0 — through the DSP, −12 dB relative to the ceiling (`TONE_LEVEL`); 1 — bypassing the DSP and the volume, exact DAC level; 2 — the same in 250 ms bursts out of every second, 5 ms edges |
| `audio.tone_hz` | 20…20,000 Hz, default 1000; continuous phase |
| `audio.tone_db10` | level of modes 1–2, tenths of dBFS of the DAC's full scale, −600…0, default −260 |
| `audio.tone_ch` | 1 left, 2 right, 3 both |

- Modes 1–2 switch off by themselves after 60 s (source → `off`, mode → 0); the tone through the
  DSP after 120 s without any change. A mode change goes through a closed gate.
- `audio.cal_vol_cap` (volume ceiling, %) and `audio.cal_port_off` (port off) apply only while the
  tone plays; they are not saved and are reset when the source stops being the tone.

## Edges of sound

- Two gates with the same smootherstep curve, opening 20 ms, closing 100 ms at any rate
  (`components/audio_dsp/include/fade_gate.h`): the envelope before the chain and the output gate
  before the DAC. Closing is longer because a 20 ms cut on loud bass is audible.
- The envelope closes on pause, flush, a deferred retune, a new BT format, a source switch,
  amplifier settling. The chain keeps running on silence, and the output gate closes over what is
  still ringing in it (subsonic, shelves, resonant band, the bass limiter's crossover); the chain is
  reset only behind a closed output gate. A retune and BT format adoption also wait for a closed
  output gate.
- A partial block from the ring: what arrived fades out with a 2 ms smoothstep tail (no longer
  than the block). An empty block in the middle of sound: the last sample is brought down to
  silence with the same tail (`fade_from_hold`).
- `amp_enable` follows the source, not the signal: it turns on when a source ≠ `off` is selected,
  into silence — the gates stay closed for 300 ms after the task starts and 100 ms after the
  turn-on or a retune (by `esp_timer`). On a transition to `off` it turns off only after the TX DMA
  has played silence through the whole queue behind a closed output gate (3840 + 240 frames, 85 ms
  at 48 kHz).
- Mute (`audio.mute`) is a 10 ms ramp of the input gain in the chain; it does not touch
  `amp_enable`.

## DSP chain

`audio_core::DspChain` (`components/audio_dsp/src/dsp_chain.cpp`), float, in blocks, no heap
after start. The order in `DspChain::process`:

1. volume leveler (only while it is on);
2. input gain: trim · volume · EQ headroom;
3. subsonic and the lift at the port;
4. loudness compensation;
5. "Bass" and "Treble";
6. PEQ, up to 10 bands;
7. width / mono bass (mid/side);
8. balance;
9. bass limiter;
10. brickwall.

Inside the chain **1.0 = the amplifier ceiling**, not the DAC's full scale (section "Amplifier
ceiling"). The filters are trapezoidal SVFs (Simper), one section per band for both channels
(`dsp_kernels.cpp`, explicit `fmaf`, `-ffp-contract=off`): a DF2 biquad in float32 is noisy at a
low band and a high fs. Design in double; peaks, shelves and notch are matched (Vicanek: the
magnitude response matches the analogue RBJ prototype at DC, Nyquist and f0), LPF/HPF bilinear, so
the EQ is the same at 44.1…96 kHz. Sections run two per pass. States < 1e-15 are zeroed every
block.

### Parameters

| Key | Range, default | Stage |
|---|---|---|
| `audio.volume` | 0…100 %, 30 | input gain (law below) |
| `audio.vol_max` | 40…100 %, 100 | "Maximum": plays `volume × vol_max / 100` of the same law |
| `audio.vol_start` | 0…100 %, 0 | after power-on the volume is no higher than this (0 — as it was) |
| `audio.mute` | bool | 10 ms ramp |
| `audio.leveler` | bool, false | "Volume leveling" (section below) |
| `audio.lev_sched` | bool, false | the leveler by the schedule |
| `audio.lev_from`, `audio.lev_to` | 0…1435 min of the day, step 5; 1320 (22:00), 420 (07:00) | the schedule's window |
| `audio.drive_db` | 0…6 dB, 0 | how far 100 % drives into the limiters above the clean ceiling |
| `audio.hp_hz` | 0 off / 20…80 Hz, 40 | subsonic, 4th-order Butterworth |
| `audio.spk_type` | 0 not set / 1 ported / 2 sealed | sealed — the port has no effect, whatever number is saved |
| `audio.port_hz` | 0 off / 30…80 Hz | the speakers' port frequency |
| `audio.port_lift_db` | 0…6 dB, 3 | peak at the port frequency, Q 1.4 |
| `audio.port_src` | 0 not set / 1 by hand / 3 measured | where the port frequency came from |
| `audio.loudness` | 0…100 %, 70 | loudness compensation strength, 0 — off |
| `audio.loud_ref` | 20…100 %, 100 | "Usual volume": where loudness compensation adds nothing |
| `audio.bass_db`, `audio.treble_db` | −6…+6 dB, 0 | shelves at 100 Hz and 10 kHz, Q 0.71 |
| `audio.eq_enabled` | bool, true | PEQ bypass (crossfade) |
| `audio.width` | 0…200 %, 100 | 0 mono, 100 as is, 200 double side |
| `audio.mono_bass_hz` | 0…300 Hz, 0 | 2nd-order Butterworth HPF on S; below 20 Hz — off |
| `audio.balance_db10` | −60…+60 (tenths of dB), 0 | > 0 — the left channel quieter, < 0 — the right |
| `audio.bass_xo_hz` | 0 off / 60…250 Hz, 120 | bass limiter crossover |
| `audio.rms_cap_db10` | −120…0 (tenths of dB), 0 off | bass power ceiling, dB below a full-scale sine in both channels |
| `audio.amp_clip_db10` | −240…0 (tenths of dBFS), −98 | amplifier ceiling C |

All of them are saved (persist). Shelves at 0 dB ("Bass", "Treble"), balance 0, loudness compensation 0 (and at the top of the scale), width 100,
the leveler off are a bit-exact "wire".

### Volume

- The law (`DspChain::volume_db`): from 50 % up −60·(1 − p/100) dB, i.e. 0.6 dB per step; below
  that the steps grow to ~1.4 dB, 1 % = −80 dB; 0 % is silence. 100 % = `audio.drive_db`: the
  source's full scale at 100 % is exactly the ceiling.
- The ramp is two one-pole filters in cascade, τ 5 ms each.
- "Maximum" compresses the scale rather than cutting it off: 100 % from any client is the limit,
  and the step gets finer (0.51 dB at 85 %). The saved volume is not rewritten.
- After power-on (and after `POST /api/restore`, which restarts the panel): if the saved volume is
  higher than `audio.vol_start` (≠ 0), the saved volume itself is lowered to it, so all clients
  show what is playing.
- **Headroom B** (`audio.eq_headroom_db`) is the loudest point of the total magnitude response
  (trim, PEQ, the "Bass" and "Treble" shelves, width above 1, subsonic with the port
  lift) over 20 Hz…min(20 kHz, 0.49·fs). The volume stops at drive − B: the top of the scale is
  flat, and below it EQ on/off differs only by the bands. With the bass limiter, B at each
  frequency counts only what the limiter does not take away there (LR4 fractions).
- `audio.volume` is written by: the panel (encoder — 2 % per click; `docs/UI.md`), the web and the
  API, DLNA RenderingControl, AirPlay (through a reference point, `docs/NETWORK.md`), the phone
  over AVRCP and the computer over USB (sections below).

### Volume leveling

For a film mixed quiet with loud advertising in its breaks (`components/audio_dsp/src/leveler.cpp`,
`audio_core::Leveler`). It acts on the source before the volume, so what it holds does not depend on
where the volume is.

- Loudness as ITU-R BS.1770 measures it: the K-weighting (its pre-filter shelf and the RLB
  high-pass, from the analogue prototypes, so at every rate; at 48 kHz within 0.05 dB of the
  standard's own filters), mean square of the mono sum (L + R)/2, in LUFS. Decorrelated content reads
  3 dB lower than the standard would make it.
- Two levels: over ~0.4 s (one-pole, BS.1770's momentary window is 400 ms) and over ~50 ms. The
  gain wanted is −20 LUFS minus the slow level, and no more than −12 LUFS minus the fast one (a bang
  after a quiet scene), within −20…+6 dB.
- Down: a one-pole on the gain in dB, τ 100 ms — a break 16 dB over the film is down by half in
  0.3 s and to within a dB in a second. Up: only after 1.5 s without a cut, then at 3 dB/s at most —
  back from that break in about 7 s.
- A 5 ms block under −60 LUFS is a pause: it is not measured, and the gain stays where it is.
- The gain is applied as a straight line across each block, decided by the block before.
- Off is a wire, bit-exact. Switched off while it holds a gain, it goes back to 0 dB at 3 dB/s
  first. A chain reset (source switch, new rate) restarts the measurement and keeps the gain.
- The current gain is `audio.lev_db`.
- **Schedule** (`AudioModule::leveler_schedule`, once a second): while `audio.lev_sched` is on and
  SNTP has set the clock, `audio.leveler` is switched on when the minute of the day enters
  [`lev_from`, `lev_to`) and off when it leaves; the window may cross midnight, equal ends are an
  empty window. When the schedule starts to apply (switched on, moved, the clock set at boot) it
  sets the switch to what it says now. Between the edges the switch on the Player is left alone.

### Subsonic and port

- While a port is set (30…80 Hz, speakers not sealed), the subsonic sits at 0.8 of the port
  frequency, regardless of `audio.hp_hz`; at the frequency itself, a peak of `audio.port_lift_db`.
  There the cone barely moves (the port does the work), so the lift is cheap; below it, protection.
  The peak counts toward B.
- The subsonic acts when its frequency is within 20…80 Hz; a change is crossfaded, like a band
  edit.
- The port frequency is measured as described in section "Speaker calibration: port frequency".

### Loudness compensation

- Two RBJ shelves fitted to ISO 226:2003 (`tools/eq/fit_loudness.py`): low 148.6 Hz Q 0.359,
  high 10 kHz Q 0.71.
- a = level(`loud_ref`) − level(volume), level(p) = min(volume_db(p), −B), a ≤ 50 dB.
  The shelves give 0.5605·a and 0.2253·a dB × strength. Deviation from the contours: up to 1.1 dB
  at a ≤ 30, 1.8 dB at 50.
- The shelves always lift less than the volume took away, so nothing comes out above what 100 %
  plays. The coefficients glide with the volume (τ 50 ms). The current bass lift is
  `audio.loud_db`.

### "Bass" and "Treble"

- Two dedicated sections after loudness compensation (`DspChain::set_tone`): a low shelf at 100 Hz
  and a high shelf at 10 kHz, Q 0.71, each within ±6 dB. They take none of the 10 PEQ bands and play
  with the EQ off as well; a change is crossfaded like a band edit; they count toward B.
- "Bass" and "Treble" go to the audio task as one `TIMBRE` command
  (`AudioModule::timbre()`: "Treble" in bits 0–7, "Bass" in 8–15).
- A shelf at 0 dB is a bit-exact wire.

### PEQ and `eq.json`

- Up to 10 bands: `peak`, `lowshelf`, `highshelf`, `lowpass`, `highpass`, `notch`; fields `type`,
  `f`, `gain`, `q`, `on`. Shelf Q ≤ 1.0 (above that a shelf overshoots its own gain).
- A band change is crossfaded over 20 ms: the old design plays out on its own copy of the state;
  an edit during the fade waits, and the last one wins. Filter state is reset only behind a closed
  output gate (pause, stop, source change, retune).
- The EQ document is `audio_core::EqStore` (`components/audio_dsp/include/eq_store.h`): `eq.json`
  (what plays) and `eq_custom.json` ("Custom") in `/userdata` through `UserStore`, with the factory
  `data/eq.json` as the fallback. Both files are read once at start; schema `version` 2, a file of a
  newer schema is not read (EQ flat). Before the first write over a file from an older build, a
  copy `<name>.v2` is left next to it; a file that cannot be read moves to `<name>.bad` on the
  first write.
- The editors are the panel's EQ page and the web (`GET`/`POST /api/eq`: `{"rev",bands}`,
  `{"rev",preset}`, `{"keep":true}`; an edit from a stale revision → 409). The document's owner is
  `AudioModule::own_eq()` on the main task: it passes the bands to the audio task through `EqBus`
  (seqlock, no file reads in real time) and publishes `audio.eq_rev`.
- Files are not written while the panel's EQ page is open (`ui.screen` = `eq`), nor earlier than
  60 s after the last edit from the web (or at once on `{"keep":true}`): a LittleFS write stops
  the cache of both cores, and the speakers click.
- Presets (`eq_presets.cpp`): the same 10 bands — a low shelf at 40 Hz, eight peaks 62.5…8000 Hz,
  a high shelf at 12 kHz. Eight are offered: "Custom", `flat`, `pop`, `rock`, `jazz`,
  `classical`, `electronic`, `hiphop`; the rest of the table stays for sets saved under them.
- The section trim (`gain_db`, −24…+12 dB) is part of the input gain and of headroom B.
- `audio.eq_reload` — redesign the bands from the current document.
- `eq.json` is divided into two output sections and modes (`mode`: `2.0` | `biamp` | `4.0` |
  `2.1`); the firmware plays section 0 in mode `2.0`. `audio.out_mode` is the mode from the file,
  `audio.out_caps` = 1 (stereo) — the layouts the firmware plays.

### Stereo and balance

- Width is the scale of S in mid/side; at 100 % the stage is skipped. Above ~140 % it boosts the
  out-of-phase content.
- Mono bass is an HPF on S: everything below the frequency becomes mono.
- Balance only attenuates one channel and takes no headroom; it glides like the volume.

### Limiters

- **Bass limiter** (`audio.bass_xo_hz`): an LR4 split. When the bass together with the rest would
  exceed −0.5 dB relative to the ceiling, only the bass is compressed — down to −12 dB, look-ahead
  ≈ 5 ms, hold 50 ms, release 200 ms. If the peak comes mostly from the upper band, the bass is
  left alone: the transition is linear between 30 and 70 % of the upper band's share of the peak,
  and the brickwall takes the rest. A crossover change goes through a 10 ms mute. The delay line
  is in PSRAM (without it the stage is disabled). `audio.rms_cap_db10` is a long bass power ceiling
  for a power supply that sags (averaging ~50 ms).
- **Brickwall**: look-ahead (M+1)·D frames, M = 9, D = 8 (≤ 50 kHz) or 16 (≤ 100 kHz) — 1.7–1.8 ms;
  a knot envelope with 40 ms hold and 250 ms release; by construction the output does not exceed
  the ceiling.
- The delays of both limiters are included in `PcmRing::downstream_frames()`.

### Protection

- A block in which a NaN/Inf has appeared (at the output or in a filter's state) is replaced with
  silence, the chain is reset, and `audio.dsp_faults` grows.
- The float → int32 conversion clips to ±1 and counts the clipped samples (`audio.out_clamps`,
  should be 0); NaN becomes silence, not full scale.

## Amplifier ceiling

- C is the loudest the amplifier reproduces cleanly, in DAC dBFS (`audio.amp_clip_db10`, limited
  to −24…0 dB). The limiter threshold T = min(C − 0.5, −1) dBFS; the output conversion multiplies
  by 10^(T/20), so the chain's 1.0 lands on T. `audio.ceiling_dbfs` shows T.
- Default C = −9.8 dBFS: TPA3255 at PVDD 24 V, gain 21.5 dB (SLASEA8A §7.5), PCM5102A 2.1 Vrms
  (SLAS859C) — this holds for the input circuit with one and with both inputs fed from the DAC. The
  limiter is then at −10.3 dBFS.
- `audio.amp_src`: 0 from the datasheet, 1 entered by hand, 2 found by ear, 3 measured. At start,
  a ceiling marked as from the datasheet that is not the datasheet value is re-marked as entered by
  hand; `audio.amp_cal` = (`amp_src` = 3).
- The web has no ceiling, `drive_db` or `rms_cap_db10` (`DROPPED` in `webui/src/lib/longtail.js`;
  `amp_src` has no widget); they are changed through the API (`POST /api/settings`, Basic auth —
  `docs/NETWORK.md`).

Measuring C (a multimeter on AC V, preferably a phone with a spectrum analyser). The tone of modes
1–2 is loud at steps 3–4 and switches off by itself after 60 s — then send it again.

```
curl -u <user>:<password> -X POST http://<panel>/api/settings \
     -d '{"audio.source":1,"audio.tone_mode":1,"audio.tone_hz":60,"audio.tone_db10":-260,"audio.tone_ch":1}'
```

0. PVDD at idle — the multimeter on DC V across the amplifier's power terminals.
1. Tone −26 dBFS, 60 Hz, left channel (the request above). AC V between the two terminals of the
   left speaker (BTL — neither to ground): V.
2. AC V from each terminal to the amplifier's ground: both ≈ V/2 → Vmax = 0.95·PVDD; one ≈ V, the
   other ≈ 0 (one half-bridge swings) → Vmax = 0.95·PVDD/2.
   C = −26 + 20·log10(Vmax / (1.414·V)). Repeat for the right (`tone_ch` 2), take the smaller.
   Example: PVDD 24 V, one half-bridge, V = 1.25 V → C = −9.8 dBFS.
3. Listening check: `tone_mode` 2, 100 Hz, both channels, level `10·(C − 1)` and `10·(C + 1)`. At
   C + 1 the sound is harsh (the 3rd harmonic at 300 Hz jumps up) or the module's CLIP lights; at
   C − 1 it is clean. Harsh at C − 1 as well → C − 0.5 and repeat; clean at C + 1 as well → C + 0.5
   (no higher than the calculation + 1.5 dB).
4. Power supply: `tone_mode` 1, 60 Hz, both, level `10·(C − 0.5)`, ~3 s, DC V on PVDD.
   A sag > 2 V → `audio.rms_cap_db10` = −30 (still > 2 V on heavy bass at 100 % — −40).
   Less → add 20·log10(PVDD under load / PVDD at idle) to C (the number is negative).
5. Write `audio.amp_clip_db10` = round(10·C), `audio.amp_src` = 3; `tone_mode` 0, the source as
   it was.

Without a multimeter, step 3 only: `tone_db10` from −160 in steps of +5 (0.5 dB) up to the first
harshness; C = that level in dB − 0.5 (±1 dB), `audio.amp_src` = 2.

## Speaker calibration: port frequency

For ported speakers. Web: the port calibration page (address `system/service/calibration/port`,
`webui/src/pages/service/CalibrationPage.svelte`) and the "Speakers" wizard
(`webui/src/pages/speakers/Wizard.svelte`). Only a person saves what was found ("Apply" /
"Accept" / "Save"): `audio.port_hz` (30…80), `audio.port_src` and `audio.spk_type` = 1.

**With the panel's microphone** (`audio.port_meas` = true, ~45 s; task `port_meter`, core 0):

1. The microphone is at the panel's top edge. That edge is 0.5–1 cm from the centre of one
   speaker's large driver, not touching; better with the grille off; the room quiet.
   `audio.port_meas_ch` — which speaker (1 left, 2 right).
2. The panel listens to the silence for ~4 s (source `off`), then plays a tone bypassing the DSP
   on one channel: the level is chosen at 90 Hz within −60…−30 dBFS, then a sweep in whole hertz
   over 20…100 Hz at ~1/24 octave (without 50 and 60 Hz — mains hum), a 0.4 s Hann window per
   point.
3. The near-field dip = the port frequency (`audio_core::find_port`). After the measurement the
   tone, its level and the source return to what they were (if someone selected another source,
   theirs stays).
4. `audio.port_meas_status` is a message from the web's catalog (`@cal.m_*`), `audio.port_meas_hz`
   what was found:
   - "Found: N Hz" — the dip is clear (≥ 12 dB above, ≥ 6 dB below, ≥ 15 dB over the noise), the
     web offers "Apply";
   - "Found N Hz — outside 30…80" — not applied;
   - "Doubtful, ~N Hz: check with a finger" — the dip is shallow or close to the noise; the
     frequency is shown, not applied;
   - "No dip: a closed speaker, or too far" — the speaker is sealed or the microphone is too far;
     "The microphone cannot hear the tone" — the panel is not at the selected speaker.
   The measured curve is in the log, lines `port meter, dBFS tone/noise …`.

**By hand**: a tone through the DSP with `audio.cal_port_off` and `audio.cal_vol_cap` = 20 (port
off, volume ≤ 20 % only while the tone plays). Frequency from 30 to 80 Hz in steps of 2–5 Hz;
where the large driver's cone nearly stands still and the port blows hardest is the port
frequency. A tone without changes switches off by itself after 120 s.

`audio.port_lift_db` after saving: 3 dB by default; boomy — 2, too little bass — 4–5. While a port
is set, `audio.hp_hz` has no effect.

**The finger check is final**: a tone from the phone over Bluetooth, the DSP neutral (port 0,
loudness compensation 0, EQ off), a finger on the large driver's dust cap, 55…75 Hz — where the
vibration is weakest. The microphone can be off by a few hertz: a room resonance at the port
frequency fills the dip and shifts it up, a rear port pulls it down.

## Bluetooth via the satellite

A2DP is received by a separate ESP32 board with its own firmware
([ModESP_BT_Satellite](https://github.com/Zapadenec1982/ModESP_BT_Satellite)). The sound goes over
the I2S line GPIO33/30/31, everything else over the UART link; the byte contract is
`docs/LINK_PROTOCOL.md`.

| Part | What it does |
|---|---|
| `modules/bt` | the link with the satellite: binding `uart_sat` (role `sat_link`, driver `uart_link`; J5, TX GPIO26, RX GPIO27, 460,800 baud), keys `bt.*`, `GET`/`POST /api/bt` |
| `components/sat_link` | COBS + CRC-16 frames, parsing and building messages (`sat_link.h`), the format transition machine (`format_sync.h`), publishing the format to the audio task (`stream_control.h`); no ESP-IDF, tested on the host |
| `modules/audio` | adopts the announced format, clocks the link or reads the slave port, the BT ring |

### Link

- The bytes are taken off the UART by task `sat_uart` (core 0, priority 8), which puts whole
  frames into a 6 KB ring; processing is on the main task.
- HELLO once a second until the satellite answers with its own; then PING every 2 s; the link is
  lost after 6 s without any frame. `bt.link`, `bt.sat` (the satellite's build), `bt.rtt_ms`;
  counters `bt.uart_ovf`, `bt.rx_drops`, `bt.seq_gaps`.
- Our capabilities in HELLO: `RELOCK_I2S`, `WIDE_SLOTS`, `FORMAT_READY`, `96KHZ`, `I2S_MASTER`
  always; `WANT_COVER` and streamed covers when their handler is ready; `PAIR_CONFIRM` while there
  is a screen to confirm pairing on. The satellite's capabilities are trimmed to its version
  (`sat::device_caps`). A different `proto_major` — the link is refused.

### Who clocks the link

| Satellite | Clock | Path on the panel |
|---|---|---|
| ≥ 1.9, HELLO with `DEV_CAP_I2S_SLAVE` | panel | I2S0 outputs its BCLK on GPIO33 (drive 2) and WS on GPIO30 (drive 3), DATA from GPIO31 goes into I2S0 RX; 32-bit slots, the output at the stream's rate with the board's MCLK; I2S1 parked and stopped; `bt.link_master` = true |
| < 1.9 | satellite | I2S1 slave RX on GPIO33/30/31; format from `STREAM_FMT`/`STREAM_PREPARE`, slot 16 or 32 |

- The panel drives GPIO33/30 only while Bluetooth is the selected source, the format was adopted
  in panel-clocked mode and the satellite's last HELLO carries `DEV_CAP_I2S_SLAVE`. A source
  change, loss of the link or a HELLO without the bit returns both pins to inputs (`release_link`,
  the outputs are disabled first).
- In panel-clocked mode the input and the output are on one clock: nothing to resample or trim;
  the drift between the phone and the panel is absorbed by the satellite (`docs/LINK_PROTOCOL.md`,
  "Who clocks I2S").
- Slave port (satellite < 1.9): held on XTAL; a format XTAL cannot clock (32-bit slots at
  44.1/48 kHz need a clock source above 45–49 MHz, and the crystal is 40 MHz; 96 kHz — any slots)
  moves it to APLL, and the master to the highest MCLK multiple out of 1280…512·fs at which APLL
  (2·MCLK) stays under 125 MHz: 1280·fs at 44.1/48 kHz, 640·fs at 96
  (`audio_core::i2s_plan_for_slave`, `components/audio_dsp/include/i2s_clock.h`). While the slave
  holds APLL, the output clock is tied to the stream's family.
- On leaving BT the slave port is parked on XTAL at the same slot width (48 kHz for 16 bits,
  32 kHz for 32): a width change reallocates the DMA descriptors, and after Wi-Fi that memory may
  no longer be there.

### Format

- A satellite ≥ 1.3 announces the format with `STREAM_PREPARE` and waits for `STREAM_READY`; an
  older one with `STREAM_FMT`. The `bt` module publishes the request in `sat::stream_control()` and
  in `bt.rate_hz`, `bt.bits`.
- The audio task with the BT source sees a new revision (or a change of who clocks): it closes the
  envelope over the sound of the old format, waits for a closed output gate and the reader's pause,
  adopts the format (`adopt_bt_format_master` or `adopt_bt_format`) and reports the result. The
  `bt` module answers `STREAM_READY` (ready / refusal) and repeats the answer every 100 ms as long
  as `STREAM_PREPARE` has arrived within the last second.
- While BT is not the selected source, the format is not adopted and `STREAM_READY` is not sent —
  the satellite holds silence. Leaving BT cancels the readiness; selecting it again adopts the
  format anew.
- Panel-clocked mode accepts only 32-bit slots. A format the board cannot clock — silence and one
  line in the log. Until there is a format — silence (`BT: no STREAM_FMT yet`): a guessed width
  sounds as noise.
- The rates are the satellite's business: 44.1 and 48 kHz; 96 kHz only with LHDC V5 and our
  `HOST_CAP_96KHZ`. Codec — `bt.codec` (SBC, AAC, aptX, aptX HD, LHDC V5).

### Playback

- The reader task `audio_bt` (core 1, priority 23) reads 240 frames at a time (10 ms timeout) from
  I2S0 RX in panel-clocked mode or from I2S1, converts them to float and writes them into a PSRAM
  ring of 1 s at 48 kHz (48,000 frames; at 96 kHz, 0.5 s). Frames with no room are discarded
  (`dropped` in the log).
- After format adoption and whenever the ring runs empty — a pre-buffer of up to 120 ms
  (`BT_TARGET_MS`) in frames of the current rate, in silence. A queue above 500 ms (no more than
  90 % of the ring) — the tail is moved to the target once.
- **The default build, `MODESP_AUDIO_BT_DIRECT_CONTINUOUS=ON`** (`modules/audio/CMakeLists.txt`):
  the samples from the ring go into the DSP unchanged — no ASRC, no clock servo,
  `audio.bt_clock_follow` has no effect. Stream rate ≠ output rate → silence until the next format
  handshake. Running empty → the envelope closes, pre-buffer, then sound; there is no time limit.
  With a satellite < 1.9 (two crystals) this gives a re-buffering once every few minutes.
- **`-D MODESP_AUDIO_BT_DIRECT_CONTINUOUS=OFF`** — the path for a satellite < 1.9. The path is
  chosen once, on the pre-buffer in silence, and does not change in the middle of sound:
  - **clock servo**, if `audio.bt_clock_follow` (default true), the rates are equal and the port
    can trim: the output APLL is adjusted in proportion to the queue error, a step every 250 ms,
    no more than ±100 ppm, hysteresis 2 ppm; the samples are bit for bit the ones that arrived.
    Two underruns, or 8 s at the trim limit ≤ −95 ppm with a falling queue → ASRC from the next
    pre-buffer;
  - **ASRC** otherwise: `components/audio_resampler`, 48 taps × 48 filters, Blackman-Harris, LPF
    at 0.98 of the source's Nyquist; ratio = output rate / announced rate, adjusted by the ring's
    fill: ±500 ppm within ±40 ms of the target, smoothly up to ±2 % at a deviation ≥ 100 ms.
    `audio.asrc_ppm`, `audio.in_rate`.
- Diagnostic builds — section "Diagnostics".

### Control from the panel and the phone

- Volume: the satellite is an AVRCP absolute volume target and does not attenuate by itself.
  `VOLUME_STATE` with `source` = 1 (a move on the phone) becomes `audio.volume` (0…127 →
  0…100 %) only when BT is the selected source and 8 s have passed since the phone connected; a
  level that arrives with the connection gets the room's volume in reply
  (`modules/bt/include/bt_volume_gate.h`). A change of `audio.volume` on the panel goes to the
  phone (`CMD_VOLUME`) while the link is up.
- Transport: `bt.cmd` 1 play, 2 pause, 3 stop, 4 next, 5 previous, 6 toggle (by
  `bt.play_state`).
- While another source is selected and the phone is streaming, the module sends PAUSE (no more
  often than once every 3 s). Returning to BT lifts only a pause the module set, and only if the
  phone was playing then. The phone reports playing, but there has been no stream for 5 s → one
  PLAY, so that the satellite resumes a suspended stream (`bt_stall_kick.h`); a pause a person
  asked for is not cancelled.
- Metadata: `bt.state`, `bt.peer`, `bt.title`, `bt.artist`, `bt.album`, `bt.pos_s`, `bt.dur_s`,
  `bt.play_state`; the cover — `modules/bt/src/bt_cover.cpp` (`docs/LINK_PROTOCOL.md`,
  "Cover art"). Disconnecting the phone clears the track and the format.
- Settings and pairing (a satellite with `DEV_CAP_BT_SETTINGS`): `GET`/`POST /api/bt`,
  `bt.cfg_rev`, `bt.pair_*`. The satellite's first name becomes the panel's name if that is not set
  yet; after that a satellite with a different name is given the panel's name
  (`modules/bt/include/device_name.h`, `docs/NETWORK.md`).

## USB audio

Module `modules/usb_audio`, component `components/usb_uac` (TinyUSB): the panel is a USB Audio
Class 2 device on the HS port. The port's hardware limits: `docs/CONCEPT.md`, "USB".

- 44.1 / 48 / 88.2 / 96 kHz, 16 or 24 bits, asynchronous with a feedback endpoint: the panel's
  clock sets the pace, there is no resampling. Device name — the panel's name, serial number — the
  MAC.
- The producer task (core 1, priority 23) drains the FIFO every 1 ms. While the source is `usb`:
  every new stream → flush of what is in the ring → a new session (`request_new_session`) and
  `ask_rate` → writing as `OWNER_USB`; a rate the clock did not take — silence until the next
  stream. While USB is not the source, the stream is read out, measured and discarded.
- Feedback holds the fill (ring + FIFO) at the consumer's cushion + 20 ms: correction
  (target − fill) / fs, mean fill τ 64 ms, limit ±0.5 %.
- The computer started playing (peak above −60 dBFS within the last 300 ms) and the selected
  source is not playing → `audio.source` = 7. Host mute → zeros.
- The host volume (−80…0 dB, spread evenly over 0…100) goes into `audio.volume` through a reference
  point (`modules/usb_audio/include/usb_volume.h`): at the start of a stream the room keeps its
  level; for 3 s after the start and while USB is not the source the level only sets the
  reference; after that piecewise-linear (the top of the slider is the top of the panel, the bottom
  is silence); a move of the knob or the web becomes the new reference.
- The controller's interrupt and both tasks are on core 1 (`usb_audio.core`, 0 or 1, after a
  restart): the interrupt comes every microframe, and a delay over 125 µs loses a packet. The
  TinyUSB task — priority 20, control requests only.
- Keys: `usb_audio.state` (off | waiting | ready | playing), `usb_audio.rate`, `usb_audio.bits`,
  `usb_audio.diag` (phase, frames, fill, feedback, packets, losses, level, tone, gaps),
  `usb_audio.losses` (the latest packet losses).

## Spectrum

- `audio_core::SpectrumAnalyzer`: 28 bands 40 Hz…16 kHz, a 1024-point FFT of the mono sum with a
  Hann window, hop 512; at 88.2/96 kHz the mono sum is averaged in pairs, so the FFT works on the
  equivalent of 44.1/48 kHz. The signal is taken after the chain, and the volume cut is added back;
  the limiter's reduction is not.
- It runs only when the source ≠ `off` and the panel shows a screen with the spectrum strip
  (`ui.screen` = `now`, `radio` or `standby`); otherwise it publishes one frame at the floor and
  stays quiet.

## Diagnostics

| Key | Normal | If not |
|---|---|---|
| `audio.underruns` | does not grow during sound | the producer cannot keep up, or the BT input ran empty |
| `audio.lim_gr` | 0 at normal volume; a few dB at 100 % with a loud master | constantly > 3 dB — drive or C set too high |
| `audio.bass_gr` | 0…6 dB on loud bass at 100 % | constantly > 10 dB — the EQ's bass boost is too much for PVDD |
| `audio.out_clamps` | 0 | an error in the chain |
| `audio.dsp_faults` | 0 | NaN/Inf in the chain |
| `audio.dsp_pct` | ≤ 35 % (the web's warning threshold) | DSP takes too much of the core |
| `audio.dsp_max_pct` | ≤ 80 % (the web's threshold) | above 100 % — the worst block did not fit its time |

- The web's thresholds: `webui/src/lib/service.js`. `audio.dsp_pct` — DSP cycles (with commands)
  in % of one core over 0.5 s; `audio.dsp_max_pct` — the worst block in % of the block's time.
  `audio.lim_gr` and `audio.bass_gr` — the deepest reduction by the brickwall and by the bass
  limiter over the same 0.5 s.
- Other statuses (every 0.5 s): `audio.in_l`/`in_r` — the source's peak before processing;
  `audio.level_l`/`level_r` — the peak after the chain; `audio.cpu_pct` — block processing time /
  real time; `audio.eq_headroom_db`, `audio.loud_db`, `audio.vol_eff`; for BT — `audio.in_rate`
  (the measured input rate), `audio.asrc_ppm`.
- The log (tags `Audio`, `I2sStd`, `Bt` stay at INFO in the quiet-log build,
  `components/modesp_services/src/log_policy.cpp`):
  - start: `Initialized: main … heap DMA N`, `amplifier ceiling … -> limiter at …`, `bass (boot): …`;
  - `amp ON/OFF`, `output retuned to N Hz`, `N Hz plays at M: the output stops at 96000`;
  - faults: `TX DMA ran dry`, `audio task late`, `underruns`, `i2s refused N blocks in full`,
    `output: N sample(s) clamped`, `DSP: N block(s) replaced by silence`, `limiter −x dB`,
    `bass limiter −x dB`;
  - load: `DSP load over 30 s: … at N Hz` (once per 30 s of sound);
  - ring: `PCM start #…` (the first second after opening, input and output), `PCM edge #…`
    (the largest jump at the gates' edges);
  - Bluetooth: `BT path: …` (the chosen path), `link clock: …`, `clocking the link: BCLK out on GPIO33,
    WS out on GPIO30, data in on GPIO31`, every 5 s `BT signal: …` (levels, rates, queue in % and
    ms, short/dropped/primes/resyncs), `BT I2S totals`, `BT RX DMA`, `BT window`.
- Diagnostic builds (`modules/audio/CMakeLists.txt`; all but the first require
  `-D MODESP_AUDIO_BT_DIRECT_CONTINUOUS=OFF`, incompatible combinations are a CMake error):

  | Option | What it does |
  |---|---|
  | `MODESP_AUDIO_DIAGNOSTIC_FIXED_TRIM` | disables the live trim of the output clock (in the default build it is already off) |
  | `MODESP_AUDIO_DIAGNOSTIC_BT_DIRECT` | BT without ASRC at equal rates, fixed trim, no longer than 60 s |
  | `MODESP_AUDIO_DIAGNOSTIC_BT_FIXED_RATIO` | BT through ASRC with ratio 1, fixed trim, no longer than 60 s |
  | `MODESP_AUDIO_DIAGNOSTIC_CONSTANT` | only the word 256 to the DAC, music off; clock and format policy as usual |

  The BT_DIRECT and BT_FIXED_RATIO modes go quiet on the first loss (underrun, a moved tail,
  discarded frames, RX/TX DMA overflow, a short ASRC block, trim) or after 60 s and write the reason
  into `BT path`; they re-arm on an OFF/BT transition or a new format. Host check:
  `python tools/tests/test_bt_diagnostic.py` (the normal, both diagnostic and the continuous modes).
- Path check: `audio.source` = 1, `audio.volume` ≈ 30 → a 1 kHz sine at the PCM5102A output; an
  oscilloscope on JP1: BCLK = 64·fs (3.072 MHz at 48 kHz), WS = fs; MCLK (GPIO49) is held low with
  the test tone — as it should be. Bluetooth: `bt.link` = true, `bt.link_master` = true with a
  satellite ≥ 1.9, in the log `BT path: we clock the link at N Hz / 32-bit` and `BT signal` lines
  with a non-zero input level.
- The on-board ES8311 and NS4150 are not used for playback. The ES8311's ADC is the microphone
  input for the port measurement and diagnostic recording (`docs/MIC_CAPTURE.md`); when it opens
  the microphone, `mic_input` holds PA_CTRL (GPIO11) low.
