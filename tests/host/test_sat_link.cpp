/**
 * @file test_sat_link.cpp
 * @brief The satellite link's framing and messages, pinned against docs/LINK_PROTOCOL.md.
 *
 * This code talks to firmware built from another repository by another author, so the only
 * thing holding the two halves together is the written contract. Every number below is
 * quoted from it rather than derived from this implementation: the CRC check value, the
 * 1045-byte worst case for COBS, the 1040/1036/1048 buffer sizes, the field order of each
 * message. If a test here fails, either this side drifted or the document did — and both
 * are worth stopping for.
 *
 * The resynchronisation cases matter most. COBS was chosen over a length header precisely
 * so that a lost byte, a corrupted one, or a frame too big for the receiver costs exactly
 * one frame and never a timeout, so each of those three faults is followed here by a good
 * frame that must still arrive.
 */

#include "doctest.h"
#include "sat_link/sat_link.h"

#include <string.h>

#include <string>
#include <vector>

using namespace sat;

// ── helpers ────────────────────────────────────────────────────────────────────────────────

namespace {

struct Catcher {
    std::vector<uint8_t>              types;
    std::vector<uint8_t>              seqs;
    std::vector<std::vector<uint8_t>> payloads;

    static void on(void* ctx, const Frame& f)
    {
        auto* c = (Catcher*)ctx;
        c->types.push_back(f.type);
        c->seqs.push_back(f.seq);
        c->payloads.emplace_back(f.payload, f.payload + f.len);
    }
};

/// Builds a frame by hand so a test can choose a wrong CRC on purpose.
size_t hand_frame(uint8_t type, uint8_t seq, const uint8_t* pay, size_t n,
                  uint16_t crc, uint8_t* out)
{
    uint8_t body[128];
    body[0] = type;
    body[1] = seq;
    memcpy(body + 2, pay, n);
    body[2 + n] = (uint8_t)(crc & 0xFF);
    body[3 + n] = (uint8_t)(crc >> 8);

    out[0]       = 0;
    const size_t k = cobs_encode(body, n + 4, out + 1);
    out[1 + k]   = 0;
    return k + 2;
}

std::string as_string(const Str& s)
{
    return std::string(s.p ? s.p : "", s.len);
}

}  // namespace

// ── CRC ────────────────────────────────────────────────────────────────────────────────────

TEST_CASE("the CRC is CCITT-FALSE and not one of its neighbours") {
    // The published check value. CCITT-FALSE shares a polynomial with three other CRC-16
    // variants and differs only in the initial value and reflection, so this single vector
    // is what separates it from a link that agrees with itself and nothing else.
    const char* s = "123456789";
    CHECK(crc16((const uint8_t*)s, 9) == 0x29B1);

    // Empty input is the bare initial value, not zero.
    CHECK(crc16(nullptr, 0) == 0xFFFF);
}

// ── COBS ───────────────────────────────────────────────────────────────────────────────────

TEST_CASE("COBS survives every shape of zero") {
    struct Case { const char* name; std::vector<uint8_t> data; };
    const Case cases[] = {
        {"one byte",            {0x42}},
        {"a single zero",       {0x00}},
        {"all zeros",           {0, 0, 0, 0, 0}},
        {"leading zero",        {0, 1, 2, 3}},
        {"trailing zero",       {1, 2, 3, 0}},
        {"zeros at both ends",  {0, 1, 0, 2, 0}},
        {"no zeros at all",     {1, 2, 3, 4, 5}},
    };

    for (const auto& c : cases) {
        CAPTURE(c.name);
        uint8_t enc[64], dec[64];
        const size_t n = cobs_encode(c.data.data(), c.data.size(), enc);

        // Nothing a COBS encoder emits may be zero: that is the whole point of it.
        for (size_t i = 0; i < n; i++) CHECK(enc[i] != 0);

        const size_t m = cobs_decode(enc, n, dec, sizeof dec);
        REQUIRE(m == c.data.size());
        CHECK(memcmp(dec, c.data.data(), m) == 0);
    }
}

TEST_CASE("COBS overhead stays inside the buffer the contract asks for") {
    // 254 non-zero bytes is exactly one full group, and the byte after it opens another.
    std::vector<uint8_t> run(254, 0xAA);
    uint8_t enc[2048], dec[2048];

    size_t n = cobs_encode(run.data(), run.size(), enc);
    CHECK(cobs_decode(enc, n, dec, sizeof dec) == 254);
    CHECK(memcmp(dec, run.data(), 254) == 0);

    run.push_back(0xBB);
    n = cobs_encode(run.data(), run.size(), enc);
    CHECK(cobs_decode(enc, n, dec, sizeof dec) == 255);
    CHECK(memcmp(dec, run.data(), 255) == 0);

    // The worst case the document names: a full 1040-byte frame with no zero in it encodes
    // to 1045, which with two delimiters is 1047 and fits the 1048-byte receive buffer.
    std::vector<uint8_t> full(MAX_FRAME, 0x5A);
    n = cobs_encode(full.data(), full.size(), enc);
    CHECK(n == 1045);
    CHECK(n + 2 <= MAX_WIRE);
}

TEST_CASE("a malformed group is refused rather than trusted") {
    uint8_t dec[64];
    // A code byte that claims more bytes than the run holds.
    const uint8_t runaway[] = {0x40, 0x01};
    CHECK(cobs_decode(runaway, sizeof runaway, dec, sizeof dec) == 0);

    // A zero inside a frame cannot happen and means we mis-framed.
    const uint8_t inner_zero[] = {0x03, 0x01, 0x00, 0x02};
    CHECK(cobs_decode(inner_zero, sizeof inner_zero, dec, sizeof dec) == 0);

    // Output that would not fit is refused, not truncated.
    const uint8_t fine[] = {0x05, 1, 2, 3, 4};
    uint8_t       tiny[2];
    CHECK(cobs_decode(fine, sizeof fine, tiny, sizeof tiny) == 0);
}

// ── framing ────────────────────────────────────────────────────────────────────────────────

TEST_CASE("a frame written by one side is read by the other") {
    Writer  w;
    Reader  r;
    Catcher got;

    const uint8_t pay[] = {0x01, 0x00, 0xFF, 0x7F};
    const size_t  n     = w.frame(MSG_BT_STATE, pay, sizeof pay);
    REQUIRE(n > 0);
    CHECK(n <= MAX_WIRE);

    CHECK(r.feed(w.wire(), n, Catcher::on, &got) == 1);
    REQUIRE(got.types.size() == 1);
    CHECK(got.types[0] == MSG_BT_STATE);
    CHECK(got.seqs[0] == 0);
    REQUIRE(got.payloads[0].size() == sizeof pay);
    CHECK(memcmp(got.payloads[0].data(), pay, sizeof pay) == 0);
}

