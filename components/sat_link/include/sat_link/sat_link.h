#pragma once
/// The satellite link, byte for byte.
///
/// A second ESP32 carries Bluetooth for this player: it decodes A2DP and sends the audio on
/// I2S-B, clocked by the satellite or, with a 1.9+ satellite, by this panel
/// (docs/LINK_PROTOCOL.md, "3.5 Who clocks I2S (1.9)"), and this UART carries everything that is
/// not audio — what is playing, what the phone did to the volume and which transport button we
/// want pressed.
/// Nothing else travels on it (docs/LINK_PROTOCOL.md, "Reserved codes and bits").
///
/// Framing is COBS between 0x00 delimiters. That choice is what lets a receiver drop a
/// frame it cannot hold, or lose a byte to noise, and be back in step at the very next
/// delimiter — no timeout, no guessing, no half-frame delivered as if it were whole.
///
/// docs/LINK_PROTOCOL.md is the contract and it is shared with the satellite's own repo.
/// This file implements version 1.10 of it (PROTO_MINOR) and nothing else.
///
/// Nothing here touches ESP-IDF, so it builds and is tested on the host.

#include "cover_wire.h"
#include "format_sync.h"
#include <stddef.h>
#include <stdint.h>

namespace sat {

// ── Sizes. All three come from the contract and none of them is derivable from another. ────
constexpr size_t MAX_FRAME   = 1040;            ///< decoded: type + seq + payload + crc16
constexpr size_t MAX_PAYLOAD = MAX_FRAME - 4;   ///< 1036
constexpr size_t MAX_WIRE    = 1048;            ///< what a receiver must be able to hold

constexpr uint8_t PROTO_MAJOR = 1;
/// 1.10: the port expander is gone. Codes 0x30..0x35, device bits 4 and 5 and host bit 2
/// mean nothing any more and are never reused.
constexpr uint8_t PROTO_MINOR = 10;

// ── Message types ──────────────────────────────────────────────────────────────────────────
enum : uint8_t {
    MSG_HELLO         = 0x01,
    MSG_PING          = 0x02,
    MSG_PONG          = 0x03,
    MSG_BT_STATE      = 0x10,
    MSG_TRACK_META    = 0x11,
    MSG_TRACK_POS     = 0x12,
    MSG_STREAM_FMT    = 0x13,
    MSG_COVER         = 0x14,
    MSG_VOLUME_STATE  = 0x15,
    MSG_STREAM_PREPARE = FMT_PREPARE,
    MSG_STREAM_READY   = FMT_READY,
    MSG_PAIR_REQUEST  = 0x1A,   ///< 1.8
    MSG_PAIR_RESULT   = 0x1B,   ///< 1.8
    MSG_BT_CONFIG     = 0x1C,   ///< 1.8
    MSG_BOND_LIST     = 0x1D,   ///< 1.8
    MSG_CMD_TRANSPORT = 0x20,
    MSG_CMD_VOLUME    = 0x21,
    MSG_CMD_RADIO     = 0x22,
    MSG_COVER_RETRY   = COVER_RETRY,
    MSG_PAIR_REPLY    = 0x25,   ///< 1.8
    MSG_BT_CONFIG_SET = 0x26,   ///< 1.8
    MSG_BOND_REMOVE   = 0x27,   ///< 1.8
    MSG_BT_REFRESH    = 0x28,   ///< 1.8
    // 0x30..0x35: the port expander until 1.9. Never reused; a 1.9 satellite's GPIO_MAP
    // arrives as an unknown type and is dropped like any other.
    MSG_LOG           = 0x40,
};

/// What the satellite says it can do.
enum : uint32_t {
    DEV_CAP_A2DP       = 1u << 0,
    DEV_CAP_META       = 1u << 1,
    DEV_CAP_ABS_VOLUME = 1u << 2,
    DEV_CAP_COVER      = 1u << 3,
    // Bits 4 and 5: EXPANDER and ADC until 1.9. Never reused; device_caps() clears them.
    /// Added in 1.7. Says a COVER_RETRY naming the current pair will be answered with a
    /// fresh generation and a repeated OBEX GET. Requires DEV_CAP_COVER and minor >= 7;
    /// a satellite older than that never sets it, so the host simply never asks and the
    /// link is 1.6 in every observable way.
    DEV_CAP_COVER_RETRY = COVER_RETRY_CAP,
    /// Added in 1.8. Sends PAIR_REQUEST/RESULT, BT_CONFIG and BOND_LIST, and takes PAIR_REPLY,
    /// BT_CONFIG_SET, BOND_REMOVE and BT_REFRESH. Without it the host shows no settings: a
    /// 1.7 satellite keeps its visibility and its bonds to itself.
    DEV_CAP_BT_SETTINGS = 1u << 7,
    /// Added in 1.9, and set only while true: the satellite's I2S output is a SLAVE that
    /// drives DATA alone and takes BCLK and WS from us. We drive those two lines only while
    /// the latest HELLO carries it — a satellite that clocks the link itself (every build
    /// before 1.9) must never meet a second driver on the same wire.
    DEV_CAP_I2S_SLAVE = 1u << 8,
};

/// The satellite's HELLO capabilities with every bit its own version cannot support cleared.
///
/// The satellite does exactly this to ours on arrival, and this side had no counterpart: it
/// read `caps` only to log it. A bit is not a promise on its own — a build from before the
/// bit existed may set it for something else entirely, and the contract words every new
/// capability as "bit N AND minor >= M" for that reason. Doing it once, here, means every
/// later test is a single bit test and nobody has to remember which version brought which.
///
/// Free function and constexpr so the host tests can pin the rule itself rather than its
/// effect on a module that needs ESP-IDF to exist.
constexpr uint32_t device_caps(uint8_t minor, uint32_t caps)
{
    // 1.7. Requires the cover bit too: retrying a transfer that never happens is meaningless,
    // and a device claiming it without DEV_CAP_COVER is describing something else.
    if (minor < 7 || !(caps & DEV_CAP_COVER)) caps &= ~(uint32_t)DEV_CAP_COVER_RETRY;
    // 1.8.
    if (minor < 8) caps &= ~(uint32_t)DEV_CAP_BT_SETTINGS;
    // 1.9.
    if (minor < 9) caps &= ~(uint32_t)DEV_CAP_I2S_SLAVE;
    // 1.10 retired bits 4 and 5 (the expander). A 1.9 satellite still sets bit 4, and it
    // promises nothing this side uses, so it is dropped here for every version.
    caps &= ~((1u << 4) | (1u << 5));
    return caps;
}

/// What we say we can do. Bit 0 is the one that changes the satellite's behaviour: without
/// it the satellite offers the phone 44.1 kHz only, because a host that cannot relock its
/// I2S has no business being handed 48 kHz mid-connection.
enum : uint32_t {
    HOST_CAP_RELOCK_I2S = 1u << 0,
    HOST_CAP_WANT_COVER = 1u << 1,
    // Bit 2: EXPANDER until 1.9. Never set, never reused.
    /// Added in 1.1. Says a STREAM_FMT announcing 32-bit slots will be survived: the input
    /// channel is reconfigured to the announced width rather than reading a 32-bit stream
    /// with a 16-bit configuration, which is silent and sounds like a bad codec.
    HOST_CAP_WIDE_SLOTS = 1u << 3,
    HOST_CAP_FORMAT_READY = FMT_HOST_CAP,
    HOST_CAP_96KHZ = 1u << 5,
    /// Added in 1.8. Says pairing requests will be shown and answered with PAIR_REPLY. With it
    /// the satellite stops agreeing to every phone by itself — so it is set only while there
    /// is a screen to ask on, or nobody could pair at all.
    HOST_CAP_PAIR_CONFIRM = 1u << 7,
    /// Added in 1.9. We clock the link: BCLK and WS at the announced rate in 32-bit slots,
    /// from the same controller and PLL as the DAC. A satellite that sees it stays a slave
    /// and says so with DEV_CAP_I2S_SLAVE; one that does not keeps clocking the link itself.
    /// Measured reason: as a SLAVE this P4 received ~95 856 of the 96 000 frames a second the
    /// satellite's own pins carried; frames it clocks itself cannot go missing.
    HOST_CAP_I2S_MASTER = 1u << 8,
};

// ── 1.8: Bluetooth settings ────────────────────────────────────────────────────────────────

enum : uint8_t {   ///< PAIR_REQUEST.kind
    PAIR_NUMERIC = 0,   ///< show the passkey, answer with PAIR_REPLY
    PAIR_CONSENT = 1,   ///< Just Works: no code, answer with PAIR_REPLY
    PAIR_PASSKEY = 2,   ///< show the passkey to be typed on the phone; nothing to answer
};

enum : uint8_t {   ///< PAIR_RESULT.status; parse() folds the reserved values into FAILED
    PAIR_OK = 0, PAIR_REJECTED = 1, PAIR_TIMEOUT = 2, PAIR_FAILED = 3,
};

enum : uint8_t {   ///< BT_CONFIG codec bits
    CODEC_BIT_SBC = 1u << 0, CODEC_BIT_AAC = 1u << 1, CODEC_BIT_APTX = 1u << 2,
    CODEC_BIT_APTX_HD = 1u << 3, CODEC_BIT_LHDC_V5 = 1u << 4,
};

enum : uint8_t {   ///< BOND_LIST flags
    BOND_CONNECTED = 1u << 0,   ///< connected now
    BOND_LAST      = 1u << 1,   ///< the last one connected: the auto-reconnect target
};

constexpr size_t BOND_MAX    = 16;   ///< entries in one BOND_LIST
constexpr size_t BT_NAME_MAX = 32;   ///< bytes of UTF-8 in any 1.8 name

enum : uint8_t {   ///< BT_STATE.state
    BT_OFF = 0, BT_IDLE = 1, BT_DISCOVERABLE = 2, BT_CONNECTING = 3,
    BT_CONNECTED = 4, BT_STREAMING = 5,
};

enum : uint8_t {   ///< BT_STATE.codec
    CODEC_NONE = 0, CODEC_SBC = 1, CODEC_AAC = 2, CODEC_APTX = 3, CODEC_APTX_HD = 4, CODEC_LHDC_V5 = 5,
};

enum : uint8_t {   ///< TRACK_POS.state
    PLAY_STOPPED = 0, PLAY_PLAYING = 1, PLAY_PAUSED = 2, PLAY_FWD_SEEK = 3, PLAY_REV_SEEK = 4,
};

/// The longest TRACK_POS millisecond value that is allowed to mean anything.
///
/// The contract puts this cleanup on the satellite ("`total_ms` завжди санітизувати"), and
/// parse() repeats it because an older satellite build is still a legal peer. One bound
/// covers both faults it has to catch: a day is far below 2^31, so every negative value —
/// the bench phone answered GetPlayStatus with a length of −1000, which arrives here as
/// 0xFFFFFC18 — is already past it.
constexpr uint32_t POS_MAX_MS = 24u * 60u * 60u * 1000u;

enum : uint8_t {   ///< CMD_TRANSPORT.cmd
    TR_PLAY = 0, TR_PAUSE = 1, TR_STOP = 2, TR_NEXT = 3, TR_PREV = 4,
    TR_FFWD_START = 5, TR_FFWD_STOP = 6, TR_REW_START = 7, TR_REW_STOP = 8,
};

// ── Primitives ─────────────────────────────────────────────────────────────────────────────

/// CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no final XOR. Check value for "123456789"
/// is 0x29B1, and the host tests pin that.
uint16_t crc16(const uint8_t* data, size_t len);

/// Standard COBS. `dst` needs len + len/254 + 2 bytes. Returns bytes written.
size_t cobs_encode(const uint8_t* src, size_t len, uint8_t* dst);

/// Returns bytes written, or 0 when the input is malformed or will not fit in dst_cap.
/// A zero return also covers "decoded to nothing", which is the empty frame the contract
/// tells us to ignore — so one check covers both.
size_t cobs_decode(const uint8_t* src, size_t len, uint8_t* dst, size_t dst_cap);

// ── Reading ────────────────────────────────────────────────────────────────────────────────

/// A string as it sits in the received frame: not NUL-terminated, and only valid until the
/// next byte is fed to the Reader. Copy what you keep.
struct Str {
    const char* p   = nullptr;
    uint16_t    len = 0;
};

struct Frame {
    uint8_t        type    = 0;
    uint8_t        seq     = 0;
    const uint8_t* payload = nullptr;   ///< into the Reader's buffer; same lifetime as Str
    size_t         len     = 0;
};

/// Byte-stream frame assembler. Holds ~2 KB of buffers, so it belongs to a module, not a
/// stack frame.
class Reader {
public:
    using Fn = void (*)(void* ctx, const Frame& f);

