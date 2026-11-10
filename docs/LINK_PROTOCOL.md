**English** · [Українська](LINK_PROTOCOL.uk.md)

# Device–host link protocol

<!--
  The shared contract of two firmwares. The document is written in English and Ukrainian, and each
  language exists in two copies: docs/LINK_PROTOCOL.md (English) and docs/LINK_PROTOCOL.uk.md
  (Ukrainian) in the player repository (host, ESP32-P4), docs/PROTOCOL.md (English) and
  docs/PROTOCOL.uk.md (Ukrainian) in the satellite repository (device, ESP32). The copies of one
  language are identical word for word except the first line, whose link points at the other
  language's file in its own repository. Both languages say the same thing with the same
  structure. Neither side depends on whether the other's tree is on disk.
  tools/check_protocol_mirror.py in the player repository compares the copies together with the
  shared headers; a CRLF/LF difference does not count, any other does.

  Edit all four copies at once. A change to what goes over the wire raises proto_minor; an
  editorial edit that does not change the wire does not. A mismatch between copies is a broken
  link, not a formatting nit.

  Implementations: device — components/satlink and main/link.c, tests tests/host/;
  host — components/sat_link and modules/bt, tests tests/host/test_sat_link.cpp.
  Shared files, identical in both repositories: the headers format_sync.h and cover_wire.h
  (components/satlink/include/satlink/ in the satellite, components/sat_link/include/sat_link/ in
  the player) and tests/host/format_sync_cases.h.
-->

Version **1.10**. The frame, version and capability negotiation, every message with its layout and
rules, audio format transitions, queues, and compatibility between versions.

The **device** (D) is an ESP32 board that receives audio from a phone over Bluetooth. The **host**
(H) is another microcontroller that plays that audio and shows the state. Two channels connect
them:

- **I2S** — audio, D → H only. Lines BCLK, WS, DATA, Philips I2S format, stereo, no MCLK. The
  device drives DATA; BCLK and WS are driven by whichever side clocks the link (3.5).
- **UART** — everything else, both ways. This document is about it, and about the rules that tie
  it to I2S: format announcements and which side clocks.

Four principles from which the rest follows:

1. **Audio matters more than telemetry.** No work on the link may stop or break the I2S
   stream.
2. **Nobody blocks on the link.** If the other side is not reading, telemetry is dropped, not
   kept waiting.
3. **A silent host is not a failure.** No `PONG` — the device keeps playing and keeps
   accumulating state. The exception is a format transition awaiting acknowledgement: nobody
   completes it without the acknowledgement.
4. **Different versions are the normal case.** The boards are flashed separately, so every pair of
   versions is defined, and a new capability bit takes effect only when both sides have announced
   it.

---

## 1. Physical layer

- UART **460800 baud, 8N1**, no hardware flow control.
- The device's TX is connected to the host's RX, and vice versa. Pin numbers are up to each
  board.
- The receiver must have buffer headroom and a separate overflow counter: without flow control,
  bytes the receiver has not picked up in time are lost together with their frames. The device
  holds 4 × 1048 bytes (about 90 ms) and gives its receive task a higher priority than the audio
  work. The host holds 8192 bytes in the driver (about 180 ms) and reads them with a separate task
  (section 13).

---

## 2. Frame

```
0x00 | COBS( type:u8 | seq:u8 | payload… | crc16:u16 ) | 0x00
```

- **`0x00` is the delimiter.** By construction of COBS there are no zeros inside a frame, so the
  receiver synchronises on the very first delimiter after power-up.
- **The leading `0x00` is optional:** back-to-back frames share one delimiter. Both sides send
  both delimiters. The receiver ignores empty frames between delimiters.
- **Sizes.** A decoded frame (`type | seq | payload | crc16`) is **at most 1040 bytes**, so the
  payload is up to **1036**. The receive buffer on the wire is **1048 bytes**, and that is not the
  same number: in the worst case COBS adds `ceil(n / 254)` bytes, i.e. 1040 + 5 = 1045, plus two
  delimiters — 1047, rounded up to 1048. Both implementations allocate exactly 1048.
- **An oversize frame** is discarded by the receiver up to the next `0x00`, and the receiver **does
  not lose synchronisation**. That is what COBS is here for.
- **`crc16`** — CRC-16/CCITT-FALSE: polynomial `0x1021`, initial `0xFFFF`, no reflection and no
  final XOR, over `type | seq | payload` before encoding; little-endian on the wire. A frame with a
  bad CRC is discarded.
- **`seq`** — `u8`, wraps around, incremented separately in each direction with every frame sent.
  The receiver counts gaps but does not ask for repeats: the protocol has no retransmission.

### Field types

| Notation | Meaning |
|---|---|
| `u8`, `u16`, `u32` | unsigned integers, **little-endian** |
| `str` | `u16 len`, then `len` bytes of UTF-8 with no terminating zero. An empty string is `len = 0` and means "unknown" |
| `u8 x[n]` | `n` bytes as is |

### What the receiver does with what it does not understand

- **An unknown type** is ignored; the device also counts it. So a new type never breaks the older
  side.
- **A payload shorter than the layout** is discarded entirely.
- **Extra bytes at the end of a payload.** The device discards such a payload entirely and counts
  it: extra bytes mean the sides disagree on the layout. The host checks the exact length only in
  `STREAM_PREPARE`, `COVER_RESET` and `COVER_END` and discards such a frame; in other messages it
  ignores the extra bytes.
- **Out-of-range values** — per the rules of the specific message below.
- There is no NACK for any frame: the protocol has no request–response pairs with guaranteed
  delivery.

---

## 3. Negotiation: `HELLO`

### `0x01` HELLO, both directions

```
u8   proto_major      // 1
u8   proto_minor      // the version of this document the sender follows
u32  caps             // the sender's capability bits, specific to each side (below)
str  build            // the sender's build, e.g. "sat 0.1.0 v5.5.5"; for humans only
```

### When it is sent