TEST_CASE("the split between reads does not matter") {
    Writer w;
    uint8_t stream[4096];
    size_t  total = 0;
    for (int i = 0; i < 5; i++) {
        const uint8_t pay[] = {(uint8_t)i, 0x00, 0x11};
        const size_t  n     = w.frame(MSG_LOG, pay, sizeof pay);
        memcpy(stream + total, w.wire(), n);
        total += n;
    }

    // One read for everything.
    {
        Reader r; Catcher got;
        CHECK(r.feed(stream, total, Catcher::on, &got) == 5);
        CHECK(got.seqs == std::vector<uint8_t>{0, 1, 2, 3, 4});
    }
    // One byte at a time — the case a UART actually produces when the line is slow.
    {
        Reader r; Catcher got;
        size_t delivered = 0;
        for (size_t i = 0; i < total; i++) delivered += r.feed(stream + i, 1, Catcher::on, &got);
        CHECK(delivered == 5);
        CHECK(got.seqs == std::vector<uint8_t>{0, 1, 2, 3, 4});
    }
    // An awkward split that lands inside frames.
    {
        Reader r; Catcher got;
        size_t delivered = 0;
        for (size_t i = 0; i < total; i += 7)
            delivered += r.feed(stream + i, (i + 7 <= total) ? 7 : total - i, Catcher::on, &got);
        CHECK(delivered == 5);
        CHECK(got.seqs == std::vector<uint8_t>{0, 1, 2, 3, 4});
    }
}

TEST_CASE("a bad CRC costs one frame and not the stream") {
    Reader  r;
    Catcher got;
    uint8_t buf[128];

    const uint8_t pay[] = {0xDE, 0xAD};
    size_t n = hand_frame(MSG_LOG, 7, pay, sizeof pay, 0x0000, buf);   // a CRC we invented
    CHECK(r.feed(buf, n, Catcher::on, &got) == 0);
    CHECK(r.bad_crc() == 1);

    Writer w;
    n = w.frame(MSG_PING, pay, sizeof pay);
    CHECK(r.feed(w.wire(), n, Catcher::on, &got) == 1);
    CHECK(got.types.size() == 1);
    CHECK(got.types[0] == MSG_PING);
}

TEST_CASE("a frame too big for the buffer is dropped without losing the next one") {
    Reader  r;
    Catcher got;

    std::vector<uint8_t> flood(MAX_WIRE + 200, 0x77);
    CHECK(r.feed(flood.data(), flood.size(), Catcher::on, &got) == 0);
    const uint8_t delim = 0;
    CHECK(r.feed(&delim, 1, Catcher::on, &got) == 0);
    CHECK(r.too_big() == 1);

    Writer        w;
    const uint8_t pay[] = {1, 2, 3};
    const size_t  n     = w.frame(MSG_PONG, pay, sizeof pay);
    CHECK(r.feed(w.wire(), n, Catcher::on, &got) == 1);
    CHECK(got.types[0] == MSG_PONG);
}

TEST_CASE("joining a stream mid-frame costs only the partial frame") {
    Writer w;
    Reader r;
    Catcher got;

    const uint8_t pay[] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60};
    const size_t  n     = w.frame(MSG_TRACK_POS, pay, sizeof pay);

    // Start listening in the middle: everything up to the closing delimiter is rubbish.
    const size_t cut = n / 2;
    CHECK(r.feed(w.wire() + cut, n - cut, Catcher::on, &got) == 0);
    CHECK(got.types.empty());

    const size_t n2 = w.frame(MSG_TRACK_POS, pay, sizeof pay);
    CHECK(r.feed(w.wire(), n2, Catcher::on, &got) == 1);
    CHECK(got.types[0] == MSG_TRACK_POS);
}

TEST_CASE("empty runs between delimiters are ignored, not counted as faults") {
    Reader  r;
    Catcher got;

    const uint8_t zeros[] = {0, 0, 0, 0, 0};
    CHECK(r.feed(zeros, sizeof zeros, Catcher::on, &got) == 0);
    CHECK(r.bad_crc() == 0);
    CHECK(r.bad_cobs() == 0);
    CHECK(r.too_big() == 0);
}

TEST_CASE("the biggest legal payload goes through and one byte more does not") {
    Writer  w;
    Reader  r;
    Catcher got;

    std::vector<uint8_t> big(MAX_PAYLOAD, 0x5A);
    const size_t n = w.frame(MSG_COVER, big.data(), big.size());
    REQUIRE(n > 0);
    CHECK(n <= MAX_WIRE);
    CHECK(r.feed(w.wire(), n, Catcher::on, &got) == 1);
    CHECK(got.payloads[0].size() == MAX_PAYLOAD);

    // Refusing must not burn a sequence number: a gap would read as a lost frame.
    const uint8_t before = w.seq();
    std::vector<uint8_t> too_big(MAX_PAYLOAD + 1, 0x5A);
    CHECK(w.frame(MSG_COVER, too_big.data(), too_big.size()) == 0);
    CHECK(w.seq() == before);
}

TEST_CASE("the sequence number wraps instead of stopping") {
    Writer        w;
    const uint8_t b = 0;
    for (int i = 0; i < 255; i++) w.frame(MSG_PING, &b, 1);
    CHECK(w.seq() == 255);
    w.frame(MSG_PING, &b, 1);
    CHECK(w.seq() == 0);
}

// ── messages ───────────────────────────────────────────────────────────────────────────────

namespace {

/// Frames one device-to-host message and hands it straight back as a parsed Frame, the way
/// the satellite would put it on the wire.
struct Loop {
    Writer  w;
    Reader  r;
    Frame   f;
    bool    got = false;

    static void on(void* ctx, const Frame& in)
    {
        auto* l = (Loop*)ctx;
        l->f    = in;
        l->got  = true;
    }

    bool send(uint8_t type, const Packer& p)
    {
        got = false;
        const size_t n = w.frame(type, p.data(), p.size());
        if (!n) return false;
        r.feed(w.wire(), n, Loop::on, this);
        return got;
    }
};

}  // namespace

TEST_CASE("HELLO carries the version the two sides must agree on") {
    Loop    l;
    uint8_t buf[64];
    Packer  p(buf, sizeof buf);
    p.u8(1);
    p.u8(0);
    p.u32(DEV_CAP_A2DP | DEV_CAP_META | DEV_CAP_ABS_VOLUME);
    p.str("sat 0.1.0 a1b2c3d");
    REQUIRE(l.send(MSG_HELLO, p));

    Hello h;
    REQUIRE(parse(l.f, h));
    CHECK(h.major == 1);
    CHECK(h.minor == 0);
    CHECK(h.caps == 0x07u);
    CHECK(as_string(h.build) == "sat 0.1.0 a1b2c3d");
}

TEST_CASE("the first TRACK_META after a connection is empty, and that parses") {
    // Measured on the bench: the phone answered the first metadata request with three empty
    // fields and filled them in about ten seconds later. An empty string is a legal message,
    // not a short read, and the host must not treat it as a fault.
    Loop    l;
    uint8_t buf[64];
    Packer  p(buf, sizeof buf);
    p.str("", 0);
    p.str("", 0);
    p.str("", 0);
    REQUIRE(l.send(MSG_TRACK_META, p));

    TrackMeta m;
    REQUIRE(parse(l.f, m));
    CHECK(m.title.len == 0);
    CHECK(m.artist.len == 0);
    CHECK(m.album.len == 0);
}