    /// Feeds n bytes, calling fn once per good frame. Returns how many it delivered.
    size_t feed(const uint8_t* data, size_t n, Fn fn, void* ctx);

    void reset();

    uint32_t bad_crc()  const { return bad_crc_; }
    uint32_t too_big()  const { return too_big_; }
    uint32_t bad_cobs() const { return bad_cobs_; }

    /// Frames the satellite sent that never arrived here, counted from the gaps in `seq`.
    ///
    /// The other three counters each name one way a frame can be spoiled on OUR side of the
    /// wire. This one is the only one that sees a frame which never reached us at all — a
    /// byte lost to noise takes its whole frame with it and trips nothing else, because a
    /// frame that is not delivered is not examined. It also catches the frames the driver
    /// dropped before we saw them, which is what an unwatched UART overrun looks like from
    /// here, and that is the fault this counter was added for.
    uint32_t seq_gaps() const { return seq_gaps_; }

private:
    void deliver(Fn fn, void* ctx, size_t& count);

    uint8_t  wire_[MAX_WIRE];
    uint8_t  frame_[MAX_FRAME];
    size_t   n_        = 0;
    bool     over_     = false;
    uint32_t bad_crc_  = 0;
    uint32_t too_big_  = 0;
    uint32_t bad_cobs_ = 0;
    bool     have_seq_ = false;   ///< false until the first good frame gives us a reference
    uint8_t  last_seq_ = 0;
    uint32_t seq_gaps_ = 0;
};

/// Little-endian payload reader. Every accessor is bounds-checked once; after the first
/// overrun ok() stays false and the rest read as zero, so a caller can parse a whole
/// message and test ok() at the end instead of after every field.
class Cursor {
public:
    Cursor(const uint8_t* p, size_t n) : p_(p), n_(n) {}