- **Each side sends `HELLO` after start-up and in reply to the other's `HELLO`.** The host sends
  its first one as its link comes up at start-up. After the exchange both work at the lower `proto_minor` of the two.
- **The device repeats its `HELLO` once a second** until it hears the host's `HELLO`. That way a
  lost greeting does not leave a freshly restarted device in the old mode. `PING`/`PONG` getting
  through does not yet mean that capabilities have been exchanged.
- **The host repeats its `HELLO` once a second while the link is not up**: until the first device
  `HELLO` with the same `proto_major`, and after the link is lost (section 4). The host's first
  greeting could land in the device's one-second window (below) and go unanswered, and without the
  device's capabilities, bit 8 in particular, the host does not clock I2S. The device answers a
  repeat that arrives after the window.
- **The host sends `HELLO` outside the rate limit** when the link has just come up and when its
  own `caps` have changed (bit 7, section 9). It sends its other replies to the device's `HELLO` no
  more than once a second.
- **All device `HELLO`s — the start-up one, repeats and replies — go no more than once a second.**
  A host `HELLO` that arrives less than a second after the device's previous `HELLO` gets no
  separate reply: the host already has a fresh device `HELLO`, and if the host missed it, it will
  get a reply to its repeat. The exception is when the device's own `caps` have changed
  (section 3.4): then the reply goes immediately.
- **The device ignores a `HELLO` of its own version with its own `build` string**, whatever `caps`
  it carries. That way a link looped back on itself does not take itself for a host.

### Different `proto_major`

Incompatible. The link does not come up.

- **The device** writes an error to `LOG`, from then on sends only `HELLO`, `PING`, `PONG` and
  `LOG`, and of what it receives handles only `HELLO`, `PING` and `PONG`; the rest it counts and
  discards.
- **The host** writes an error to its own log, repeats `HELLO` once a second and answers `PING`
  with `PONG`. It sends neither `PING` nor other messages.

In this state the host does not drive BCLK and WS (3.5), and the device announces no format to it,
so the host plays no Bluetooth audio (6.1).

### 3.1 Device capabilities

| Bit | Name | Since | Meaning |
|---|---|---|---|
| 0 | A2DP_SINK | 1.0 | receives A2DP |
| 1 | AVRCP_META | 1.0 | AVRCP metadata: `TRACK_META`, `TRACK_POS` |
| 2 | ABS_VOLUME | 1.0 | AVRCP absolute volume: `VOLUME_STATE`, `CMD_VOLUME` |
| 3 | COVER_ART | 1.0 | cover art |
| 4 | — | | reserved, always 0 (section 11) |
| 5 | — | | reserved, always 0 (section 11) |
| 6 | COVER_RETRY | 1.7 | executes `COVER_RETRY`. Only together with bit 3 |
| 7 | BT_SETTINGS | 1.8 | sends `0x1A`–`0x1D` and understands `0x25`–`0x28` |
| 8 | I2S_SLAVE | 1.9 | the device's I2S is slave right now: drives only DATA, takes BCLK and WS from the host. **Only while this is true** |

Device bit 6 and host bit 6 sit in different words and mean different things; the matching
numbers are a coincidence.

### 3.2 Host capabilities

| Bit | Name | Since | Requires | Meaning |
|---|---|---|---|---|
| 0 | RELOCK | 1.0 | | can reconfigure I2S reception on the fly on a format announcement |
| 1 | COVER | 1.0 | | wants cover art (section 10) |
| 2 | — | | | reserved: the host does not set it, the device ignores it (section 11) |
| 3 | WIDE | 1.1 | | accepts 32-bit slots |
| 4 | FORMAT_READY | 1.3 | | acknowledges format transitions (`STREAM_READY`) |
| 5 | 96KHZ | 1.4 | 0, 4 | accepts 96 kHz in 16- or 32-bit stereo slots |
| 6 | COVER_STREAM | 1.6 | 1 | accepts streamed cover art with `COVER_END` |
| 7 | PAIR_CONFIRM | 1.8 | | shows the pairing request and answers with `PAIR_REPLY` |
| 8 | I2S_MASTER | 1.9 | 0, 3 | the host clocks the link: BCLK and WS at the announced rate, 32-bit slots |

### 3.3 How the device reads the host's capabilities

A bit takes effect only if the host has announced the version in which the bit appeared, and the
bits it requires. The device clears the rest before deciding anything, in this order:

| Bit | Cleared if |
|---|---|
| 4 FORMAT_READY | `proto_minor` < 3 |
| 5 96KHZ | `proto_minor` < 4, or bit 4 (after the previous row) is zero, or bit 0 is zero |
| 1 COVER | `proto_minor` < 5 |
| 6 COVER_STREAM | `proto_minor` < 6, or bit 1 (after the previous row) is zero |
| 7 PAIR_CONFIRM | `proto_minor` < 8 |
| 8 I2S_MASTER | `proto_minor` < 9, or bit 0 is zero, or bit 3 is zero |

Bits 0 and 3 are never cleared. Bit 3 is itself the negotiation: a host that does not accept
32-bit slots does not set it at any version, so the device looks at the bit, not at the version.

The host likewise clears the device's bits when it receives its `HELLO`:

| Bit | Cleared if |
|---|---|
| 6 COVER_RETRY | `proto_minor` < 7, or bit 3 is zero |
| 7 BT_SETTINGS | `proto_minor` < 8 |
| 8 I2S_SLAVE | `proto_minor` < 9 |
| 4, 5 | always (section 11) |

### 3.4 What the device does on the host's `HELLO`

The device treats every host `HELLO` as a possible host restart and tells it everything again:

1. **I2S role** (1.9): becomes slave with host bit 8, or master without it. This is done **before**
   the reply, and if the role has changed, the reply with the changed bit 8 goes without the rate
   limit.
2. **`HELLO`** in reply, with the rate limit above.
3. **Format.** If there is a stream, the device recomputes the slot width. If nothing has changed,
   it repeats the current announcement with the same identifiers. If the width, the
   acknowledgement mode or the 96 kHz permission has changed, a new transition begins (section 6).
