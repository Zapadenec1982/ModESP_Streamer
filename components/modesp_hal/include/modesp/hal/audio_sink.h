/**
 * @file audio_sink.h
 * @brief Semantic seam of the audio subsystem (audio analogue of IDisplayPort).
 *
 * The ABSTRACT player module (PlayerModule) decides WHAT and WHEN to play
 * (clip library, decode, queue, priority) and pushes ready 16-bit mono PCM into
 * an IAudioSink. The sink (Max98357aSink, NullSink, future codecs) only knows
 * HOW to get those samples to the speaker — it owns the I²S channel + DMA and a
 * hardware mute, nothing else. The module never touches I²S; the sink never
 * touches files/decoders/state.
 *
 * Single owner of the audio output: exactly one sink, driven by exactly one
 * playback task inside the player. All sink methods are called from that single
 * task in sequence (init from main task before the task starts) — no cross-task
 * channel races.
 */

#pragma once

#include <cstdint>
#include <cstddef>

namespace modesp::audio {

class IAudioPort;   // player-fork: full-duplex stereo transport (below)

/// Backend capabilities — semantic, queried once by the module.
struct AudioCaps {
    uint32_t max_sample_rate = 48000;
    bool     hw_volume       = false;  ///< false ⇒ player applies software volume
};

/// Low-level PCM transport to an I²S amplifier (16-bit signed mono).
class IAudioSink {
public:
    virtual ~IAudioSink() = default;

    /// One-time hardware init (channel/GPIO). false ⇒ audio disabled, system lives.
    virtual bool init() { return true; }

    virtual AudioCaps caps() const { return {}; }

    /// Prepare the channel for a stream at `sample_rate_hz` and unmute the amp.
    /// Called before a run of write()s. Returns false on hardware error.
    virtual bool start(uint32_t sample_rate_hz) = 0;

    /// Push `frames` mono samples; blocks (paced by the DMA) up to `timeout_ms`.
    /// Returns the number of frames actually written.
    virtual size_t write(const int16_t* pcm, size_t frames, uint32_t timeout_ms) = 0;

    /// Stop the stream and mute the amp (idle silence).
    virtual void stop() = 0;

    virtual bool is_healthy() const = 0;
    virtual const char* name() const = 0;

    /// player-fork: capability cast (no RTTI) — a backend that is a full-duplex stereo
    /// I²S transport returns its IAudioPort here; the audio-core module pulls/pushes
    /// 32-bit frames through it. Default: sink only.
    virtual IAudioPort* as_port() { return nullptr; }
};

// ── player-fork: full-duplex stereo transport seam ──────────────────────────
// One I²S controller = one clock domain: on a master port write() and read() are
// paced by the same BCLK/WS, so the DAC output and the ADC input never drift
// against each other (docs/CONCEPT.md, "Clock domains"). A slave port (external master such as
// the BT module) is read-only; its true rate is whatever the peer clocks, and the
// audio core measures it from the frame count.

struct AudioPortCaps {
    uint32_t sample_rate_hz = 48000;   ///< the rate it starts at (slave: what the peer clocks)
    /// What the clock can be moved to, since it moves with the source now.
    ///
    /// Not a property of the DAC, which takes almost anything: a property of what the P4's
    /// audio PLL can make at this MCLK multiple. The port is asked before a retune so that
    /// a 96 kHz file is played at 96 kHz and a rate nothing can make is converted instead
    /// of being attempted and failing mid-track.
    uint32_t min_rate_hz    = 8000;
    uint32_t max_rate_hz    = 192000;
    uint8_t  slot_bits      = 32;      ///< I²S slot width; samples travel as int32, left-justified
    /// MCLK as a multiple of the sample rate. A master's, and on the P4 it decides where the
    /// audio PLL sits — at twice this MCLK — which in turn decides what a SLAVE port sharing
    /// that PLL can be clocked at. So it is not decoration: see i2s_clock.h.
    uint16_t mclk_multiple  = 256;
    /// Frames the port holds between write() and the pins — its DMA queue. What a caller
    /// needs to say how late a frame written now will be heard; 0 when the port does not say.
    uint32_t tx_queue_frames = 0;
    bool     has_tx         = false;
    bool     has_rx         = false;
    bool     is_master      = true;
};

struct AudioPortDiagnostics {
    // IDF completion-queue overflows: evidence that servicing was late. They do
    // not by themselves prove a stopped BCLK or identify audible samples.
    uint32_t rx_queue_overflows = 0, tx_queue_overflows = 0;
    uint32_t rx_short_reads = 0, tx_short_writes = 0;
    uint32_t trim_attempts = 0, trim_applied = 0;
    // Receive DMA completions the driver was told about, and the frames in each. IDF hands
    // over only the LAST finished block per interrupt, so completions that arrive faster
    // than the interrupt is served vanish without an overflow: blocks x frames falling short
    // of the rate is that loss, and nothing else counts it.
    uint32_t rx_dma_blocks = 0, rx_dma_block_frames = 0;
    // The hardware's own word, sampled at every receive block: DMA receive FIFOs that
    // overflowed and transmit FIFOs that ran dry (any channel), and this port's I2S receive
    // FIFO hung for want of DMA. Sticky bits read and cleared, so bursts count once.
    uint32_t hw_rx_fifo_overflows = 0, hw_tx_fifo_underflows = 0, hw_rx_hung = 0;
};

/// The three pins of a peer's I2S link: its bit clock, its frame clock and its data line.
/// -1 when absent. Named from the link's point of view, not from any port's.
struct LinkPins {
    int bclk = -1, ws = -1, din = -1;
    bool valid() const { return bclk >= 0 && ws >= 0 && din >= 0; }
};

class IAudioPort {
public:
    virtual ~IAudioPort() = default;