    uint8_t  u8();
    uint16_t u16();
    uint32_t u32();
    Str      str();
    bool     bytes(const uint8_t*& out, size_t n);

    bool   ok()   const { return ok_; }
    size_t left() const { return ok_ ? n_ - i_ : 0; }

private:
    const uint8_t* p_;
    size_t         n_;
    size_t         i_  = 0;
    bool           ok_ = true;
};

// ── Writing ────────────────────────────────────────────────────────────────────────────────

/// Little-endian payload writer over a caller-supplied buffer.
class Packer {
public:
    Packer(uint8_t* p, size_t cap) : p_(p), cap_(cap) {}

    void u8(uint8_t v);
    void u16(uint16_t v);
    void u32(uint32_t v);
    void str(const char* s, size_t n);
    void str(const char* s);            ///< NUL-terminated
    void bytes(const uint8_t* s, size_t n);

    const uint8_t* data() const { return p_; }
    size_t         size() const { return i_; }
    bool           ok()   const { return ok_; }

private:
    uint8_t* p_;
    size_t   cap_;
    size_t   i_  = 0;
    bool     ok_ = true;
};

/// Owns the outgoing sequence number and one wire buffer.
class Writer {
public:
    /// Frames one message. Returns the number of bytes in wire() to put on the UART, or 0
    /// if the payload is too long — in which case seq does not advance, because a sequence
    /// number burned on a frame nobody saw is a gap the other side would report as loss.
    size_t frame(uint8_t type, const uint8_t* payload, size_t len);