4. **Metadata and cover art.** The device re-reads the metadata from the phone, so `TRACK_META`
   will repeat. If cover art is negotiated, it starts the cover art from a **new** generation:
   `COVER_RESET` and a full transfer again, and a transfer in progress is cut off without
   `COVER_END`.
5. **Bluetooth settings** (to a host with `proto_minor` >= 8): `BT_CONFIG`, `BOND_LIST` and a
   repeat of the active `PAIR_REQUEST`.
6. **`BT_STATE`**, but no more than once every 10 s.

That is why a periodic `HELLO` from the host is expensive: each one restarts the cover art. The host
needs `HELLO` at start-up and after its own restart, and repeats of it only until the device's
`HELLO` arrives.

### 3.5 Who clocks I2S (1.9)

The host can clock the link: a frame the host counts out itself cannot be lost, whereas as a slave
at 96 kHz / 32 bits the host does not receive every frame the device's pins carry.

- **The device starts as slave** and drives only DATA. It drives nothing the host may drive, so
  until the first `HELLO` no line has two drivers.
- **Host `HELLO` with bit 8** → the device stays slave and replies with bit 8. **Without bit 8**
  (a host with `proto_minor` < 9 or without the capability) → the device becomes master and
  replies without bit 8.
- **The host drives BCLK and WS only while the device's last `HELLO` carries bit 8**, and stops as
  soon as a `HELLO` does not carry it or the link is gone.
- **Slots are always 32 bits** (`bits` = 32). The rate is the announced one; the host clocks
  exactly that, from the same clock as its own DAC.
- **The device absorbs the drift between the phone and the host.** It resamples to the host's
  clock, so the samples on the wire are no longer bit for bit what the decoder produced, and the
  low bits of the slot are not necessarily zeros.
- **A device the host has never said `HELLO` to stays slave**, and there is no audio on I2S until
  the host greets it.

---

## 4. Liveness: `PING` / `PONG`

### `0x02` PING, both directions

```
u32  tick_ms          // the sender's uptime, ms
```

### `0x03` PONG, both directions

```
u32  echo_ms          // tick_ms from the PING this answers
u32  own_ms           // the responder's uptime, ms
```

- **The device** sends `PING` **once a second**, regardless of `HELLO`, including to an
  incompatible host.
- **The host** sends `PING` **every 2 s** and only while the link is up, i.e. after a device
  `HELLO` with the same `proto_major`. Before such a `HELLO`, and to an incompatible device, the
  host sends no `PING`.
- `PING` is answered with `PONG` **immediately**, regardless of `HELLO`. Both sides do so.
- For the device a missing `PONG` is only a counter. It keeps playing, resets nothing and does
  not consider the host gone.
- **A host that has received no frame for 6 s considers the link lost**: it forgets the announced
  format, the device's capabilities, the Bluetooth settings and the cover art, stops driving BCLK
  and WS (3.5) and again repeats `HELLO` once a second. A live device sends `PING` every second, so
  6 s without a single frame means it is not there.
- Working `PING`/`PONG` proves the wire is alive, but not that the sides have exchanged `HELLO`.

---

## 5. Message list

Length is the payload, without `type`, `seq` and CRC; `s` is a string's length in bytes.
"Droppable" means it may be lost when the host cannot keep up reading (section 13).

| Code | Name | Direction | Since | Length | Droppable |
|---|---|---|---|---|---|
| `0x01` | HELLO | both | 1.0 | 8 + s | |
| `0x02` | PING | both | 1.0 | 4 | |
| `0x03` | PONG | both | 1.0 | 8 | |
| `0x10` | BT_STATE | D → H | 1.0 | 10 + s | |
| `0x11` | TRACK_META | D → H | 1.0 | 6 + s₁ + s₂ + s₃ | |
| `0x12` | TRACK_POS | D → H | 1.0 | 9 | yes |
| `0x13` | STREAM_FMT | D → H | 1.0 | 6 | |
| `0x14` | COVER | D → H | 1.0; not sent | 11 + len | yes |
| `0x15` | VOLUME_STATE | D → H | 1.0 | 2 | |
| `0x16` | STREAM_PREPARE | D → H | 1.3 | 14 | |
| `0x17` | COVER_RESET | D → H | 1.5 | 8 | |
| `0x18` | COVER_DATA | D → H | 1.5 | 16 + 1..1000 | yes |
| `0x19` | COVER_END | D → H | 1.6 | 16 | yes |
| `0x1A` | PAIR_REQUEST | D → H | 1.8 | 13 + s | |
| `0x1B` | PAIR_RESULT | D → H | 1.8 | 7 | |
| `0x1C` | BT_CONFIG | D → H | 1.8 | 7 + s | |
| `0x1D` | BOND_LIST | D → H | 1.8 | 2 + Σ(9 + s) | |
| `0x20` | CMD_TRANSPORT | H → D | 1.0 | 1 | |
| `0x21` | CMD_VOLUME | H → D | 1.0 | 1 | |
| `0x22` | CMD_RADIO | H → D | 1.0 | 3 | |
| `0x23` | STREAM_READY | H → D | 1.3 | 9 | |
| `0x24` | COVER_RETRY | H → D | 1.7 | 8 | |
| `0x25` | PAIR_REPLY | H → D | 1.8 | 7 | |
| `0x26` | BT_CONFIG_SET | H → D | 1.8 | 6 + s | |
| `0x27` | BOND_REMOVE | H → D | 1.8 | 6 | |
| `0x28` | BT_REFRESH | H → D | 1.8 | 0 | |
| `0x40` | LOG | D → H | 1.0 | 3 + s | yes |

A side sends a version 1.x message only when the other side has announced `proto_minor` >= x and
the bits the message requires. This is stated with each message below.

Codes `0x30`–`0x35` are reserved (section 11).

---

## 6. Audio format

### 6.1 The rule