    /// Create the channels (idle, no clocks yet). false ⇒ port unusable, system lives.
    virtual bool init() = 0;
    virtual AudioPortCaps port_caps() const = 0;
    virtual AudioPortDiagnostics diagnostics() const { return {}; }

    /// Start clocks/DMA. TX sends silence until the first write().
    virtual bool start() = 0;

    /// Retune the master clock to `hz`, keeping everything else.
    ///
    /// The output rate follows whatever is playing rather than being fixed, so that a
    /// 44.1 kHz source does not have to be converted to 48 — the conversion costs more CPU
    /// than decoding the source did. Returns false when the port cannot do it (a slave
    /// port has no clock of its own to change).
    ///
    /// `mclk_multiple` of 0 means the board's own. Anything else is asking for a different
    /// audio PLL frequency — the PLL is twice this MCLK, it is shared with every other I2S
    /// channel on the chip, and raising it is how a SLAVE port on the same PLL is given
    /// enough source clock for wide slots (see i2s_clock.h). It also lowers the master's own
    /// ceiling, so it is asked for per format rather than configured once.
    virtual bool set_rate(uint32_t hz, uint16_t mclk_multiple) {
        (void)hz; (void)mclk_multiple; return false;
    }

    /// Adopt a format a peer announced: sample rate AND slot width together.
    ///
    /// A SLAVE port has no clock to retune, but it still has to be told what is coming: the
    /// peripheral counts bit-clock cycles per slot from its own configuration, so a channel
    /// opened for 16-bit slots reading a 32-bit stream assembles one frame out of two and
    /// glues the two channels into one word. That is not a clock fault and set_rate() cannot
    /// fix it.
    ///
    /// The caller is responsible for the silence: this stops the channel, reconfigures it and
    /// starts it again, so whatever is in the DMA is lost. Returns false when the port cannot
    /// take that combination, in which case it is left running as it was.
    virtual bool set_format(uint32_t hz, uint8_t slot_bits) {
        (void)hz; (void)slot_bits; return false;
    }
    /// The slot width it is running at now, in bits.
    virtual uint8_t slot_bits() const { return 0; }

    // ── trimming the clock, in parts per million ──
    //
    // A rate the port is SET to is a nominal one: two crystals nominally at the same
    // frequency differ by tens of parts per million, and over a queue that difference is a
    // drift. Where the peer owns the clock — the Bluetooth satellite is the I2S master and
    // this panel its slave — the choice is to absorb that drift with a resampler, which
    // costs about a quarter of a core and is never bit-exact, or to move this port's own
    // clock the same few ppm and absorb it for nothing (docs/AUDIO.md, "Playback").

    /// Whether the clock can be trimmed right now. It is a question about the moment, not
    /// about the hardware: on this chip the audio PLL can only be retuned while exactly one
    /// other peripheral holds it, and who holds it changes with the format being played.
    virtual bool can_trim() const { return false; }

    /// Trim the clock by `ppm` around the rate set_rate() last established. `ppm` of 0 puts
    /// it back exactly on nominal. `applied_ppm`, when given, receives what was actually
    /// achieved — the synthesiser is quantised and a request lands on the nearest step it
    /// has, which the caller should report rather than assume.
    ///
    /// Returns false when the port would not do it, in which case the clock has not moved.
    virtual bool trim_ppm(int32_t ppm, int32_t* applied_ppm) {
        (void)ppm; (void)applied_ppm; return false;
    }

    /// Whether this port's line input is in use: its MCLK pin driven and its receive DMA
    /// running. A duplex master otherwise keeps both going for an ADC nobody is listening to
    /// — the fastest edge on the header, and a second receive stream on the same DMA as the
    /// Bluetooth input. Off holds MCLK low and disables the receive channel; the transmit
    /// side is untouched. No-op for a port with neither.
    virtual void set_line_input(bool on) { (void)on; }

    /// The pins this port reads a peer's link on (an input port only).
    virtual LinkPins link_pins() const { return {}; }

    /// Clock a peer's link from THIS port: its bit and frame clocks also driven out on
    /// `pins->bclk`/`pins->ws`, and its receive side fed from `pins->din`, so the peer's
    /// data arrives on the very clock that plays it. nullptr hands the pins back as inputs.
    /// Only for a duplex master; false when this port cannot. The caller must know the peer
    /// is not driving those lines itself (protocol 1.9, DEV_CAP_I2S_SLAVE).
    virtual bool set_link_master(const LinkPins* pins) { (void)pins; return false; }

    /// What it is running at now.
    virtual uint32_t rate() const = 0;
    /// Push `frames` interleaved stereo int32 frames; blocks (DMA-paced) up to timeout.
    virtual size_t write(const int32_t* stereo, size_t frames, uint32_t timeout_ms) = 0;
    /// Pull `frames` interleaved stereo int32 frames; blocks up to timeout. 0 if !has_rx.
    virtual size_t read(int32_t* stereo, size_t frames, uint32_t timeout_ms) = 0;
    virtual void stop() = 0;

    virtual bool is_healthy() const = 0;
    virtual const char* name() const = 0;
};

} // namespace modesp::audio
