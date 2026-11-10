/**
 * @file test_stream_pos.cpp
 * @brief How long a stream is, and where a second lives in it.
 *
 * Three functions, all arithmetic, all of them answering a question a progress bar asks.
 * They are tested rather than trusted because one of them has a trap in it that already
 * cost this project once: the decoder reports an MP3's bitrate in KILOBITS while the field
 * is documented in bits, and believing it turns a four-minute song into seventy-seven
 * hours. That case is pinned below with the numbers that produced it.
 *
 * The card player derived this first. The stream engine needs the same answers the moment a
 * server sends Content-Length, so the arithmetic moved into components/audio_pipe and these
 * tests came with it — one copy of the formula, one copy of the trap, one place to fix.
 */

#include "doctest.h"
#include "audio_pipe.h"

#include <cstring>
#include <initializer_list>

TEST_CASE("a bitrate reported in kilobits is read as kilobits") {
    // The case that produced the bug: a 200 kbit/s MP3 whose decoder reports 201.
    CHECK(audio_core::stream_bps(201, 44100, 2, 16) == 201000);
    // And the same field when it means what it says.
    CHECK(audio_core::stream_bps(128000, 44100, 2, 16) == 128000);
    // The boundary is 10 kbit/s: nothing this firmware decodes runs below it, so anything
    // smaller can only be the other unit.
    CHECK(audio_core::stream_bps(9999, 44100, 2, 16) == 9999000);
    CHECK(audio_core::stream_bps(10000, 44100, 2, 16) == 10000);
}

TEST_CASE("PCM reports no bitrate, and its rate times its frame is one") {
    CHECK(audio_core::stream_bps(0, 44100, 2, 16) == 44100u * 2 * 16);
    CHECK(audio_core::stream_bps(0, 48000, 1, 24) == 48000u * 1 * 24);
}

TEST_CASE("a four-minute track is four minutes long") {
    // 201 kbit/s — what the decoder reports for a nominal 200 — for 240 s.
    const uint64_t bytes = 6030000;
    const uint32_t bps = audio_core::stream_bps(201, 44100, 2, 16);
    CHECK(audio_core::stream_duration_s(bytes, bps) == 240);

    // What the trap did: the same file measured against 201 BITS per second. Kept as a
    // number rather than a comment, so a regression reads as "sixty-six hours" rather than
    // as "wrong somewhere".
    CHECK(audio_core::stream_duration_s(bytes, 201) == 240000);
}

TEST_CASE("a length that cannot be known is zero, not a guess") {
    CHECK(audio_core::stream_duration_s(0, 128000) == 0);       // a stream with no end
    CHECK(audio_core::stream_duration_s(6000000, 0) == 0);      // no bitrate yet
    CHECK(audio_core::stream_duration_s(6000000, 4) == 0);      // a bitrate too absurd to use
}

TEST_CASE("a seek lands at the same fraction of the bytes as of the seconds") {
    const uint64_t bytes = 6000000;
    const int32_t  dur   = 240;

    CHECK(audio_core::stream_seek_byte(bytes, 0, dur, 0, 0) == 0);
    CHECK(audio_core::stream_seek_byte(bytes, 120, dur, 0, 0) == 3000000);
    CHECK(audio_core::stream_seek_byte(bytes, 60, dur, 0, 0) == 1500000);
}

TEST_CASE("a seek past either end stays inside the track") {
    const uint64_t bytes = 6000000;
    const int32_t  dur   = 240;

    CHECK(audio_core::stream_seek_byte(bytes, -5, dur, 0, 0) == 0);
    // The last second, not one past it: a byte offset equal to the length is the end of the
    // file, and a GET from there returns nothing at all.
    CHECK(audio_core::stream_seek_byte(bytes, 240, dur, 0, 0) ==
          audio_core::stream_seek_byte(bytes, 239, dur, 0, 0));
    CHECK(audio_core::stream_seek_byte(bytes, 10000, dur, 0, 0) < bytes);
}

