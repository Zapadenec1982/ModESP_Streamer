/**
 * @file usb_uac.h
 * @brief The panel as a USB sound card: USB Audio Class 2 on the P4's high-speed port.
 *
 * docs/CONCEPT.md, "USB": the HS Type-C is wired as a device, so USB is a UAC2 device — 44.1, 48,
 * 88.2 and 96 kHz, 16 or 24 bits, asynchronous with a feedback endpoint so that the panel's clock
 * sets the pace and nothing resamples. UAC2 is in every desktop OS's own driver (Windows 10 1703+,
 * macOS, Linux).
 *
 * USB is audio.source 7 (docs/CONCEPT.md, "`audio.source` values"). While the module says USB is
 * the output (set_output), a producer task writes what the computer sends into the player's ring
 * as OWNER_USB, asks the ring for the computer's rate — a new session whenever it changes — and
 * steers the computer by the ring's fill: the feedback endpoint asks for a little more or a
 * little less than nominal, so the ring stays where the consumer wants it however far the two
 * clocks are apart. While USB is not the output the stream is drained and measured, and dropped.
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace usb_uac {

struct Config {
    const char* product  = nullptr;   ///< iProduct, UTF-8 — the panel's one name
    const char* serial   = nullptr;   ///< iSerialNumber, ASCII
    int         core     = 1;         ///< the controller's interrupt and both tasks
    int         priority = 20;        ///< TinyUSB's task: control requests only
    int         producer_priority = 23;   ///< above the audio task: it moves 1 ms at a time
};

enum class State : uint8_t {
    off,        ///< not started, or the controller would not come up
    waiting,    ///< no host — or a suspended bus, which is what an unplug looks like here
    ready,      ///< configured by a host, nothing streaming
    playing,    ///< an alternate setting with data is selected
};

/// Where the producer is with the ring.
enum class Phase : uint8_t {
    idle,       ///< not the output, or nothing streaming
    flush,      ///< what is queued fades out, so the consumer primes again
    ask,        ///< asked the ring for the computer's rate, waiting for the clock
    run,        ///< writing
    refused,    ///< the clock would not go to this rate: silence until the rate changes
};

/// One second of the stream; totals are since start().
struct Stats {
    State    state        = State::off;
    Phase    phase        = Phase::idle;
    uint32_t rate         = 0;     ///< Hz, as the host set the clock
    uint8_t  bits         = 0;     ///< 16 or 24 while playing
    bool     output       = false; ///< USB is the selected source
    bool     signal       = false; ///< audible audio in the last 300 ms
    uint32_t rx_frames    = 0;     ///< frames the host sent in the last second
    uint32_t written      = 0;     ///< frames written into the ring in the last second
    uint32_t dropped      = 0;     ///< total frames not written: not the output, ring full, clock settling
    uint32_t overflows    = 0;     ///< total times the ring was full
    uint32_t fill_min_ms  = 0;     ///< ring + receive FIFO over the last second
    uint32_t fill_max_ms  = 0;
    uint32_t target_ms    = 0;     ///< the fill the feedback steers to
    int32_t  fb_ppm       = 0;     ///< feedback against nominal, the last second's mean
    float    peak_db[2]   = {-120.f, -120.f};   ///< dBFS, left and right
    float    tone_hz      = 0;     ///< left channel; 0 when it is not one clean tone
    uint32_t glitches     = 0;     ///< total breaks in a clean tone — lost or repeated samples
    int16_t  host_volume  = 0;     ///< 1/256 dB, what the host's slider says
    bool     host_mute    = false;
    uint32_t streams      = 0;     ///< total alternate-setting changes
    uint32_t pkts         = 0;     ///< data packets in the last second; 8000 at high speed
    uint32_t lost         = 0;     ///< total microframes with no packet taken
    uint32_t loss_events  = 0;     ///< total times packets were lost
    uint32_t same_frame   = 0;     ///< total packets the controller had held for a late interrupt
    uint32_t gap_max_us   = 0;     ///< longest gap between packet interrupts in the last second
    int      core         = -1;    ///< where the interrupt and the tasks run
};

/// Start the device. Returns false if the PHY or the stack would not come up.
bool  start(const Config& cfg);
bool  started();
/// USB is the selected source: write into the ring (claiming it) or stop (releasing it).
void  set_output(bool on);
/// Audible audio from the host in the last 300 ms — what the module switches the source on.
bool  signal();
/// The last completed second.
Stats stats();

/// The host's volume as it stands now, 1/256 dB. `Stats::host_volume` is the same number as
/// of the last once-a-second report, which is too slow to follow a slider.
int16_t host_volume();
const char* state_name(State s);
const char* phase_name(Phase p);
/// The last loss events, newest first: "t=123.456s 3 (412 us); ...". Returns the length written.
size_t loss_log(char* out, size_t n, unsigned max_events);

}  // namespace usb_uac