TEST_CASE("TRACK_META carries UTF-8 unchanged") {
    Loop    l;
    uint8_t buf[256];
    Packer  p(buf, sizeof buf);
    p.str("Весна");
    p.str("Океан Ельзи");
    p.str("");
    REQUIRE(l.send(MSG_TRACK_META, p));

    TrackMeta m;
    REQUIRE(parse(l.f, m));
    CHECK(as_string(m.title) == "Весна");
    CHECK(as_string(m.artist) == "Океан Ельзи");
    CHECK(m.album.len == 0);
}

TEST_CASE("BT_STATE keeps the address as six raw bytes") {
    Loop    l;
    uint8_t buf[64];
    Packer  p(buf, sizeof buf);
    p.u8(BT_STREAMING);
    p.u8(CODEC_AAC);
    const uint8_t addr[6] = {0x00, 0x1A, 0x7D, 0xDA, 0x71, 0x13};
    p.bytes(addr, 6);
    p.str("Pixel 7");
    REQUIRE(l.send(MSG_BT_STATE, p));

    BtState s;
    REQUIRE(parse(l.f, s));
    CHECK(s.state == BT_STREAMING);
    CHECK(s.codec == CODEC_AAC);
    REQUIRE(s.addr != nullptr);
    CHECK(memcmp(s.addr, addr, 6) == 0);
    CHECK(as_string(s.name) == "Pixel 7");
}

TEST_CASE("a nonsense track length becomes unknown rather than a 49-day song") {
    // The same bench phone reported a duration of -1000 ms, which arrives here as
    // 0xFFFFFC18. Anything past a day means the player has no idea, and saying so beats
    // drawing a progress bar against a number nobody believes.
    Loop    l;
    uint8_t buf[16];
    Packer  p(buf, sizeof buf);
    p.u8(PLAY_PLAYING);
    p.u32(12345);
    p.u32(0xFFFFFC18u);
    REQUIRE(l.send(MSG_TRACK_POS, p));

    TrackPos t;
    REQUIRE(parse(l.f, t));
    CHECK(t.state == PLAY_PLAYING);
    CHECK(t.elapsed_ms == 12345u);
    CHECK(t.total_ms == 0u);
}

TEST_CASE("a real track length survives") {
    Loop    l;
    uint8_t buf[16];
    Packer  p(buf, sizeof buf);
    p.u8(PLAY_PAUSED);
    p.u32(65000);
    p.u32(243000);
    REQUIRE(l.send(MSG_TRACK_POS, p));

    TrackPos t;
    REQUIRE(parse(l.f, t));
    CHECK(t.total_ms == 243000u);
}

TEST_CASE("a nonsense position is refused too, and does not take the length with it") {
    // The length has been sanitised since the bench phone answered -1000 for it. The
    // POSITION was not, and it is the field the progress bar is actually drawn from: a
    // rubbish length only hides the bar, a rubbish position pins it at the far end and
    // writes a four-digit time under it. Same rule, same bound, and the length beside it
    // must survive — a fault in one field is not a reason to throw away the other.
    Loop    l;
    uint8_t buf[16];
    Packer  p(buf, sizeof buf);
    p.u8(PLAY_PLAYING);
    p.u32(0xFFFFFC18u);            // -1000 ms, as it arrives over an unsigned field
    p.u32(243000);
    REQUIRE(l.send(MSG_TRACK_POS, p));

    TrackPos t;
    REQUIRE(parse(l.f, t));
    CHECK(t.elapsed_ms == 0u);
    CHECK(t.total_ms == 243000u);
}

TEST_CASE("the sanity bound is a day, and a day itself is still a position") {
    // Pinned on both sides so the bound cannot drift into the range of real values: a
    // twelve-hour recording is a thing that exists, and refusing it would put an empty
    // field where an honest number belongs.
    struct Case { uint32_t sent, want; };
    const Case cases[] = {
        {0, 0},
        {1, 1},
        {POS_MAX_MS - 1, POS_MAX_MS - 1},
        {POS_MAX_MS, POS_MAX_MS},          // exactly a day: the contract says PAST a day
        {POS_MAX_MS + 1, 0},
        {0xFFFFFFFFu, 0},
    };
    for (const Case& c : cases) {
        Loop    l;
        uint8_t buf[16];
        Packer  p(buf, sizeof buf);
        p.u8(PLAY_PLAYING);
        p.u32(c.sent);
        p.u32(c.sent);
        REQUIRE(l.send(MSG_TRACK_POS, p));

        TrackPos t;
        REQUIRE(parse(l.f, t));
        CHECK(t.elapsed_ms == c.want);
        CHECK(t.total_ms == c.want);
    }
}

TEST_CASE("STREAM_FMT is what the I2S clock is set from") {
    Loop    l;
    uint8_t buf[16];
    Packer  p(buf, sizeof buf);
    p.u32(44100);
    p.u8(32);
    p.u8(2);
    REQUIRE(l.send(MSG_STREAM_FMT, p));

    StreamFmt s;
    REQUIRE(parse(l.f, s));
    CHECK(s.rate_hz == 44100u);
    CHECK(s.bits == 32);
    CHECK(s.channels == 2);
}

TEST_CASE("VOLUME_STATE says who moved it") {
    Loop    l;
    uint8_t buf[8];
    Packer  p(buf, sizeof buf);
    p.u8(96);
    p.u8(1);
    REQUIRE(l.send(MSG_VOLUME_STATE, p));

    Volume v;
    REQUIRE(parse(l.f, v));
    CHECK(v.volume == 96);
    CHECK(v.source == 1);
}

TEST_CASE("a cover chunk describes itself, so losing the first one is survivable") {
    Loop    l;
    uint8_t buf[1200];
    Packer  p(buf, sizeof buf);
    p.u16(3);            // chunk
    p.u16(17);           // total
    p.u8(1);             // jpeg
    p.u32(17 * 1024);
    p.u16(1024);
    std::vector<uint8_t> data(1024, 0xC3);
    p.bytes(data.data(), data.size());
    REQUIRE(l.send(MSG_COVER, p));

    CoverChunk c;
    REQUIRE(parse(l.f, c));
    CHECK(c.chunk == 3);
    CHECK(c.total == 17);
    CHECK(c.mime == 1);
    CHECK(c.total_len == 17u * 1024u);
    CHECK(c.len == 1024);
    CHECK(memcmp(c.data, data.data(), 1024) == 0);
}

TEST_CASE("a short payload fails to parse instead of reading past it") {
    Loop    l;
    uint8_t buf[8];
    Packer  p(buf, sizeof buf);
    p.u8(BT_CONNECTED);
    p.u8(CODEC_SBC);
    p.u8(0x00);          // one byte of an address that should be six
    REQUIRE(l.send(MSG_BT_STATE, p));

    BtState s;
    CHECK_FALSE(parse(l.f, s));
}

TEST_CASE("a string length that overruns the payload is refused") {
    Loop    l;
    uint8_t buf[16];
    Packer  p(buf, sizeof buf);
    p.u8(2);
    p.u16(500);          // claims 500 bytes of text
    p.bytes((const uint8_t*)"short", 5);
    REQUIRE(l.send(MSG_LOG, p));

    LogLine g;
    CHECK_FALSE(parse(l.f, g));
}