    const uint8_t* wire() const { return wire_; }
    uint8_t        seq()  const { return seq_; }

private:
    uint8_t body_[MAX_FRAME];   ///< pre-COBS, so framing never puts a kilobyte on the stack
    uint8_t wire_[MAX_WIRE];
    uint8_t seq_ = 0;
};

// ── Typed messages ─────────────────────────────────────────────────────────────────────────
// Each parse returns false when the frame is the wrong type or runs short. Views point into
// the Reader's buffer.

struct Hello      { uint8_t major = 0, minor = 0; uint32_t caps = 0; Str build; };
struct BtState    { uint8_t state = 0, codec = 0; const uint8_t* addr = nullptr; Str name; };
struct TrackMeta  { Str title, artist, album; };
struct TrackPos   { uint8_t state = 0; uint32_t elapsed_ms = 0, total_ms = 0; };
struct StreamFmt  { uint32_t rate_hz = 0; uint8_t bits = 0, channels = 0; };
struct Volume     { uint8_t volume = 0, source = 0; };
struct Ping       { uint32_t tick_ms = 0; };
struct Pong       { uint32_t echo_ms = 0, own_ms = 0; };
struct LogLine    { uint8_t level = 0; Str text; };
struct CoverChunk {
    uint16_t       chunk = 0, total = 0;
    uint8_t        mime = 0;
    uint32_t       total_len = 0;
    const uint8_t* data = nullptr;
    uint16_t       len = 0;
};
/// 1.7. We only send this one, but it parses too: a builder nobody can decode is a
/// builder nobody has checked, and every other message here is pinned by a round trip.
struct CoverRetry { uint32_t nonce = 0, generation = 0; };

// 1.8. The addresses point into the frame like the strings do: copy what you keep.
struct PairRequest { const uint8_t* addr = nullptr; uint8_t kind = 0; uint32_t passkey = 0; Str name; };
struct PairResult  { const uint8_t* addr = nullptr; uint8_t status = 0; };
struct BtConfig {
    uint8_t radio = 0, discoverable = 0, auto_reconnect = 0;
    uint8_t codecs_supported = 0, codecs_enabled = 0;
    Str     name;
};
struct Bond { const uint8_t* addr = nullptr; uint8_t flags = 0; Str name; };
/// The host sends these three; they parse too, for the round trips the tests pin.
struct PairReply   { const uint8_t* addr = nullptr; uint8_t accept = 0; };
struct BtConfigSet { uint8_t radio = 0, discoverable = 0, auto_reconnect = 0, codecs_enabled = 0; Str name; };
struct BondRemove  { const uint8_t* addr = nullptr; };

bool parse(const Frame& f, Hello& out);
bool parse(const Frame& f, BtState& out);
bool parse(const Frame& f, TrackMeta& out);
bool parse(const Frame& f, TrackPos& out);
bool parse(const Frame& f, StreamFmt& out);
bool parse(const Frame& f, Volume& out);
bool parse(const Frame& f, Ping& out);
bool parse(const Frame& f, Pong& out);
bool parse(const Frame& f, LogLine& out);
bool parse(const Frame& f, CoverChunk& out);
bool parse(const Frame& f, CoverRetry& out);
bool parse(const Frame& f, PairRequest& out);
bool parse(const Frame& f, PairResult& out);
bool parse(const Frame& f, BtConfig& out);
bool parse(const Frame& f, PairReply& out);
bool parse(const Frame& f, BtConfigSet& out);
bool parse(const Frame& f, BondRemove& out);

/// 1.8. BOND_LIST, the same shape: up to cap entries into out, `count` as the frame has it,
/// `total` the bonds the satellite holds (more than count when the oldest did not fit).
bool parse_bond_list(const Frame& f, Bond* out, size_t cap, size_t& count, uint8_t& total);

// ── Building the messages we send ──────────────────────────────────────────────────────────
// Each writes into `w` and returns the wire length, 0 on overflow.

size_t build_hello(Writer& w, uint32_t caps, const char* build);
size_t build_ping(Writer& w, uint32_t tick_ms);
size_t build_pong(Writer& w, uint32_t echo_ms, uint32_t own_ms);
size_t build_transport(Writer& w, uint8_t cmd);
size_t build_volume(Writer& w, uint8_t volume);
size_t build_radio(Writer& w, bool on, bool discoverable, bool disconnect);

/// 1.7. Asks the satellite to fetch this track's cover again after a transfer that
/// produced no usable image. Send only to a peer whose HELLO carried minor >= 7 and
/// DEV_CAP_COVER_RETRY: the contract makes the behaviour conditional on the bit, not on
/// the version alone, because a device may know 1.7 and still not want the traffic.
size_t build_cover_retry(Writer& w, uint32_t nonce, uint32_t generation);

// 1.8. Send only to a peer whose HELLO carried minor >= 8 and DEV_CAP_BT_SETTINGS.

/// The answer to the current PAIR_REQUEST of kind 0 or 1.
size_t build_pair_reply(Writer& w, const uint8_t addr[6], bool accept);
/// The whole state, not a change: every field as it should be afterwards. An empty or null
/// name keeps the current one; a name longer than BT_NAME_MAX bytes is refused (returns 0)
/// rather than cut, because a cut can split a UTF-8 character and the satellite would then
/// refuse the whole name and keep the old one silently.
size_t build_bt_config_set(Writer& w, bool radio, bool discoverable, bool auto_reconnect,
                           uint8_t codecs_enabled, const char* name);
/// A null addr forgets every bond (FF×6 on the wire).
size_t build_bond_remove(Writer& w, const uint8_t* addr);
size_t build_bt_refresh(Writer& w);

}  // namespace sat