**The device does not change the rate or the slot width without announcing them first.** The reason
does not matter: a new A2DP connection, a different codec, a change in the host's capabilities, a
change of I2S role, a restart of its own I2S. The phone negotiates the rate at the start of a
connection, and it **usually** holds until the end, but neither side may **rely** on that.

**The host may decline to play until the format is announced.** This is not an error but the only
safe behaviour: a guessed width sounds like noise, not like silence, and by ear it cannot be told
from a bad codec.

There are two announcements: the old `STREAM_FMT` with a 50 ms pause (6.3) and the acknowledged
`STREAM_PREPARE` / `STREAM_READY` (6.4). The second is used when the host has `proto_minor` >= 3
and bit 4, the first for everyone else.

### 6.2 Rate and slot width

**Rates.** Without host bit 0 the device offers the phone **only 44.1 kHz**: a host that does not
reconfigure reception on the fly will not survive a rate change in the middle of a connection. With
bit 0 — 44.1 and 48 kHz. 96 kHz — only LHDC V5 and only with bit 5 (and therefore with bits 0 and
4 and `proto_minor` >= 4).

The device decides what to offer when it registers its A2DP endpoints, and it registers them
**again only when no phone is connected**. So capabilities that arrive with a late `HELLO` take
effect from the next phone connection. After power-up the device waits up to 10 s for the host's
`HELLO` before registering the endpoints for the first time.

The rate says nothing about the recording itself: the phone takes the codec's default rate.

**Slot width** on the wire:

| Condition | Slot | How the samples sit |
|---|---|---|
| host clocks (bit 8, 1.9) | 32 | see 3.5 |
| device clocks, no bit 3 | 16 | 24-bit sources are narrowed with triangular dither |
| bit 3, no acknowledgements (no bit 4) | 32 for all codecs | in the high bits; low bits are zeros |
| bits 3 and 4 | the decoder's native width | in the high bits; low bits are zeros |

Native width: SBC and AAC — 16; aptX and aptX HD — 32 (24 significant bits); LHDC V5 — 16 for a
16-bit source and 32 for a 24-bit one. Bit 3 is a permission, not an order to widen everything.

**Narrowing uses dither, not dropping the low byte.** Dropping shifts every sample in one direction
and leaves an error correlated with the signal, which is audible as an artefact on quiet material.

**Channels — always 2.** All endpoints offer stereo only. The device does not announce a format
that is not stereo with a non-zero rate and a 16- or 32-bit slot, and audio stays off.

**What bit 3 costs the host when the device clocks the link.** 32-bit slots at 44.1 and 48 kHz
need an internal receiver clock of 22.6 and 24.6 MHz from a source twice as high, and the crystal is
40 MHz. So for the duration of such a stream (and of any 96 kHz stream) the host's input channel
switches to the APLL, and while it plays, **the host's output clock is tied to that stream's
family**: 96 and 44.1 kHz at the same time are impossible.

### 6.3 The old path: `STREAM_FMT` and 50 ms

#### `0x13` STREAM_FMT, D → H

```
u32  rate_hz          // 44100, 48000 or 96000
u8   bits             // slot width on the wire: 16 or 32
u8   channels         // 2
```

For a host without acknowledgements (no bit 4, or `proto_minor` < 3).

- **Before the first sample of every A2DP connection — and before any other format change.**
- After the announcement — **at least 50 ms of silence** on I2S, and only then samples of the new
  format. The count starts when the frame has physically left the UART, not when it was queued.
- The device repeats `STREAM_FMT` every 250 ms until the transition is complete, and on every host
  `HELLO`, because the host may have restarted and missed the announcement.
- The format is forgotten on disconnection: announcing a stale one is worse than announcing none.
- A host that has already received `STREAM_PREPARE` from the device ignores `STREAM_FMT` until the
  link is lost.

### 6.4 Acknowledged transition (1.3)

#### `0x16` STREAM_PREPARE, D → H

```
u32  boot_nonce       // non-zero, random on every device boot
u32  generation       // non-zero, grows with every transition and reconnection
u32  rate_hz
u8   bits             // 16 or 32
u8   channels         // 2
```

Exactly 14 bytes. Only to a host with `proto_minor` >= 3 **and** bit 4. Bit 3 still separately
permits 32-bit slots.

#### `0x23` STREAM_READY, H → D

```
u32  boot_nonce       // from STREAM_PREPARE
u32  generation       // from STREAM_PREPARE
u8   status           // 0 ready, 1 refused
```

Exactly 9 bytes. Any other length, an unknown `status` or zero identifiers make it invalid.

#### What the device does

1. **Closes the audio.** It closes the PCM producer, waits for the write already in progress,
   flushes its buffers, the remainder and the TX DMA. While it waits, I2S runs on the old clock and
   carries zeros; the clock does not stop.
2. **Sends `STREAM_PREPARE`**, and **only then** changes the I2S format: right after the frame has
   physically left the UART.
3. **Holds silence** until all of these are true: the local configuration succeeded, a
   `STREAM_READY` with the same `boot_nonce` and `generation` and status 0 has arrived, and at
   least 50 ms have passed since the physical send. Queuing does not start the timer, a repeat does
   not restart it.
4. **Repeats the same `STREAM_PREPARE` every 250 ms** while it waits. The queues stay bounded.
   **No timeout replaces `STREAM_READY`.**

#### What the host does

The host accepts `STREAM_PREPARE` only from a device with `proto_minor` >= 3 with which the link is
up, and publishes the request to the audio path at once and whole. If Bluetooth is the selected
source: fade out the buffered old audio, pause reception, wait for its last publication, discard the
old buffers and the ASRC history, apply the clock and slot plan and start reception, and only then
answer "ready"; if the format could not be applied, answer with a refusal. If another source is
selected, keep the request and defer the preparation and `STREAM_READY` until Bluetooth is
selected. Leaving Bluetooth cancels readiness; selecting it again requires preparation even for the
same request.

The host repeats its current answer every 100 ms while the device keeps repeating `STREAM_PREPARE`
(up to 1 s after the last one), to recover a lost one.