TEST_CASE("parsing checks the type, so a mixed-up handler cannot read the wrong fields") {
    Loop    l;
    uint8_t buf[16];
    Packer  p(buf, sizeof buf);
    p.u32(44100);
    p.u8(32);
    p.u8(2);
    REQUIRE(l.send(MSG_STREAM_FMT, p));

    TrackPos t;
    CHECK_FALSE(parse(l.f, t));
    StreamFmt s;
    CHECK(parse(l.f, s));
}

// ── the messages we send ───────────────────────────────────────────────────────────────────

TEST_CASE("the commands this player sends come back as the contract describes them") {
    Writer  w;
    Reader  r;
    Catcher got;

    size_t n = build_hello(w, HOST_CAP_RELOCK_I2S | HOST_CAP_WANT_COVER, "player 0.4.0");
    REQUIRE(n > 0);
    r.feed(w.wire(), n, Catcher::on, &got);

    n = build_transport(w, TR_NEXT);
    r.feed(w.wire(), n, Catcher::on, &got);

    n = build_volume(w, 64);
    r.feed(w.wire(), n, Catcher::on, &got);

    n = build_radio(w, true, true, false);
    r.feed(w.wire(), n, Catcher::on, &got);

    REQUIRE(got.types.size() == 4);
    CHECK(got.types == std::vector<uint8_t>{MSG_HELLO, MSG_CMD_TRANSPORT, MSG_CMD_VOLUME,
                                            MSG_CMD_RADIO});

    {   // HELLO announces version 1.0 and our capabilities
        Cursor c(got.payloads[0].data(), got.payloads[0].size());
        CHECK(c.u8() == PROTO_MAJOR);
        CHECK(c.u8() == PROTO_MINOR);
        CHECK(c.u32() == (HOST_CAP_RELOCK_I2S | HOST_CAP_WANT_COVER));
        CHECK(as_string(c.str()) == "player 0.4.0");
        CHECK(c.ok());
    }
    CHECK(got.payloads[1] == std::vector<uint8_t>{TR_NEXT});
    CHECK(got.payloads[2] == std::vector<uint8_t>{64});
    CHECK(got.payloads[3] == std::vector<uint8_t>{1, 1, 0});
}

// ── protocol 1.10: the port expander is gone ───────────────────────────────────────────────

TEST_CASE("1.10: the expander's device bits 4 and 5 mean nothing from any satellite") {
    // A 1.9 satellite sets bit 4 (it understood the expander's messages even with no lines),
    // and a 1.10 one never does. Either way nothing this side reads them, so they are cleared
    // for every version and the bits around them survive untouched.
    const uint32_t claims = DEV_CAP_A2DP | DEV_CAP_COVER | (1u << 4) | (1u << 5);
    for (uint8_t minor : {uint8_t(0), uint8_t(9), uint8_t(10), uint8_t(255)}) {
        CAPTURE((int)minor);
        const uint32_t got = device_caps(minor, claims);
        CHECK((got & ((1u << 4) | (1u << 5))) == 0u);
        CHECK((got & (DEV_CAP_A2DP | DEV_CAP_COVER)) == (DEV_CAP_A2DP | DEV_CAP_COVER));
    }
}

TEST_CASE("1.10: a 1.9 satellite's GPIO_MAP is just a frame of an unknown type") {
    // The reader does not know message types at all: it hands any whole frame up, and the
    // module drops what it has no case for. So the old satellite's GPIO_MAP after our HELLO
    // costs one frame and nothing else.
    Loop    l;
    uint8_t buf[4];
    Packer  p(buf, sizeof buf);
    p.u8(0);                                   // count = 0, the only table any 1.9 build sent
    REQUIRE(l.send(0x33, p));
    CHECK(l.f.type == 0x33);
    CHECK(l.f.len == 1);
    CHECK(PROTO_MINOR == 10);
}

// ── protocol 1.7: the cover retry, and the negotiation that gates it ───────────────────────
//
// The panel and the satellite are flashed separately and WILL run mismatched versions, so
// the cases below are not hypothetical: every one of them is a pair somebody will actually
// have on a desk. The rule under test is the contract's own wording — the host sends 0x24
// only to a peer whose HELLO carried minor >= 7 AND device capability bit 6 — and it is
// tested as the predicate rather than through the module, because the module needs ESP-IDF.

TEST_CASE("COVER_RETRY names one pair and survives the round trip") {
    Writer  w;
    Reader  r;
    Catcher got;

    const size_t n = build_cover_retry(w, 0xDEADBEEFu, 0x00000007u);
    REQUIRE(n > 0);
    REQUIRE(r.feed(w.wire(), n, Catcher::on, &got) == 1);

    CHECK(got.types[0] == MSG_COVER_RETRY);
    CHECK(got.types[0] == 0x24);            // the number in the document, not a symbol
    CHECK(got.payloads[0].size() == 8);     // "exactly 8 bytes"

    Frame f{got.types[0], got.seqs[0], got.payloads[0].data(), got.payloads[0].size()};
    CoverRetry c;
    REQUIRE(parse(f, c));
    CHECK(c.nonce == 0xDEADBEEFu);
    CHECK(c.generation == 7u);
}

TEST_CASE("a COVER_RETRY that names no pair is neither built nor parsed") {
    // "Both nonzero" in the contract. All-zero is the shape a truncated or padded frame
    // takes, and the satellite's pair check would drop it anyway — but a fault named at the
    // layout is a fault somebody can find, where one silently absorbed downstream is not.
    Writer w;
    CHECK(build_cover_retry(w, 0, 7) == 0);
    CHECK(build_cover_retry(w, 7, 0) == 0);
    CHECK(w.seq() == 0);                    // a refused build must not burn a sequence number

    uint8_t    zero[8] = {0};
    Frame      f{MSG_COVER_RETRY, 0, zero, sizeof zero};
    CoverRetry c;
    CHECK_FALSE(parse(f, c));

    // Right type, right emptiness, wrong length: nine bytes is not eight.
    uint8_t    nine[9] = {1, 0, 0, 0, 1, 0, 0, 0, 0};
    Frame      g{MSG_COVER_RETRY, 0, nine, sizeof nine};
    CHECK_FALSE(parse(g, c));

    // And the type is checked, so eight bytes of some other message cannot be read as a pair.
    uint8_t good[8] = {1, 0, 0, 0, 1, 0, 0, 0};
    Frame   h{MSG_CMD_VOLUME, 0, good, sizeof good};
    CHECK_FALSE(parse(h, c));
}

TEST_CASE("a pre-1.7 satellite is never asked to retry, whatever bits it sets") {
    // This is the mismatch that will actually happen: the panel is reflashed, the satellite
    // is not. Bit 6 of the device word is free space in 1.6, so a build that sets it for its
    // own reasons must not be read as a promise.
    const uint32_t claims = DEV_CAP_A2DP | DEV_CAP_COVER | DEV_CAP_COVER_RETRY;
    for (uint8_t minor = 0; minor < 7; minor++) {
        CAPTURE(minor);
        CHECK((device_caps(minor, claims) & DEV_CAP_COVER_RETRY) == 0u);
        // and nothing else is disturbed on the way past
        CHECK((device_caps(minor, claims) & DEV_CAP_COVER) == DEV_CAP_COVER);
        CHECK((device_caps(minor, claims) & DEV_CAP_A2DP) == DEV_CAP_A2DP);
    }
}