TEST_CASE("PCM lands on a frame boundary, because half a frame is noise") {
    // 16-bit stereo: four bytes to a frame. A length and a position chosen so the naive
    // answer is deliberately off by two.
    const uint64_t bytes = 1000002;
    const int32_t  dur   = 7;
    const uint64_t off = audio_core::stream_seek_byte(bytes, 3, dur, 16, 2);
    CHECK(off % 4 == 0);

    // 24-bit mono is three bytes to a frame, which is the case where "round to a power of
    // two" would have looked right and been wrong.
    const uint64_t off24 = audio_core::stream_seek_byte(bytes, 3, dur, 24, 1);
    CHECK(off24 % 3 == 0);
}

TEST_CASE("nothing to seek in is offset zero") {
    CHECK(audio_core::stream_seek_byte(0, 30, 240, 16, 2) == 0);      // length unknown
    CHECK(audio_core::stream_seek_byte(6000000, 30, 0, 16, 2) == 0);  // duration unknown
}

TEST_CASE("the input limit never exceeds the buffer the input is staged in") {
    // The player stages CHUNK frames of float stereo and resamples into twice that. The
    // formula this replaced bounded the input by the OUTPUT buffer alone, which is only
    // safe while the ratio is at least two: at 44.1 -> 48 kHz, ratio 1.0884, it asked for
    // 1874 frames of a 1024-frame buffer and wrote 6800 bytes past the end of a PSRAM
    // block. That is the whole reason this function exists, so it is pinned first.
    constexpr size_t CHUNK = 1024;
    CHECK(audio_core::resample_input_limit(CHUNK, CHUNK * 2, 48000.0 / 44100.0) == CHUNK);

    // Both capacities bind, whichever is tighter. Below ratio ~2 the input buffer is the
    // limit; above it the output buffer is, and the answer has to shrink with the ratio.
    for (double ratio : {0.5, 1.0, 1.0884, 1.5, 2.0, 2.1769, 4.0, 6.0}) {
        CAPTURE(ratio);
        const size_t n = audio_core::resample_input_limit(CHUNK, CHUNK * 2, ratio);
        CHECK(n <= CHUNK);                                  // fits where it is written
        CHECK((double)n * ratio <= (double)(CHUNK * 2));    // fits where it is converted to
        CHECK(n > 0);                                       // and always makes progress
    }

    // 22.05 -> 48 kHz is the case the output buffer really does bind, and the one the
    // original formula got right — it must keep getting it right.
    CHECK(audio_core::resample_input_limit(CHUNK, CHUNK * 2, 48000.0 / 22050.0) < CHUNK);
}

TEST_CASE("an input limit with nowhere to put the output is zero, not a guess") {
    constexpr size_t CHUNK = 1024;
    CHECK(audio_core::resample_input_limit(0, CHUNK * 2, 1.0) == 0);   // nothing to stage
    CHECK(audio_core::resample_input_limit(CHUNK, 8, 1.0) == 0);       // no room for margin
    CHECK(audio_core::resample_input_limit(CHUNK, CHUNK * 2, 0.0) == 0);
    CHECK(audio_core::resample_input_limit(CHUNK, CHUNK * 2, -1.0) == 0);
}

TEST_CASE("a FLAC says how long it is in its first bytes") {
    // "fLaC", a STREAMINFO header (last block, 34 bytes), block and frame sizes, then
    // 44,100 Hz, 2 channels, 16 bits and 10,584,000 samples: four minutes.
    uint8_t head[42] = {'f', 'L', 'a', 'C', 0x80, 0x00, 0x00, 0x22,
                        0x10, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                        0x0A, 0xC4, 0x42, 0xF0, 0x00, 0xA1, 0x7F, 0xC0};
    CHECK(audio_core::flac_duration_s(head, sizeof(head)) == 240);
    CHECK(audio_core::flac_duration_s(head, 25) == 0);            // not all of it yet

    uint8_t no_total[42];
    memcpy(no_total, head, sizeof(head));
    no_total[21] &= 0xF0; no_total[22] = no_total[23] = no_total[24] = no_total[25] = 0;
    CHECK(audio_core::flac_duration_s(no_total, sizeof(no_total)) == 0);   // the encoder left it out

    uint8_t mp3[42] = {'I', 'D', '3'};
    CHECK(audio_core::flac_duration_s(mp3, sizeof(mp3)) == 0);
    CHECK(audio_core::flac_duration_s(nullptr, 42) == 0);
}