#### Matching rules

- An exact repeat of the same request (identifiers and format) is idempotent. The same identifier
  with a different format, or an older generation within the current boot, is a refusal, and the
  host sends it immediately.
- Audio is released only by a `STREAM_READY` that matches on both `boot_nonce` and `generation`.
  Old or repeated answers will not release a newer transition.
- **A refusal is final** for that transaction: audio stays off, and a later "ready" for the same
  identifier does not undo it.
- Disconnection erases the format state. A reconnection, even with the same rate and width, gets a
  new generation. When the generation overflows, the device takes a new non-zero `boot_nonce`.
- A host `HELLO` triggers a repeat announcement, so a restarted host learns the current format.
- A failed I2S configuration leaves silence. A driver failure on the device is final until restart:
  the driver may have freed the old DMA buffers without allocating new ones.
- An established stream survives the absence of `PING`/`PONG`, but a transition awaiting
  acknowledgement never falls back to "plain 50 ms".

### 6.5 96 kHz

Bit 5 is an explicit rate capability: bit 0 alone permits only 44.1 and 48. The device offers LHDC
V5 at 96 kHz only after a 1.4+ `HELLO` with bits 0, 4 and 5; a missing, older or incomplete `HELLO`
leaves the endpoint at 44.1/48. Transitions to 96 kHz go through `STREAM_PREPARE` / `STREAM_READY`
with the same 50 ms. If a 96 kHz stream is playing and a new host lacks this capability, audio stays
off and is not announced. LHDC V5 carries native 16-bit PCM or 24 significant bits in 32-bit
containers. The endpoint does not offer 192 kHz, LL modes, spatial processing or lossless.

---

## 7. Bluetooth state and track

### `0x10` BT_STATE, D → H

```
u8   state            // 0 off, 1 waiting, 2 discoverable for pairing,
                      // 3 connecting, 4 connected, 5 playing
u8   codec            // 0 none, 1 SBC, 2 AAC, 3 aptX, 4 aptX HD, 5 LHDC V5
u8   addr[6]          // the phone's BD_ADDR, zeros when not connected
str  name             // the phone's name, empty when unknown; up to 64 bytes
```

On events: A2DP connection state, stream start and stop, codec negotiated, successful
authentication, phone name read, a radio or discoverability change, and also after a host `HELLO`
(no more than once every 10 s). Not sent periodically.

### `0x11` TRACK_META, D → H

```
str  title            // each field up to 128 bytes
str  artist
str  album
```

On a track change. An empty string means unknown. The attributes arrive from the phone one by one,
so the device sends `TRACK_META` 300 ms after the last one. On disconnection — an empty one
immediately.

**The first `TRACK_META` after connecting is often empty**, and that is normal: the phone's first
answer to a metadata request right after connecting can be empty, and a later track-changed
notification fills the fields. So the device sends `TRACK_META` as soon as it has something to
send, even empty, and sends it **again** when the fields have filled. The host keeps it as "last
known" and does not take the first message as final.

### `0x12` TRACK_POS, D → H

```
u8   state            // 0 stopped, 1 playing, 2 paused, 3 fast forward, 4 rewind
u32  elapsed_ms
u32  total_ms         // 0 = unknown
```

- **Once a second, only while playing.** In pause and stop there are no periodic ones; the host
  keeps the last value.
- **Immediately on a state change** and on every position answer from the phone. So `state` 2 or 0
  does occur in `TRACK_POS` too.
- `TRACK_POS` is not sent on disconnection; the host learns of it from `BT_STATE`.

Where the position comes from depends on the phone, and the device tries the cheaper path first:

1. If the phone supports the `PLAY_POS` notification, the device registers for it (5 s step), sends
   `TRACK_POS` on every event, and between events, once a second, an interpolated position so the
   bar moves smoothly.
2. If not, it polls `GetPlayStatus` once a second and sends the position on the answer.

**`total_ms` is always sanitised**: phones even return a negative length. Anything negative or
longer than a day becomes 0, i.e. "unknown". The host applies the same limit (a day) to
`elapsed_ms` and `total_ms`.

### `0x15` VOLUME_STATE, D → H

```
u8   volume           // 0..127, AVRCP absolute volume
u8   source           // 0 changed from this side (the host's CMD_VOLUME),
                      // 1 changed by the phone
```

On every change. **The host applies the volume, not the device.** The device is an AVRCP absolute
volume target, so the phone sends the stream at full scale and only reports the number; the device
forwards it and does not scale the samples. Scaling on the device would lose resolution before the
host. The host acts only on `source` 1: `source` 0 is the echo of its own `CMD_VOLUME`.

The device does not repeat `VOLUME_STATE` on `HELLO` or on connection, so until the first change the
host does not know the volume. After start-up the device takes it as 127.

---

## 8. Control

### `0x20` CMD_TRANSPORT, H → D

```
u8   cmd              // 0 play, 1 pause, 2 stop, 3 next, 4 prev,
                      // 5 ffwd start, 6 ffwd stop, 7 rew start, 8 rew stop
```

Values above 8 are ignored. Without an AVRCP connection the command does nothing.

**A pause the phone ignores.** If the A2DP stream is still running 5 s after `pause` or `stop`, the
device suspends it itself (AVDTP SUSPEND). Any other command before the 5 s run out cancels this
check; a repeated `pause` does not restart the count. The phone cancels it too: when its stream
stops or when it reports that it is playing again (the person resumed playback on the phone). A
phone that ignores the pause does neither. After such a cancellation the next `pause` starts a new
count. The device restarts a stream it suspended itself (AVDTP START): on `play` from the host or
when the phone reports that it is playing again.

### `0x21` CMD_VOLUME, H → D

```
u8   volume           // 0..127
```

Anything above 127 is clamped to 127. The phone learns of it if it has registered for the volume
notification. The device always answers with `VOLUME_STATE` with `source` 0.

### `0x22` CMD_RADIO, H → D