TEST_CASE("a 1.7 satellite that does not offer the retry is not asked either") {
    // The other direction of the same mismatch: a new satellite is entitled to decline the
    // OBEX traffic, so the contract gates on the BIT and not on the version alone.
    CHECK((device_caps(7, DEV_CAP_A2DP | DEV_CAP_COVER) & DEV_CAP_COVER_RETRY) == 0u);
    CHECK((device_caps(9, DEV_CAP_A2DP | DEV_CAP_COVER) & DEV_CAP_COVER_RETRY) == 0u);

    // Claiming the retry without claiming covers at all describes something else, whatever
    // the version, and is not honoured.
    CHECK((device_caps(7, DEV_CAP_A2DP | DEV_CAP_COVER_RETRY) & DEV_CAP_COVER_RETRY) == 0u);

    // Both halves present: this and only this is a peer we may send 0x24 to.
    const uint32_t ok = DEV_CAP_A2DP | DEV_CAP_COVER | DEV_CAP_COVER_RETRY;
    CHECK((device_caps(7, ok) & DEV_CAP_COVER_RETRY) == DEV_CAP_COVER_RETRY);
    // A satellite newer than us keeps it too: the gate is a floor, not an equality.
    CHECK((device_caps(255, ok) & DEV_CAP_COVER_RETRY) == DEV_CAP_COVER_RETRY);
}

TEST_CASE("our own HELLO announces 1.10, and the older bits keep their meaning") {
    // A minor bump must not move a capability bit: a satellite built against 1.6 reads bit 6
    // of the HOST word as "takes streaming covers", and one built against 1.7 bit 6 of its
    // own word as "honours COVER_RETRY".
    CHECK(PROTO_MAJOR == 1);
    CHECK(PROTO_MINOR == 10);
    CHECK(COVER_STREAM_CAP == (1u << 6));       // host word, 1.6
    CHECK(DEV_CAP_COVER_RETRY == (1u << 6));    // device word, 1.7 — a different word
    CHECK(HOST_CAP_PAIR_CONFIRM == (1u << 7));  // host word, 1.8
    CHECK(DEV_CAP_BT_SETTINGS == (1u << 7));    // device word, 1.8
    CHECK(HOST_CAP_I2S_MASTER == (1u << 8));    // host word, 1.9
    CHECK(DEV_CAP_I2S_SLAVE == (1u << 8));      // device word, 1.9
    CHECK(DEV_CAP_COVER == (1u << 3));
    CHECK(HOST_CAP_WANT_COVER == (1u << 1));

    Writer       w;
    Reader       r;
    Catcher      got;
    const size_t n = build_hello(w, HOST_CAP_WANT_COVER | COVER_STREAM_CAP, "player 0.4.0");
    REQUIRE(r.feed(w.wire(), n, Catcher::on, &got) == 1);
    Frame f{got.types[0], got.seqs[0], got.payloads[0].data(), got.payloads[0].size()};
    Hello h;
    REQUIRE(parse(f, h));
    CHECK(h.major == 1);
    CHECK(h.minor == 10);
    CHECK(h.caps == (HOST_CAP_WANT_COVER | COVER_STREAM_CAP));
}

// ── protocol 1.8: Bluetooth settings and pairing confirmed on the panel ────────────────────
//
// The satellite on the bench runs 1.7 while the panel runs 1.8, and the other way round after
// the next satellite flash: both pairs must behave. The rule is the contract's — the host
// sends 0x25–0x28 only to a peer with minor >= 8 AND device bit 7 — so it is tested as the
// predicate, and the layouts are tested against the document's field order.

TEST_CASE("the 1.8 messages have the numbers the document gives them") {
    CHECK(MSG_PAIR_REQUEST  == 0x1A);
    CHECK(MSG_PAIR_RESULT   == 0x1B);
    CHECK(MSG_BT_CONFIG     == 0x1C);
    CHECK(MSG_BOND_LIST     == 0x1D);
    CHECK(MSG_PAIR_REPLY    == 0x25);
    CHECK(MSG_BT_CONFIG_SET == 0x26);
    CHECK(MSG_BOND_REMOVE   == 0x27);
    CHECK(MSG_BT_REFRESH    == 0x28);
    CHECK(BOND_MAX == 16);
    CHECK(BT_NAME_MAX == 32);
}

TEST_CASE("a pre-1.8 satellite shows no settings, whatever bits it sets") {
    const uint32_t claims = DEV_CAP_A2DP | DEV_CAP_COVER | DEV_CAP_COVER_RETRY | DEV_CAP_BT_SETTINGS;
    for (uint8_t minor = 0; minor < 8; minor++) {
        CAPTURE(minor);
        CHECK((device_caps(minor, claims) & DEV_CAP_BT_SETTINGS) == 0u);
        CHECK((device_caps(minor, claims) & DEV_CAP_A2DP) == DEV_CAP_A2DP);
    }
    // 1.8 without the bit: the satellite may decline, and is then a 1.7 one to this side.
    CHECK((device_caps(8, DEV_CAP_A2DP) & DEV_CAP_BT_SETTINGS) == 0u);
    CHECK((device_caps(8, claims) & DEV_CAP_BT_SETTINGS) == DEV_CAP_BT_SETTINGS);
    CHECK((device_caps(255, claims) & DEV_CAP_BT_SETTINGS) == DEV_CAP_BT_SETTINGS);
    // and the 1.7 rule still holds inside a 1.8 word
    CHECK((device_caps(8, claims) & DEV_CAP_COVER_RETRY) == DEV_CAP_COVER_RETRY);
}

TEST_CASE("a pre-1.9 satellite is never taken for an I2S slave, whatever bits it sets") {
    // We drive BCLK and WS only while the satellite says it is a slave; a satellite that
    // clocks the link itself (every build before 1.9) must never meet a second driver.
    const uint32_t claims = DEV_CAP_A2DP | DEV_CAP_COVER | DEV_CAP_BT_SETTINGS | DEV_CAP_I2S_SLAVE;
    for (uint8_t minor = 0; minor < 9; minor++) {
        CAPTURE(minor);
        CHECK((device_caps(minor, claims) & DEV_CAP_I2S_SLAVE) == 0u);
    }
    // 1.9 without the bit: it keeps clocking the link, as a 1.8 one would.
    CHECK((device_caps(9, DEV_CAP_A2DP) & DEV_CAP_I2S_SLAVE) == 0u);
    CHECK((device_caps(9, claims) & DEV_CAP_I2S_SLAVE) == DEV_CAP_I2S_SLAVE);
    CHECK((device_caps(255, claims) & DEV_CAP_I2S_SLAVE) == DEV_CAP_I2S_SLAVE);
    // and the 1.8 rule still holds inside a 1.9 word
    CHECK((device_caps(9, claims) & DEV_CAP_BT_SETTINGS) == DEV_CAP_BT_SETTINGS);
}