```
u8   radio            // 0 turn off, 1 turn on
u8   discoverable     // 0 no, 1 yes
u8   disconnect       // 1 = drop the current connection
```

Any non-zero value means "yes". Takes effect immediately but is **not persisted**: only
`BT_CONFIG_SET` changes the stored `radio` and `discoverable` (section 9). The reply is
`BT_STATE`. The host sends `CMD_RADIO` only with `disconnect` = 1; `radio` and `discoverable` in it
are the last ones from `BT_CONFIG`, and for a device without bit 7 or before the first
`BT_CONFIG` — 1 and 1.

---

## 9. Bluetooth settings (1.8)

The host shows the Bluetooth settings and pairing requests; the device keeps and carries them out.
Two bits, both only with `proto_minor` >= 8:

- **device, bit 7 BT_SETTINGS** — sends `0x1A`–`0x1D` and understands `0x25`–`0x28`;
- **host, bit 7 PAIR_CONFIRM** — shows the pairing request and answers with `PAIR_REPLY`.

The device sends `0x1A`–`0x1D` only to a host with `proto_minor` >= 8. The host sends `0x25`–`0x28`
only to a device with `proto_minor` >= 8 and bit 7, and accepts `0x1A`–`0x1D` only from such a
device. A mixed pair breaks nothing: a 1.8+ host with a 1.7 device simply shows no settings, a 1.8+
device with a 1.7 host pairs without a request.

### `0x1A` PAIR_REQUEST, D → H

```
u8   addr[6]
u8   kind             // 0 numeric comparison: show the passkey, wait for PAIR_REPLY
                      // 1 consent only (Just Works): no code, wait for PAIR_REPLY
                      // 2 passkey to be entered on the phone: show it, no reply needed
u32  passkey          // 0..999999, show as six digits with leading zeros; 0 for kind 1
str  name             // the phone's name, up to 32 bytes of UTF-8; empty if it could not be read
```

Only to a host with bit 7. Without it the device asks nothing and accepts on its own. The host
discards a request with `kind` above 2 or `passkey` above 999999.

- **One request at a time.** The stack runs one pairing at any moment and does not let a second
  device in during an active one, so a second `PAIR_REQUEST` does not arrive before the first one's
  `PAIR_RESULT`.
- **25 s timeout for `kind` 0 and 1.** Without a `PAIR_REPLY` within 25 s the device rejects on its
  own and sends `PAIR_RESULT` with status 2. The stack's own timer is longer (35 s), so this one
  fires first.
- **`kind` 2 has no reply timeout.** The person types the code on the phone, and the result arrives
  when the phone finishes or the stack gives up.
- **The active request is repeated in reply to `HELLO`**, because the host may have restarted. A
  repeat with the same `addr`, `kind` and `passkey` is the same request, not a new one.
- **The host closes the request on its own** if `PAIR_RESULT` has not arrived within 90 s (`kind` 0
  and 1) or 120 s (`kind` 2), or the link is gone.

Which model the phone gets is decided by the stack, not by the host. With bit 7 the device announces
DisplayYesNo. Numeric comparison (`kind` 0) happens when the phone is DisplayYesNo too and at least
one side requires MITM protection. The device requires it when the phone starts dedicated bonding
(adding a device from the phone's settings), so for an ordinary phone this is the main case. The
rest is Just Works (`kind` 1), except keyboard devices (`kind` 2).

### `0x1B` PAIR_RESULT, D → H

```
u8   addr[6]
u8   status           // 0 paired
                      // 1 rejected by the host (PAIR_REPLY with accept = 0)
                      // 2 timeout: no PAIR_REPLY within 25 s (kind 0 and 1 only)
                      // 3 failed: cancelled on the phone, authentication error
                      //   or connection lost
```

Exactly one per `PAIR_REQUEST`. The host closes the dialog. Values from 4 up are reserved; the host
shows them as 3. If the stack stays silent for 60 s after consent, the device sends status 3 on its
own.

Cancellation on the phone has no status of its own: Bluedroid reports it with the same code as an
authentication error or an LMP timeout (`AUTH_FAILURE`).

### `0x1C` BT_CONFIG, D → H

```
u8   radio            // 0 off, 1 on
u8   discoverable     // 0 hidden, 1 visible
u8   auto_reconnect   // 0/1: after start-up, call the last phone
u8   codecs_supported // bits: 0 SBC, 1 AAC, 2 aptX, 3 aptX HD, 4 LHDC V5 — what the build has
u8   codecs_enabled   // what is offered to the phone now; a subset of codecs_supported;
                      // bit 0 always 1; a codec the build cannot withdraw stays 1
str  name             // the current device name, 1..32 bytes of UTF-8
```

Sent after a `HELLO` from a host with `proto_minor` >= 8 (followed by `BOND_LIST`), after every
`BT_CONFIG_SET` with what was actually applied, and on `BT_REFRESH`. The codec bits are a mask,
not the numbers from `BT_STATE`.

- **`radio` 0** — the device accepts no connections and is not visible in a scan. The current
  connection is dropped, there is no auto-reconnect.
- **`discoverable` 0** — the device is not visible in a scan **and accepts no new pairings**, so
  only phones already paired connect. Invisibility alone does not give this: anyone who knows the
  address could still pair. **1** — visible and accepts pairing.
- **Codecs take effect from the next connection.** The current connection is not dropped for them:
  the device re-registers the endpoints when no phone is present. The phone takes the best codec
  offered, and it cannot be asked for less; withdrawing the top codec is the only way to step the
  phone down. SBC cannot be withdrawn: A2DP requires it.

### `0x1D` BOND_LIST, D → H

```
u8   total            // how many bonds the device holds
u8   count            // how many in this frame, <= 16
count times:
  u8   addr[6]
  u8   flags          // bit 0 — connected now; bit 1 — last connected
                      // (the auto-reconnect target)
  str  name           // up to 32 bytes of UTF-8, cut at a character boundary; empty = unknown
```

One frame. The order is from the most recently connected. If `total` is greater than `count`, the
older bonds do not fit in the frame; `BOND_REMOVE` with `FF×6` clears them. 16 entries are
2 + 16 × 41 = 658 bytes, within 1036.

Sent after a `HELLO` from a host with `proto_minor` >= 8, after a phone is paired, removed,
connected or disconnected, when a name becomes known, and on `BT_REFRESH`. Bluedroid's bond storage
holds no names: the device keeps its own address → name table in NVS and fills it on pairing and
from the phone name it reads. A bond with no entry in that table gets its name after the first
connection.

### `0x25` PAIR_REPLY, H → D

```
u8   addr[6]
u8   accept           // 0 no, 1 yes; anything else is invalid
```

Only for the current request of `kind` 0 or 1. A different address, an expired or already closed
request, and `kind` 2 are ignored.

### `0x26` BT_CONFIG_SET, H → D

```
u8   radio
u8   discoverable
u8   auto_reconnect
u8   codecs_enabled   // the device enables SBC itself, even when bit 0 is cleared;
                      // bits outside codecs_supported are ignored
str  name             // empty = do not change
```

The full state, not a delta. In the `radio`, `discoverable` and `auto_reconnect` fields 0 means
"no", anything else "yes". The device stores the state in NVS, applies it and answers with
`BT_CONFIG`. A name longer than 32 bytes or not UTF-8 is not applied, and `BT_CONFIG` shows the old
one. A new name takes effect immediately, but the phone may keep showing the old one from its cache
for a while.

### `0x27` BOND_REMOVE, H → D

```
u8   addr[6]          // FF×6 = forget all
```

If that phone is connected, the connection is dropped. The reply is `BOND_LIST`, including when no
such address was among the bonds. When several bonds are removed, `BOND_LIST` may arrive several
times — one per removal; the last one is current.

### `0x28` BT_REFRESH, H → D

```
(empty)
```

The device sends `BT_CONFIG` and `BOND_LIST`.

### Behaviour

- **The pairing method follows the last `HELLO`.** With bit 7 — DisplayYesNo and requests to the
  host. Without it — NoInputNoOutput and consent without a request. A switch takes effect from the
  next pairing.
- **The host sets bit 7 only while it has a working screen** to confirm on, and announces the
  change with a new `HELLO` immediately (section 3).
- **The device keeps the settings**, in NVS. The host only shows them and sends `BT_CONFIG_SET`.
  Factory defaults: `radio` 1, `discoverable` 1, `auto_reconnect` 0, all codecs of the build, name
  `modesp satellite`.
- **Auto-reconnect.** After start-up, and also when `radio` becomes 1, the device calls the last
  connected phone. No more than three attempts, 10 s apart. The attempts stop as soon as any phone
  has connected.
- **Legacy PIN** (phones before Bluetooth 2.1) is not supported.

---

## 10. Cover art

### 10.1 How it works

The device fetches the cover art from the phone over AVRCP Cover Art (OBEX) and passes it to the
host as a JPEG tied to the track. Every image has a `boot_nonce` + `track_generation` pair, both
non-zero. `boot_nonce` is random on every device boot (and not the same as in `STREAM_PREPARE`),
`track_generation` grows with every new image.

The device sends cover art **only to a host with `proto_minor` >= 6 and bits 1 and 6** — that is,
only as a stream (10.3). A lower host gets neither `COVER_RESET`, nor `COVER_DATA`, nor the old
`COVER` (`0x14`): this is a floor on the sender's side, not a change to any layout. The device takes
only the linked thumbnail the phone provides.

### 10.2 `0x17` COVER_RESET, D → H (1.5)

```
u32  boot_nonce
u32  track_generation
```

Exactly 8 bytes, both non-zero. A new pair removes the Bluetooth cover art on display and cancels
assembly and decoding of the old image. A repeat of the same pair is idempotent. The host's cover
art from other sources is not affected.

Sent before an image is fetched: on AVRCP connect and disconnect, on a track change, when the image
handle changes or disappears, and after a host `HELLO` or an executed `COVER_RETRY`. No image, or
the phone cannot do it — the tile stays empty. `COVER_RESET` is not droppable and overtakes the old
image's data still in the queue; the pair keeps tracks from mixing.

### 10.3 `0x18` COVER_DATA, D → H (1.5; streamed since 1.6)

```
u32  boot_nonce
u32  track_generation
u32  total_bytes      // full size; 0 = unknown (stream, 1.6)
u32  offset           // offset of this chunk's first byte
u8   jpeg[...]        // 1..1000 bytes; length = payload length − 16
```

There is no inner length.

- **Stream (1.6), the way the device sends:** `total_bytes` = 0, because the size is unknown until
  the end of OBEX: the phone does not report it. Chunks of 1..1000 bytes, the size may vary,
  offsets contiguous from zero. `COVER_END` marks the end.
- **Known length (1.5):** `total_bytes` is the full size, the same in every chunk, every chunk
  except the last is exactly 1000 bytes, and the image is complete when the last byte has arrived.
  The host accepts this too. The two modes may not be mixed within one generation.

The host accepts only the current pair, offset 0 first, then contiguous offsets. Unknown and old
pairs are ignored. A gap, a repeat or a corrupted chunk of the current pair cancels assembly. The
limit is 256 KiB; the host holds no more than that. The host publishes a streamed image only on
`COVER_END`, a known-length image when its last byte has arrived.

### 10.4 `0x19` COVER_END, D → H (1.6)

```
u32  boot_nonce
u32  track_generation
u32  actual_bytes     // how many JPEG bytes were sent
u32  crc32            // CRC-32 over all the JPEG bytes in sequence
```

Exactly 16 bytes. CRC-32/ISO-HDLC: polynomial `0xEDB88320` (reflected), initial value and final XOR
`0xFFFFFFFF`; the check value for `123456789` is `0xCBF43926`, on the wire `26 39 F4 CB`.

`COVER_END` goes through the same queue as `COVER_DATA` and cannot overtake it. The device sends it
only after a successful OBEX final. The host publishes the image only if the pair, the length and
the CRC match, the length is within 3..262144, and there was no gap, repeat or error. No `COVER_END`
or chunks for 5 s in a row — timeout. A partial or corrupted image is never published, because half
a picture is worse than none.

The device aborts an image **without `COVER_END`** if the first bytes are not `FF D8 FF` (not a
JPEG), the size exceeded 256 KiB, the queue or the memory reserve does not allow a chunk to be
queued, the generation changed, or OBEX returned an error. The device does not hold chunks back:
when the queue is full, it aborts rather than waits.

### 10.5 `0x24` COVER_RETRY, H → D (1.7)

```
u32  boot_nonce       // the pair whose transfer failed
u32  track_generation
```

Exactly 8 bytes, both non-zero. Only to a device with `proto_minor` >= 7 and bit 6; otherwise it is
not sent at all, and there are no retries.

The host sends it when the transfer of that pair ended without a usable image: a gap, a repeat or a
corrupted chunk, a length or CRC mismatch at `COVER_END`, or 5 s of silence. It is a hint, not a
request with a reply: there is no reply, no NACK and no timeout. A host that never received anything
leaves the tile empty.

The device executes it only if the pair names its current `boot_nonce` and current generation.
Anything else is ignored: the phone has moved on, and the next `COVER_RESET` will bring a new image.

**Executing means a new generation.** The device re-reads the metadata, repeats the OBEX GET and
sends `COVER_RESET` with a **fresh** `track_generation`, then `COVER_DATA` from zero and
`COVER_END`, as always. The old generation may not be repeated: `COVER_RESET` with the same pair is
idempotent, so the host would clear nothing, and the repeat would land on the corrupted assembly.

Both sides limit retries, and neither relies on the other's limit. The host asks no more than twice
per image; the counter starts again when an image has completed or when a `COVER_RESET` arrives that
was not an answer to its request. The device executes no more than twice per track, counting from
the last AVRCP track change or the last successful transfer. Whichever limit is reached first ends
the chain, and the tile stays empty. A retry is meant for a single lost frame; a phone or link that
loses frames constantly is a different fault, and it must not be answered with unbounded OBEX.

Mixed pairs are defined: a host that does not see device bit 6 does not send `0x24`; a device that
receives no `0x24` makes no retries. Neither side treats the absence of a retry as an error or waits
for one.

### 10.6 `0x14` COVER, D → H (1.0; not sent)

```
u16  chunk            // from zero
u16  total            // how many chunks in all
u8   mime             // 0 unknown, 1 jpeg, 2 png
u32  total_len        // full image size
u16  len              // bytes in this chunk, up to 1024
u8   data[len]
```

The cover art of 1.0–1.4 devices. A 1.10 device does not send it, a 1.10 host discards it; the
layout stays for completeness. `mime` and `total_len` are repeated in every chunk so that each is
self-describing; the limit is 256 KB.

---

## 11. Reserved codes and bits

- Codes `0x30`–`0x35` are undefined and not reused. The device counts them as an unknown type and
  does nothing; the host does not send them and discards received ones as an unknown type.
- Device bits 4 and 5 are always 0; the host clears them for any device version (3.3).
- The host does not set host bit 2, and the device ignores it.

None of these codes and bits will get a new meaning.

---

## 12. `0x40` LOG, D → H

```
u8   level            // 0 error, 1 warn, 2 info, 3 debug
str  text
```

The device's log, for a human on the host side. Droppable. Sent to an incompatible host too.

---

## 13. Queues and dropping

The device never waits for the host. When the host cannot keep up reading, some messages are lost,
and which ones is defined:

- **Droppable:** `TRACK_POS`, `LOG`, `COVER`, `COVER_DATA`, `COVER_END`. They go through separate
  lower-priority queues and are the first to be lost under pressure. The host does not rely on
  them: the next position will update it, and an image without `COVER_END` is not published.
- **The rest is not dropped**, including `BT_STATE`, `STREAM_FMT` / `STREAM_PREPARE` and
  `COVER_RESET`. They take priority over the droppable ones.
- **Cover art displaces nothing.** A chunk for which there is no room in the queue or in the memory
  reserve aborts the image; a retry is the business of `COVER_RETRY`.
- **To an incompatible host** (a different `proto_major`) the device sends only `HELLO`, `PING`,
  `PONG` and `LOG`.

The host likewise does not block on receive. A separate task on core 0 at priority 8 reads the
UART, while the audio output and I2S receive tasks run on core 1 at priorities 22 and 23. The
receive task puts whole frames into a 6144-byte ring without waiting for room, and the main loop
parses them. The host counts driver overflows, a full ring, CRC and COBS errors, oversize frames and
`seq` gaps separately.

---

## 14. Version compatibility

Each side takes from the other only what the other has announced. What a host gets from a 1.10
device, depending on its own `proto_minor`:

| Host | What changes |
|---|---|
| < 1.1 or no bit 3 | 16-bit slots only, 24-bit sources with dither |
| < 1.3 or no bit 4 | the old `STREAM_FMT` + 50 ms path; with bit 3 — 32-bit slots for all codecs |
| < 1.4 or no bit 5 | no 96 kHz |
| < 1.6 or not both bits 1 and 6 | no cover art at all |
| < 1.7 | no `COVER_RETRY`: an image with one lost part stays empty |
| < 1.8 | no Bluetooth settings; NoInputNoOutput pairing, without a request |
| < 1.9 or no bit 8 | the device is the I2S master |

In the other direction the host looks at the device's `proto_minor` and bits the same way (3.3): it
accepts `STREAM_PREPARE` from a device with `proto_minor` >= 3, `COVER_RESET` and `COVER_DATA`
from >= 5, `COVER_END` from >= 6; it sends `COVER_RETRY` only with device bit 6, `0x25`–`0x28`
only with bit 7; it drives BCLK and WS only while the device's last `HELLO` carries bit 8. The host
does not send codes `0x30`–`0x35` (section 11).