namespace {
const uint8_t PHONE[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
}

TEST_CASE("PAIR_REQUEST carries the address, the kind, the passkey and the name") {
    uint8_t buf[64];
    Packer  p(buf, sizeof buf);
    p.bytes(PHONE, 6);
    p.u8(PAIR_NUMERIC);
    p.u32(42);                              // shown as 000 042: leading zeros are the screen's
    p.str("Pixel 7");
    Frame f{MSG_PAIR_REQUEST, 0, p.data(), p.size()};

    PairRequest r;
    REQUIRE(parse(f, r));
    CHECK(memcmp(r.addr, PHONE, 6) == 0);
    CHECK(r.kind == PAIR_NUMERIC);
    CHECK(r.passkey == 42u);
    CHECK(as_string(r.name) == "Pixel 7");
}

TEST_CASE("a PAIR_REQUEST nobody could show is refused") {
    // A kind this version does not know, or a "passkey" of seven digits, asks the panel to
    // put something on the glass that means nothing. Better no dialog than a wrong one.
    for (uint8_t kind : {uint8_t(3), uint8_t(255)}) {
        uint8_t buf[32];
        Packer  p(buf, sizeof buf);
        p.bytes(PHONE, 6); p.u8(kind); p.u32(1); p.str("");
        Frame f{MSG_PAIR_REQUEST, 0, p.data(), p.size()};
        PairRequest r;
        CAPTURE(kind);
        CHECK_FALSE(parse(f, r));
    }
    uint8_t buf[32];
    Packer  p(buf, sizeof buf);
    p.bytes(PHONE, 6); p.u8(PAIR_PASSKEY); p.u32(1000000u); p.str("");
    Frame f{MSG_PAIR_REQUEST, 0, p.data(), p.size()};
    PairRequest r;
    CHECK_FALSE(parse(f, r));

    Packer q(buf, sizeof buf);
    q.bytes(PHONE, 6); q.u8(PAIR_PASSKEY); q.u32(999999u); q.str("");
    Frame g{MSG_PAIR_REQUEST, 0, q.data(), q.size()};
    CHECK(parse(g, r));
}

TEST_CASE("PAIR_RESULT folds the reserved statuses into 'failed'") {
    // "Значення від 4 зарезервовані; хост показує їх як 3."
    const uint8_t cases[][2] = {{0, PAIR_OK}, {1, PAIR_REJECTED}, {2, PAIR_TIMEOUT},
                                {3, PAIR_FAILED}, {4, PAIR_FAILED}, {200, PAIR_FAILED}};
    for (const auto& c : cases) {
        uint8_t pay[7];
        memcpy(pay, PHONE, 6);
        pay[6] = c[0];
        Frame f{MSG_PAIR_RESULT, 0, pay, sizeof pay};
        PairResult r;
        CAPTURE((int)c[0]);
        REQUIRE(parse(f, r));
        CHECK(r.status == c[1]);
        CHECK(memcmp(r.addr, PHONE, 6) == 0);
    }
}

TEST_CASE("BT_CONFIG reads in the document's field order") {
    uint8_t buf[64];
    Packer  p(buf, sizeof buf);
    p.u8(1); p.u8(0); p.u8(1);
    p.u8(CODEC_BIT_SBC | CODEC_BIT_AAC | CODEC_BIT_APTX | CODEC_BIT_APTX_HD | CODEC_BIT_LHDC_V5);
    p.u8(CODEC_BIT_SBC | CODEC_BIT_AAC);
    p.str("modesp satellite");
    Frame f{MSG_BT_CONFIG, 0, p.data(), p.size()};

    BtConfig c;
    REQUIRE(parse(f, c));
    CHECK(c.radio == 1);
    CHECK(c.discoverable == 0);
    CHECK(c.auto_reconnect == 1);
    CHECK(c.codecs_supported == 0x1F);
    CHECK(c.codecs_enabled == 0x03);
    CHECK(as_string(c.name) == "modesp satellite");

    Frame short_one{MSG_BT_CONFIG, 0, buf, 4};
    CHECK_FALSE(parse(short_one, c));
}

TEST_CASE("BOND_LIST: newest first, and total may be more than the frame holds") {
    const uint8_t second[6] = {0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5};
    uint8_t buf[128];
    Packer  p(buf, sizeof buf);
    p.u8(20);                               // twenty bonds on the satellite ...
    p.u8(2);                                // ... two of them in this frame
    p.bytes(PHONE, 6);  p.u8(BOND_CONNECTED | BOND_LAST); p.str("Pixel 7");
    p.bytes(second, 6); p.u8(0);                          p.str("");
    Frame f{MSG_BOND_LIST, 0, p.data(), p.size()};

    Bond    bonds[BOND_MAX];
    size_t  count = 0;
    uint8_t total = 0;
    REQUIRE(parse_bond_list(f, bonds, BOND_MAX, count, total));
    CHECK(total == 20);
    REQUIRE(count == 2);
    CHECK(memcmp(bonds[0].addr, PHONE, 6) == 0);
    CHECK(bonds[0].flags == (BOND_CONNECTED | BOND_LAST));
    CHECK(as_string(bonds[0].name) == "Pixel 7");
    CHECK(memcmp(bonds[1].addr, second, 6) == 0);
    CHECK(bonds[1].name.len == 0);          // a bond from before 1.8 has no name yet

    // A caller with room for one still learns there were two.
    Bond one[1];
    REQUIRE(parse_bond_list(f, one, 1, count, total));
    CHECK(count == 2);
    CHECK(as_string(one[0].name) == "Pixel 7");

    // A list cut short is refused whole, not read up to the cut.
    Frame cut{MSG_BOND_LIST, 0, p.data(), p.size() - 1};
    CHECK_FALSE(parse_bond_list(cut, bonds, BOND_MAX, count, total));

    // An empty list is a list.
    const uint8_t empty[2] = {0, 0};
    Frame e{MSG_BOND_LIST, 0, empty, sizeof empty};
    REQUIRE(parse_bond_list(e, bonds, BOND_MAX, count, total));
    CHECK(count == 0);
    CHECK(total == 0);
}

TEST_CASE("the 1.8 commands this player sends round-trip") {
    Writer  w;
    Reader  r;
    Catcher got;

    size_t n = build_pair_reply(w, PHONE, true);
    REQUIRE(n > 0);
    r.feed(w.wire(), n, Catcher::on, &got);
    // "Кухня" is ten bytes of UTF-8: the limit is in bytes and this is well inside it.
    n = build_bt_config_set(w, true, false, true, CODEC_BIT_SBC | CODEC_BIT_APTX_HD, "Кухня");
    REQUIRE(n > 0);
    r.feed(w.wire(), n, Catcher::on, &got);
    n = build_bond_remove(w, PHONE);
    r.feed(w.wire(), n, Catcher::on, &got);
    n = build_bond_remove(w, nullptr);
    r.feed(w.wire(), n, Catcher::on, &got);
    n = build_bt_refresh(w);
    REQUIRE(n > 0);
    r.feed(w.wire(), n, Catcher::on, &got);

    REQUIRE(got.types.size() == 5);
    CHECK(got.types == std::vector<uint8_t>{MSG_PAIR_REPLY, MSG_BT_CONFIG_SET, MSG_BOND_REMOVE,
                                            MSG_BOND_REMOVE, MSG_BT_REFRESH});
    auto frame_at = [&](size_t i) {
        return Frame{got.types[i], got.seqs[i], got.payloads[i].data(), got.payloads[i].size()};
    };

    PairReply reply;
    REQUIRE(parse(frame_at(0), reply));
    CHECK(memcmp(reply.addr, PHONE, 6) == 0);
    CHECK(reply.accept == 1);

    BtConfigSet set;
    REQUIRE(parse(frame_at(1), set));
    CHECK(set.radio == 1);
    CHECK(set.discoverable == 0);
    CHECK(set.auto_reconnect == 1);
    CHECK(set.codecs_enabled == (CODEC_BIT_SBC | CODEC_BIT_APTX_HD));
    CHECK(as_string(set.name) == "Кухня");
    CHECK(set.name.len == 10);

    BondRemove one, all;
    REQUIRE(parse(frame_at(2), one));
    CHECK(memcmp(one.addr, PHONE, 6) == 0);
    REQUIRE(parse(frame_at(3), all));
    CHECK(got.payloads[3] == std::vector<uint8_t>{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});

    CHECK(got.payloads[4].empty());         // BT_REFRESH: "(порожньо)"
}

TEST_CASE("a name past 32 bytes is refused whole, never cut") {
    // Cutting could split a UTF-8 character, and the satellite refuses a name that is not
    // UTF-8 and keeps the old one — so a cut name would be a change that silently did not
    // happen. Refusing here says so at the source.
    Writer w;
    const std::string ascii33(33, 'a');
    CHECK(build_bt_config_set(w, true, true, false, CODEC_BIT_SBC, ascii33.c_str()) == 0);
    CHECK(w.seq() == 0);                    // a refused build does not burn a sequence number

    std::string cyr16, cyr17;               // two bytes a letter
    for (int i = 0; i < 16; i++) cyr16 += "ж";
    cyr17 = cyr16 + "ж";
    CHECK(build_bt_config_set(w, true, true, false, CODEC_BIT_SBC, cyr16.c_str()) > 0);   // 32
    CHECK(build_bt_config_set(w, true, true, false, CODEC_BIT_SBC, cyr17.c_str()) == 0);  // 34

    // An empty name and no name both mean "keep the current one": a zero-length string.
    Reader  r;
    Catcher got;
    size_t n = build_bt_config_set(w, true, true, false, CODEC_BIT_SBC, nullptr);
    r.feed(w.wire(), n, Catcher::on, &got);
    REQUIRE(got.payloads.size() == 1);
    CHECK(got.payloads[0] == std::vector<uint8_t>{1, 1, 0, CODEC_BIT_SBC, 0, 0});
}

TEST_CASE("PING and PONG carry the two clocks the contract asks for") {
    Writer  w;
    Reader  r;
    Catcher got;

    size_t n = build_ping(w, 0x11223344u);
    r.feed(w.wire(), n, Catcher::on, &got);
    n = build_pong(w, 0x11223344u, 0x55667788u);
    r.feed(w.wire(), n, Catcher::on, &got);

    REQUIRE(got.types.size() == 2);

    Frame f;
    f.type = got.types[0]; f.payload = got.payloads[0].data(); f.len = got.payloads[0].size();
    Ping pi;
    REQUIRE(parse(f, pi));
    CHECK(pi.tick_ms == 0x11223344u);

    f.type = got.types[1]; f.payload = got.payloads[1].data(); f.len = got.payloads[1].size();
    Pong po;
    REQUIRE(parse(f, po));
    CHECK(po.echo_ms == 0x11223344u);
    CHECK(po.own_ms == 0x55667788u);
}

// ── frozen wire bytes ──────────────────────────────────────────────────────────────────────

namespace {

/// Compares a frame against a literal byte sequence and says where it first differs.
void wire_is(const uint8_t* got, size_t got_n, const std::vector<uint8_t>& want, const char* what)
{
    CAPTURE(what);
    REQUIRE(got_n == want.size());
    for (size_t i = 0; i < want.size(); i++) {
        CAPTURE(i);
        CHECK((int)got[i] == (int)want[i]);
    }
}

}  // namespace

TEST_CASE("frozen wire bytes, so the two sides cannot drift together") {
    // Everything else in this file round-trips this implementation's own Writer through its
    // own Reader, which cannot catch a change both halves make at once. Swap the CRC to
    // big-endian in Writer AND Reader and every other case here stays green while the link
    // fails against the satellite on its first frame.
    //
    // These bytes were computed by a separate implementation, in another language, from the
    // contract rather than from this code. The CRC pair is the point: 5C DF is 0xDF5C written
    // low byte first, so a byte-order change fails here and nowhere else.

    {   // PING, tick 0x11223344, seq 0
        Writer w;
        const size_t n = build_ping(w, 0x11223344u);
        wire_is(w.wire(), n, {0x00, 0x02, 0x02, 0x07, 0x44, 0x33, 0x22, 0x11, 0x5C, 0xDF, 0x00},
                "PING");
    }
    {   // CMD_VOLUME, 64, seq 0
        Writer w;
        const size_t n = build_volume(w, 64);
        wire_is(w.wire(), n, {0x00, 0x02, 0x21, 0x04, 0x40, 0xAE, 0x35, 0x00}, "CMD_VOLUME");
    }
    {   // COVER_RETRY, nonce 0x11223344, generation 5, seq 0 — protocol 1.7. Three of the
        // eight payload bytes are zero, so this pins the COBS runs as well as the layout.
        Writer w;
        const size_t n = build_cover_retry(w, 0x11223344u, 5u);
        wire_is(w.wire(), n,
                {0x00, 0x02, 0x24, 0x06, 0x44, 0x33, 0x22, 0x11, 0x05, 0x01, 0x01, 0x03,
                 0xB5, 0xA1, 0x00},
                "COVER_RETRY");
    }
    {   // STREAM_FMT, 44100 Hz, 16-bit slots, 2 channels, seq 0 — the satellite's direction,
        // and the one message whose payload carries a little-endian u32 worth pinning.
        Writer  w;
        uint8_t buf[8];
        Packer  p(buf, sizeof buf);
        p.u32(44100);
        p.u8(16);
        p.u8(2);
        const size_t n = w.frame(MSG_STREAM_FMT, p.data(), p.size());
        wire_is(w.wire(), n,
                {0x00, 0x02, 0x13, 0x03, 0x44, 0xAC, 0x01, 0x05, 0x10, 0x02, 0x30, 0x2A, 0x00},
                "STREAM_FMT");
    }
    // Protocol 1.8, computed the same way — from the contract, in Python, not from this code.
    {   // PAIR_REPLY, 11:22:33:44:55:66, accept, seq 0
        Writer w;
        const uint8_t addr[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
        const size_t n = build_pair_reply(w, addr, true);
        wire_is(w.wire(), n,
                {0x00, 0x02, 0x25, 0x0A, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x01, 0xD2, 0xBB, 0x00},
                "PAIR_REPLY");
    }
    {   // BT_CONFIG_SET, radio 1, hidden, auto-reconnect, codecs 0x0B, "P4", seq 0 — three
        // zeros in the payload, so the COBS runs are pinned with the layout
        Writer w;
        const size_t n = build_bt_config_set(w, true, false, true, 0x0B, "P4");
        wire_is(w.wire(), n,
                {0x00, 0x02, 0x26, 0x02, 0x01, 0x04, 0x01, 0x0B, 0x02, 0x05, 0x50, 0x34, 0x07,
                 0xD0, 0x00},
                "BT_CONFIG_SET");
    }
    {   // BOND_REMOVE, every bond, seq 0
        Writer w;
        const size_t n = build_bond_remove(w, nullptr);
        wire_is(w.wire(), n,
                {0x00, 0x02, 0x27, 0x09, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x91, 0x1A, 0x00},
                "BOND_REMOVE");
    }
    {   // BT_REFRESH, no payload, seq 0
        Writer w;
        const size_t n = build_bt_refresh(w);
        wire_is(w.wire(), n, {0x00, 0x02, 0x28, 0x03, 0x40, 0x92, 0x00}, "BT_REFRESH");
    }
}

// ── the sequence number, finally read ──────────────────────────────────────────────────────

namespace {

/// A frame with an arbitrary `seq` and a CRC that is actually correct, so the Reader
/// accepts it. hand_frame() exists to forge a WRONG crc; this is its honest twin.
size_t good_frame(uint8_t type, uint8_t seq, const uint8_t* pay, size_t n, uint8_t* out)
{
    uint8_t body[128];
    body[0] = type;
    body[1] = seq;
    memcpy(body + 2, pay, n);
    return hand_frame(type, seq, pay, n, crc16(body, n + 2), out);
}

/// Feeds one good frame carrying `seq` and returns how many the Reader delivered.
size_t feed_seq(Reader& r, Catcher& got, uint8_t seq)
{
    const uint8_t pay[] = {0xAB};
    uint8_t       buf[64];
    const size_t  n = good_frame(MSG_LOG, seq, pay, sizeof pay, buf);
    return r.feed(buf, n, Catcher::on, &got);
}

}  // namespace

TEST_CASE("an unbroken run of sequence numbers reports no loss") {
    Reader  r;
    Catcher got;
    for (int i = 0; i < 20; i++) CHECK(feed_seq(r, got, (uint8_t)i) == 1);
    CHECK(r.seq_gaps() == 0);
}

TEST_CASE("the first frame ever seen is not a gap, whatever its number") {
    // The satellite has been running for hours before we boot, so its counter is wherever
    // it is. Treating the first number as a distance from zero would report 200 lost frames
    // on a healthy link every time the player restarts.
    Reader  r;
    Catcher got;
    CHECK(feed_seq(r, got, 200) == 1);
    CHECK(r.seq_gaps() == 0);
    CHECK(feed_seq(r, got, 201) == 1);
    CHECK(r.seq_gaps() == 0);
}

TEST_CASE("frames that never arrived are counted, one per missing number") {
    Reader  r;
    Catcher got;
    feed_seq(r, got, 10);
    feed_seq(r, got, 11);
    feed_seq(r, got, 15);          // 12, 13, 14 never got here
    CHECK(r.seq_gaps() == 3);
    feed_seq(r, got, 16);
    CHECK(r.seq_gaps() == 3);      // and a clean step adds nothing
}

TEST_CASE("the counter wraps with the sequence instead of reporting 255 lost frames") {
    Reader  r;
    Catcher got;
    feed_seq(r, got, 254);
    feed_seq(r, got, 255);
    feed_seq(r, got, 0);           // the wrap the contract describes
    feed_seq(r, got, 1);
    CHECK(r.seq_gaps() == 0);

    // And a real loss across the wrap is still a real loss.
    feed_seq(r, got, 254);         // 2..253 — 252 frames
    CHECK(r.seq_gaps() == 252);
    feed_seq(r, got, 1);           // 255, 0 — two more, over the boundary
    CHECK(r.seq_gaps() == 254);
}

TEST_CASE("a frame spoiled on our side is a gap as well as a bad CRC") {
    // This is the case the counter was added for. A frame that fails CRC is not delivered,
    // so it never becomes the reference — and the next good frame's number is one further
    // on than it should be. The two counters then say the same thing from opposite ends:
    // bad_crc() that we ruined one, seq_gaps() that the stream is missing one.
    Reader  r;
    Catcher got;
    feed_seq(r, got, 40);

    const uint8_t pay[] = {0xDE, 0xAD};
    uint8_t       buf[64];
    size_t        n = hand_frame(MSG_LOG, 41, pay, sizeof pay, 0x0000, buf);
    CHECK(r.feed(buf, n, Catcher::on, &got) == 0);
    CHECK(r.bad_crc() == 1);
    CHECK(r.seq_gaps() == 0);      // nothing to compare against yet

    feed_seq(r, got, 42);
    CHECK(r.seq_gaps() == 1);      // 41 is missing from the delivered stream, and it is
}

TEST_CASE("a repeated sequence number is not counted as loss") {
    // gap == 0 means the same frame arrived twice, which is not a frame going missing.
    // The satellite's own reader takes the same view (gap > 1), and the two must agree.
    Reader  r;
    Catcher got;
    feed_seq(r, got, 7);
    feed_seq(r, got, 7);
    CHECK(r.seq_gaps() == 0);
    feed_seq(r, got, 8);
    CHECK(r.seq_gaps() == 0);
}

TEST_CASE("reset() drops the reference but keeps the count") {
    Reader  r;
    Catcher got;
    feed_seq(r, got, 100);
    feed_seq(r, got, 103);
    CHECK(r.seq_gaps() == 2);

    r.reset();                     // deliberate resync: what was in flight is gone
    feed_seq(r, got, 200);         // an arbitrary number, and not 97 frames of loss
    CHECK(r.seq_gaps() == 2);
    feed_seq(r, got, 202);
    CHECK(r.seq_gaps() == 3);      // counting resumes from the new reference
}

TEST_CASE("COBS overhead matches the contract at every 254-byte boundary") {
    // The contract states the overhead as ceil(n / 254). An encoder that opens a group after
    // a full one that closed exactly at the end of the input emits one byte more at each
    // multiple of 254 — still decodable by a strict decoder, but one byte off the stated
    // overhead and mis-decoded by any peer that appends the implied zero without checking for
    // end of input. That is a common shape, so it is pinned here rather than trusted.
    struct Case { size_t n, encoded; };
    const Case cases[] = {
        {1,    2},      // 1 + ceil(1/254)
        {253,  254},
        {254,  255},    // the boundary: one code byte, not two
        {255,  257},
        {508,  510},
        {1016, 1020},
        {MAX_FRAME, 1045},
    };

    std::vector<uint8_t> enc(MAX_WIRE + 64), dec(MAX_WIRE + 64);
    for (const auto& c : cases) {
        CAPTURE(c.n);
        const std::vector<uint8_t> src(c.n, 0x5A);      // no zeros: the worst case
        const size_t n = cobs_encode(src.data(), src.size(), enc.data());
        CHECK(n == c.encoded);
        CHECK(n == c.n + (c.n + 253) / 254);            // the contract's own formula
        REQUIRE(cobs_decode(enc.data(), n, dec.data(), dec.size()) == c.n);
        CHECK(memcmp(dec.data(), src.data(), c.n) == 0);
    }
}
