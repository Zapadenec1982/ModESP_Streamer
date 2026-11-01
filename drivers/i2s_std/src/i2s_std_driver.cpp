/**
 * @file i2s_std_driver.cpp
 * @brief I²S standard-mode transport over the IDF i2s_std driver.
 *
 * DMA-SRAM budget (32-bit stereo = 8 B/frame, 480 frames = 3840 B per descriptor):
 *   duplex port: 8 descriptors × 2 directions = 61 KB (80 ms each — one chan_cfg serves
 *   both channels of a full-duplex pair); RX-only slave port: 6 descriptors of 240 frames
 *   = 11.5 KB, which is what it costs at EITHER slot width (see RX_DMA_FRAMES).
 * The TX ring must outlast a flash sector erase (Boya BY25Q has no auto-suspend, both
 * caches stall) so the DAC never runs dry; an RX stall only loses a few input samples.
 * Measured on the bench: the whole audio core costs ~110 KB of DMA-SRAM (rings + ASRC + stacks).
 */

#include "i2s_std_driver.h"

#include "esp_heap_caps.h"
#include "esp_timer.h"

#include <cstring>
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_private/gpio.h"     // gpio_func_sel, as IDF's own I2S driver uses it
#include "soc/io_mux_reg.h"       // PIN_FUNC_GPIO
#include "esp_rom_gpio.h"
#include "soc/gpio_sig_map.h"
#include "soc/i2s_periph.h"
#include "soc/i2s_struct.h"
#include "soc/ahb_dma_struct.h"
// The audio PLL's reference count, which trim_ppm() has to steer before it may move the PLL.
// A public header of esp_hw_support, and the only handle anyone has on that counter.
#include "clk_ctrl_os.h"

#include "i2s_clock.h"   // the arithmetic that decides what this chip can be clocked at

static const char* TAG = "I2sStd";

static_assert(std::atomic<uint32_t>::is_always_lock_free, "ISR counters must be lock-free");
static bool IRAM_ATTR count_queue_overflow(i2s_chan_handle_t, i2s_event_data_t*, void* ctx) {
    static_cast<std::atomic<uint32_t>*>(ctx)->fetch_add(1, std::memory_order_relaxed);
    return false;
}
static bool IRAM_ATTR count_rx_overflow(i2s_chan_handle_t, i2s_event_data_t*, void* ctx) {
    static_cast<I2sStdDriver::RxEvents*>(ctx)->overflows.fetch_add(1, std::memory_order_relaxed);
    return false;
}
static bool IRAM_ATTR count_rx_block(i2s_chan_handle_t, i2s_event_data_t*, void* ctx) {
    auto* e = static_cast<I2sStdDriver::RxEvents*>(ctx);
    e->blocks.fetch_add(1, std::memory_order_relaxed);
    // What the hardware itself says, read at every block: a DMA receive FIFO that overflowed
    // or a transmit FIFO that ran dry on ANY AHB-DMA channel (this port does not know which
    // channel IDF gave it), and this port's own I2S receive FIFO hung waiting for the DMA.
    // All are sticky raw bits nothing else here enables, so read-and-clear counts each burst.
    for (int ch = 0; ch < 3; ch++) {
        if (AHB_DMA.in_intr[ch].raw.infifo_ovf_chn_int_raw) {
            AHB_DMA.in_intr[ch].clr.val = 1u << 5;          // infifo_ovf_chn_int_clr
            e->hw_rx_fifo_ovf.fetch_add(1, std::memory_order_relaxed);
        }
        if (AHB_DMA.out_intr[ch].raw.outfifo_udf_chn_int_raw) {
            AHB_DMA.out_intr[ch].clr.val = 1u << 5;         // outfifo_udf_chn_int_clr
            e->hw_tx_fifo_udf.fetch_add(1, std::memory_order_relaxed);
        }
    }
    i2s_dev_t* const dev = e->port == 0 ? &I2S0 : e->port == 1 ? &I2S1 : e->port == 2 ? &I2S2 : nullptr;
    if (dev && dev->int_raw.rx_hung_int_raw) {
        dev->int_clr.val = 1u << 2;                          // rx_hung_int_clr
        e->hw_rx_hung.fetch_add(1, std::memory_order_relaxed);
    }
    return false;
}

modesp::audio::AudioPortDiagnostics I2sStdDriver::diagnostics() const {
    modesp::audio::AudioPortDiagnostics d{
        rx_events_.overflows.load(std::memory_order_relaxed), tx_ovf_.load(std::memory_order_relaxed),
        rx_short_.load(std::memory_order_relaxed), tx_short_.load(std::memory_order_relaxed),
        trim_attempts_.load(std::memory_order_relaxed), trim_applied_.load(std::memory_order_relaxed)};
    d.rx_dma_blocks = rx_events_.blocks.load(std::memory_order_relaxed);
    d.rx_dma_block_frames = rx_ ? (tx_ ? DMA_FRAMES : RX_DMA_FRAMES) : 0;
    d.hw_rx_fifo_overflows  = rx_events_.hw_rx_fifo_ovf.load(std::memory_order_relaxed);
    d.hw_tx_fifo_underflows = rx_events_.hw_tx_fifo_udf.load(std::memory_order_relaxed);
    d.hw_rx_hung            = rx_events_.hw_rx_hung.load(std::memory_order_relaxed);
    return d;
}

void I2sStdDriver::configure(const char* role, const modesp::I2SBusResource* bus) {
    role_ = role;
    bus_ = bus;
    configured_ = (bus != nullptr);
}

// The rates this BOARD can actually clock, which is narrower than either datasheet
// suggests and was established on the bench rather than derived.
//
// The floor is the DAC's, not the P4's. PCM5102A takes no MCLK here — it locks its
// internal PLL to BCLK (docs/CONCEPT.md, "DAC — PCM5102A") — and at 22.05 kHz with 32-bit slots
// BCLK is only 1.41 MHz. I2S accepted that rate and reported success; the DAC stayed
// silent. So the floor is where the DAC is known to lock, and moving it down needs a bench
// test, not an argument.
//
// The ceiling is the audio PLL's: at 256x MCLK the driver wants a source clock of more
// than twice MCLK, and the PLL tops out at 125 MHz, so 192 kHz (MCLK 49.15 MHz, wanting
// 98 MHz) is the last rate that fits. Above it the source converts, which is now the only
// thing the resampler is for. A rate that cannot be made is REFUSED, loudly and before a
// note is played, so the ceiling is a guard rather than a guess.
using audio_core::I2S_XTAL_HZ;      // the crystal, and I2S_CLK_SRC_DEFAULT with it here

// ── The audio PLL, which IDF owns and will not talk about ─────────────────────────────────
//
// It is ONE frequency for the whole chip: the first holder sets it, and while a second one is
// attached it is frozen — every further request is answered with the frequency already there
// and a warning, not an error (clk_ctrl_os.c, periph_rtc_apll_freq_set; i2s_common.c,
// i2s_set_get_apll_freq). IDF exposes neither the frequency nor the count.
//
// Both I2S ports on this panel are instances of this file, so this is the one place that can
// honestly know the number — and knowing it is the difference between REFUSING a format and
// asking IDF for one it will reject. A rejected request is not free: reconfig_std_clock
// acquires the PLL reference BEFORE the clock calculation can fail, and records the new source
// only after it succeeds (i2s_std.c:386-398), so every failed attempt leaves a reference
// behind. Two of those and the master can never retune again — silently, for the rest of the
// uptime. That is what pinned the output at 44.1 kHz while the satellite announced 48.
static uint32_t s_apll_hz    = 0;      ///< what the master last set it to; 0 = nobody has
static bool     s_apll_slave = false;  ///< a slave port holds it too, so it cannot be moved

constexpr uint32_t MIN_RATE_HZ = 44100;
constexpr uint32_t MAX_RATE_HZ = 192000;

modesp::audio::AudioCaps I2sStdDriver::caps() const {
    modesp::audio::AudioCaps c;
    // What it CAN do, not what it happens to be doing. Reporting the current rate as the
    // maximum was harmless while the rate never moved; it is a lie now that it does.
    c.max_sample_rate = MAX_RATE_HZ;
    c.hw_volume = false;
    return c;
}

modesp::audio::AudioPortCaps I2sStdDriver::port_caps() const {
    modesp::audio::AudioPortCaps c;
    if (bus_) {
        c.sample_rate_hz = bus_->sample_rate_hz;
        c.min_rate_hz    = MIN_RATE_HZ;
        c.max_rate_hz    = MAX_RATE_HZ;
        c.slot_bits      = bus_->slot_bits;
        c.mclk_multiple  = bus_->mclk_multiple ? bus_->mclk_multiple : 256;
        c.has_tx         = bus_->dout != GPIO_NUM_NC;
        c.has_rx         = bus_->din  != GPIO_NUM_NC;
        c.is_master      = bus_->master;
        c.tx_queue_frames = c.has_tx ? TX_DMA_DESCS * DMA_FRAMES : 0;
    }
    return c;
}

bool I2sStdDriver::init() {
    if (!configured_) {
        ESP_LOGE(TAG, "Driver not configured");
        return false;
    }
    if (initialized_) return true;

    const bool want_tx = bus_->dout != GPIO_NUM_NC;
    const bool want_rx = bus_->din  != GPIO_NUM_NC;
    if (!want_tx && !want_rx) {
        ESP_LOGE(TAG, "[%s] neither dout nor din set", role_.c_str());
        return false;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(
        (i2s_port_t)bus_->port, bus_->master ? I2S_ROLE_MASTER : I2S_ROLE_SLAVE);
    chan_cfg.dma_frame_num = want_tx ? DMA_FRAMES   : RX_DMA_FRAMES;
    chan_cfg.dma_desc_num  = want_tx ? TX_DMA_DESCS : RX_DMA_DESCS;   // RX-only port: shallow ring
    chan_cfg.auto_clear_after_cb = true;   // underrun → zeros, never stale audio
    // IDF passes on only the LAST finished DMA block per interrupt (i2s_common.c,
    // i2s_dma_rx_callback: rx_eof_desc_addr), so an interrupt served later than one block
    // loses the block before it without a word. At 96 kHz an RX block is 1.25 ms. Level 3
    // lets this interrupt pre-empt the level-1 crowd (UART to the satellite, SDIO, console).
    chan_cfg.intr_priority = 3;

    // Full duplex = one call with both handles: the channels share the controller clocks.
    esp_err_t err = i2s_new_channel(&chan_cfg, want_tx ? &tx_ : nullptr, want_rx ? &rx_ : nullptr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[%s] i2s_new_channel(port %d): %s", role_.c_str(), (int)bus_->port, esp_err_to_name(err));
        return false;
    }

    i2s_std_config_t std_cfg = {};
    std_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(bus_->sample_rate_hz);
    // Clock source BY ROLE, and the distinction matters more than it looks.
    //
    // A master drives the DAC and the ADC, and the default source on this P4 (40 MHz XTAL;
    // PLL_F160M needs hw_ver3) divides to 12.288 MHz only fractionally — 40/12.288 = 3.2552 —
    // so MCLK, and the BCK derived from it, carry cycle-to-cycle jitter of tens of ns. A DAC
    // that recovers its clock from BCK (PCM5102A with SCK grounded) can fail to lock on that,
    // and the ADC's sigma-delta modulator samples on exactly that edge. The APLL synthesises
    // 12.288 MHz exactly, so a master takes the APLL.
    //
    // A SLAVE takes BCLK and WS off the wire and synthesises nothing — its internal clock only
    // has to oversample what arrives. Putting it on the APLL buys no jitter and costs the one
    // thing set_rate() depends on: every channel opened with clk_src == APLL calls
    // periph_rtc_apll_acquire, and clk_ctrl_os freezes the frequency once the reference count
    // reaches two. set_rate() returns early for a slave, so a slave never releases. Binding
    // I2S-B took the count to three and silently ended every retune — 96 and 192 kHz playback
    // went back through the converter, and on the refusal path the master's own RX stayed
    // parked on the crystal for the rest of the uptime.
    //
    // The XTAL is comfortable here: 44.1 kHz in 16-bit slots needs MCLK of at least 8 x BCLK,
    // which is 11.29 MHz, against a source above twice that. 40 MHz clears it. The "sample
    // rate is too large" refusal seen on the bench was at 48 kHz in 32-BIT slots, where the
    // requirement is above 49 MHz — a different case, not this one.
    std_cfg.clk_cfg.clk_src = bus_->master ? I2S_CLK_SRC_APLL : I2S_CLK_SRC_DEFAULT;
    switch (bus_->mclk_multiple) {
        case 128: std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_128; break;
        case 384: std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_384; break;
        case 512: std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_512; break;
        default:  std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256; break;
    }
    slot_bits_ = (bus_->slot_bits == 32) ? 32 : 16;
    std_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        slot_bits_ == 32 ? I2S_DATA_BIT_WIDTH_32BIT : I2S_DATA_BIT_WIDTH_16BIT,
        I2S_SLOT_MODE_STEREO);
    std_cfg.gpio_cfg.mclk = bus_->mclk != GPIO_NUM_NC ? bus_->mclk : I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.bclk = bus_->bclk;
    std_cfg.gpio_cfg.ws   = bus_->ws;
    std_cfg.gpio_cfg.dout = want_tx ? bus_->dout : I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.din  = want_rx ? bus_->din  : I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.invert_flags.mclk_inv = 0;
    std_cfg.gpio_cfg.invert_flags.bclk_inv = 0;
    std_cfg.gpio_cfg.invert_flags.ws_inv   = 0;

    if (tx_ && (err = i2s_channel_init_std_mode(tx_, &std_cfg)) != ESP_OK) {
        ESP_LOGE(TAG, "[%s] tx init: %s", role_.c_str(), esp_err_to_name(err));
        return false;
    }
    if (rx_ && (err = i2s_channel_init_std_mode(rx_, &std_cfg)) != ESP_OK) {
        ESP_LOGE(TAG, "[%s] rx init: %s", role_.c_str(), esp_err_to_name(err));
        return false;
    }
    // A MASTER's channels came up on the PLL, so both hold a reference to it. set_rate() has
    // to know, because two holders are one too many to retune it — and trim_ppm() has to know
    // for the opposite reason: with fewer than two the retune is not refused, it aborts.
    //
    // A slave opened on the crystal holds NOTHING, and saying otherwise here used to be
    // harmless only because set_rate() refuses a slave before it reads these. apll_refs()
    // reads them too, and a wrong count there is a panic rather than a refusal.
    rx_cfg_apll_ = rx_apll_ref_ = (rx_ != nullptr && bus_->master);
    // And this is where the PLL's frequency comes from: the master, at twice its MCLK. The
    // slave port reads it to work out whether a format it is asked for is clockable at all.
    mclk_mult_ = bus_->mclk_multiple ? bus_->mclk_multiple : 256;
    if (bus_->master) s_apll_hz = audio_core::i2s_master_apll_hz(bus_->sample_rate_hz, mclk_mult_);

    // Registration is legal only before enabling DMA. ISR callbacks only count;
    // all formatting/reporting happens in the control task.
    i2s_event_callbacks_t callbacks = {};
    callbacks.on_send_q_ovf = count_queue_overflow;
    if (tx_ && i2s_channel_register_event_callback(tx_, &callbacks, &tx_ovf_) != ESP_OK)
        return false;
    callbacks = {};
    rx_events_.port = bus_->port;
    callbacks.on_recv = count_rx_block;
    callbacks.on_recv_q_ovf = count_rx_overflow;
    if (rx_ && i2s_channel_register_event_callback(rx_, &callbacks, &rx_events_) != ESP_OK)
        return false;
    initialized_ = true;
    rate_ = bus_->sample_rate_hz;
    ESP_LOGI(TAG, "[%s] port %d %s %s%s, %lu Hz, %u-bit slots, MCLK x%u on GPIO %d, BCLK %d WS %d DOUT %d DIN %d",
             role_.c_str(), (int)bus_->port, bus_->master ? "master" : "slave",
             tx_ ? "TX" : "", rx_ ? (tx_ ? "+RX" : "RX") : "",
             (unsigned long)bus_->sample_rate_hz, (unsigned)bus_->slot_bits,
             (unsigned)bus_->mclk_multiple, (int)bus_->mclk, (int)bus_->bclk, (int)bus_->ws,
             (int)bus_->dout, (int)bus_->din);
    return true;
}

bool I2sStdDriver::start() {
    if (!initialized_) return false;
    if (running_) return true;
    const bool seeded = tx_ring_seeded_;
    tx_ring_seeded_ = false;
    esp_err_t err = ESP_OK;
    if (tx_ && !seeded) {
        // Disable resets the DMA cursor; auto-clear leaves played descriptors
        // as exact zeros. Seed EVERY descriptor before enabling TX so a restart
        // never sends an empty ring to the DAC's zero-data detector.
        //
        // A descriptor per call: IDF syncs the whole descriptor from the cache on every call,
        // and with the write-back in pieces each sync may wait behind the display's while the
        // clock is stopped (see set_rate). 64-frame calls made 64 of them, whole ones make 8.
        static int32_t* s_quiet_desc = nullptr;   // DMA_FRAMES of keepalive, made once, PSRAM
        if (!s_quiet_desc) {
            s_quiet_desc = static_cast<int32_t*>(
                heap_caps_malloc(DMA_FRAMES * 2 * sizeof(int32_t), MALLOC_CAP_SPIRAM));
            if (s_quiet_desc) {
                for (size_t i = 0; i < DMA_FRAMES * 2; i++) s_quiet_desc[i] = 1 << 8;
            }
        }
        int32_t quiet[64 * 2];
        for (auto& sample : quiet) sample = 1 << 8;
        const int32_t* src       = s_quiet_desc ? s_quiet_desc : quiet;
        const size_t   src_bytes = s_quiet_desc ? DMA_FRAMES * 2 * sizeof(int32_t)
                                                : sizeof(quiet);
        size_t remaining = TX_DMA_DESCS * DMA_FRAMES * 2 * sizeof(int32_t);
        while (remaining) {
            const size_t size = remaining < src_bytes ? remaining : src_bytes;
            size_t loaded = 0;
            err = i2s_channel_preload_data(tx_, src, size, &loaded);
            if (err != ESP_OK || loaded != size) {
                ESP_LOGE(TAG, "[%s] tx preload: %s (%u/%u)", role_.c_str(),
                         esp_err_to_name(err), (unsigned)loaded, (unsigned)size);
                return false;
            }
            remaining -= loaded;
        }
    }
    tx_pos_frames_ = 0;   // seeded or not, IDF's writer starts the next write on a fresh descriptor
    // A duplex master's receive side runs only while its line input is wanted (see
    // set_line_input); an RX-only port always receives.
    const bool want_rx = rx_ && (!tx_ || line_input_ || link_.valid());
    if (want_rx && (err = i2s_channel_enable(rx_)) != ESP_OK) {
        ESP_LOGE(TAG, "[%s] rx enable: %s", role_.c_str(), esp_err_to_name(err));
        return false;
    }
    rx_enabled_ = want_rx;
    if (tx_ && (err = i2s_channel_enable(tx_)) != ESP_OK) {
        ESP_LOGE(TAG, "[%s] tx enable: %s", role_.c_str(), esp_err_to_name(err));
        if (rx_enabled_) i2s_channel_disable(rx_);
        rx_enabled_ = false;
        return false;
    }
    running_ = true;
    return true;
}

bool I2sStdDriver::set_rate(uint32_t hz, uint16_t mclk_multiple) {
    if (!initialized_ || !bus_->master || !hz) return false;
    if (hz < MIN_RATE_HZ || hz > MAX_RATE_HZ) return false;
    const uint16_t mult = mclk_multiple ? mclk_multiple
                        : (bus_->mclk_multiple ? bus_->mclk_multiple : 256);
    if (hz == rate_ && mult == mclk_mult_) return true;

    // The MCLK multiple is not decoration here: the audio PLL comes out at twice this MCLK,
    // and that frequency is what a SLAVE port sharing the PLL has to live within. Raising it
    // is how 32-bit slots become clockable; it also lowers this port's own ceiling, so a rate
    // the PLL cannot reach at this multiple is refused here rather than half-applied.
    const uint32_t pll = audio_core::i2s_master_apll_hz(hz, mult);
    if (!pll) {
        ESP_LOGW(TAG, "[%s] %lu Hz at %ux MCLK needs a PLL above %lu Hz, which this chip does "
                      "not make", name(), (unsigned long)hz, (unsigned)mult,
                 (unsigned long)audio_core::APLL_MAX_HZ);
        return false;
    }

    // Not while the input port is on the PLL. With a slave attached the park below leaves TWO
    // holders instead of one, the frequency stays where it is, and IDF reports success anyway
    // as long as the old frequency still clears its check — so the master would come back
    // believing it had moved. Whoever wants both must park the slave first; saying no here is
    // how that stays a rule rather than a hope.
    if (s_apll_slave) {
        ESP_LOGW(TAG, "[%s] cannot retune to %lu Hz while the input port holds the PLL",
                 name(), (unsigned long)hz);
        return false;
    }

    // A clock cannot be changed while it is running, so this stops, retunes and starts
    // again. The caller does it with the output gate closed — see the audio task — because
    // the DMA is drained in the middle and whatever is in it is lost.
    const bool was_running = running_;
    if (was_running && tx_) {
        // The software envelope being closed does not mean its fade has left
        // the 8-descriptor TX queue. Push a full ring plus one descriptor of
        // DAC keepalive before stopping clocks, so no audible tail is cut off —
        // and first the rest of the descriptor being written, so the drain ends on
        // a descriptor's edge and the ring it leaves holds no auto-cleared frames.
        int32_t quiet[64 * 2];
        for (auto& sample : quiet) sample = 1 << 8;
        size_t remaining = (DMA_FRAMES - tx_pos_frames_) % DMA_FRAMES +
                           (TX_DMA_DESCS + 1) * DMA_FRAMES;
        while (remaining) {
            const size_t frames = remaining < 64 ? remaining : 64;
            size_t bytes = 0;
            const esp_err_t err = i2s_channel_write(tx_, quiet, frames * 2 * sizeof(int32_t),
                                                   &bytes, 50);
            const size_t sent = bytes / (2 * sizeof(int32_t));
            tx_pos_frames_ = (uint32_t)((tx_pos_frames_ + sent) % DMA_FRAMES);
            if (err != ESP_OK || sent != frames) {
                ESP_LOGW(TAG, "[%s] TX fade drain incomplete; keeping current clock", name());
                return false;
            }
            remaining -= sent;
        }
        // Every descriptor holds keepalive now: the last one written was the one the DMA had
        // just released, and the DMA is on the next, which the drain also wrote. So the
        // start() below does not seed the ring again. Seeding is 64 cache syncs of a whole
        // descriptor, made while the clock is stopped. With the cache write-back in pieces
        // (sdkconfig.board) every sync from a task takes one global mutex, the display's
        // frame-buffer syncs hold it, and waiting behind them kept the clock stopped long
        // enough for the DAC to click on every rate change (clicks with that option, none
        // without it). What is left with this is the faint tick of the DAC relocking, which a
        // build without the option has too.
        tx_ring_seeded_ = true;
    }
    const int64_t stopped_at = esp_timer_get_time();
    if (was_running) stop();
    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(hz);
    clk.clk_src = I2S_CLK_SRC_APLL;
    // The multiple goes through as a number. IDF only multiplies by it (i2s_std.c,
    // i2s_std_calculate_clock), and the slave plan asks for 640x and 1280x, which the enum
    // does not name; mapping those to 256 would put the PLL where the slave cannot live.
    clk.mclk_multiple = static_cast<i2s_mclk_multiple_t>(mult);
    // Let go of the PLL before asking it to move.
    //
    // Both channels of this port hold the audio PLL, and with two holders its frequency is
    // frozen — clk_ctrl_os.c keeps the current value and hands it back whenever the
    // reference count is two or more. That is why every rate above 48 kHz was refused:
    // the PLL stayed at the 24.576 MHz it was set to at boot, the driver compared it
    // against the MCLK the new rate needed, and gave ESP_ERR_INVALID_ARG. The chip was
    // never the limit.
    //
    // So the receive channel is parked off the PLL first, leaving one holder, which is
    // allowed to retune it; then it is brought back onto the frequency now set.
    //
    // It is parked at 48 kHz and 128x MCLK rather than at the rate being asked for. The
    // only other source here is the 40 MHz crystal, and the driver wants the source to be
    // more than twice MCLK — at 96 kHz and 256x that is 49 MHz, which 40 MHz is not, so
    // parking failed with "sample rate is too large". A failed park is not harmless:
    // i2s_channel_reconfig_std_clock drops the PLL reference BEFORE the call can fail and
    // records the new source only after it succeeds, so a failure leaves the count one
    // below what the channel believes. The second one took it to zero and the assert in
    // periph_rtc_apll_freq_set fired — a reboot, mid-track, on the 192 kHz file.
    //
    // 48 kHz at 128x is 6.144 MHz, comfortably under the crystal whatever the slot width,
    // and the two flags follow the reference the way the driver actually moves it rather
    // than the way it reports it, so a failure degrades instead of detonating.
    constexpr uint32_t PARK_RATE_HZ = 48000;
    if (rx_ && rx_cfg_apll_ && rx_apll_ref_) {
        i2s_std_clk_config_t park = I2S_STD_CLK_DEFAULT_CONFIG(PARK_RATE_HZ);
        park.clk_src = I2S_CLK_SRC_DEFAULT;
        park.mclk_multiple = I2S_MCLK_MULTIPLE_128;
        rx_apll_ref_ = false;                  // released by the call, pass or fail
        if (i2s_channel_reconfig_std_clock(rx_, &park) == ESP_OK) {
            rx_cfg_apll_ = false;
        } else {
            ESP_LOGW(TAG, "[%s] rx would not park off the PLL; leaving it where it is",
                     name());
        }
    }
    // From here on the channel's own nominal MCLK is about to be re-derived, so any trim is
    // no longer a trim of anything. Cleared before the call rather than after it, because a
    // call that fails half way has moved it just as much as one that succeeds.
    trim_ppm_    = 0;
    trim_warned_ = false;
    esp_err_t err = ESP_OK;
    if (tx_) err = i2s_channel_reconfig_std_clock(tx_, &clk);
    // Back onto the PLL at the frequency the transmit side has just set. The reference
    // count goes to two again, which is fine: the frequency asked for is the one already
    // there, so there is nothing to freeze.
    if (err == ESP_OK && rx_ && !rx_cfg_apll_) {
        rx_apll_ref_ = true;                   // acquired by the call, pass or fail
        err = i2s_channel_reconfig_std_clock(rx_, &clk);
        if (err == ESP_OK) rx_cfg_apll_ = true;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[%s] cannot retune to %lu Hz: %s", name(), (unsigned long)hz,
                 esp_err_to_name(err));
        if (was_running) start();
        return false;
    }
    rate_      = hz;
    mclk_mult_ = mult;
    // Reconfiguring the clock re-derives IDF's origin_mclk_hz, which is what a trim is
    // measured from, so whatever trim was on the old rate is gone. Say so rather than carry
    // a number that no longer describes anything.
    trim_ppm_    = 0;
    trim_warned_ = false;
    // The PLL moved with it, and the slave port needs to know where to.
    s_apll_hz  = pll;
    const bool ok = was_running ? start() : true;
    // How long the DAC went without a clock: the number a click at a rate change is about.
    ESP_LOGI(TAG, "[%s] output now %lu Hz, MCLK x%u, PLL at %lu Hz; clock stopped %lu us",
             name(), (unsigned long)hz, (unsigned)mult, (unsigned long)pll,
             was_running ? (unsigned long)(esp_timer_get_time() - stopped_at) : 0ul);
    return ok;
}

// ── Trimming the clock, and the reference count that makes it legal ───────────────────────
//
// i2s_channel_tune_rate does not simply write a divider. On a channel sourced from the audio
// PLL it does periph_rtc_apll_release() → set the PLL → periph_rtc_apll_acquire()
// (i2s_common.c:1485-1490), and that release is the whole mechanism: clk_ctrl_os freezes the
// frequency while two or more peripherals hold it, so dropping to one holder is the only way
// IDF has of moving it at all.
//
// So the reference count at the moment of the call must be EXACTLY two, and neither
// neighbouring value is survivable by accident:
//
//   at 1 → the release takes it to ZERO. clk_ctrl_os powers the PLL down and forgets its
//          frequency, and periph_rtc_apll_freq_set then fires assert(s_apll_ref_cnt > 0).
//          That is a panic in the audio task, mid-track. It has happened here once already,
//          from the other direction: a failed park left the count one below what the driver
//          believed, and the next retune hit that assert on a 192 kHz file.
//   at 3 → the release leaves two, the frequency stays frozen, and i2s_set_get_apll_freq
//          turns that refusal into an ESP_LOGW and returns the CURRENT frequency as though
//          it had been granted (i2s_common.c:587-590). The call reports ESP_OK and nothing
//          moved. A servo steering against a clock that is not moving winds to its limit and
//          reports success all the way.
//
// Three is not a corner case here: it is what the panel is in whenever Bluetooth is playing
// a format the crystal cannot clock, which is every 32-bit stream the satellite sends —
// master TX, master RX and the satellite input, all three on the PLL. That is exactly when
// the trim is wanted.
//
// This file is the only place that can know the number, because both I2S ports on this panel
// are instances of it. So it counts, and it normalises the count to two around the call by
// taking or letting go of a reference of its own. Both directions are safe: acquiring from
// one and releasing from three each stay clear of zero, and the extra holder is given back
// on every path out, including the failing ones.
//
// Moving the PLL under the OTHER holders is harmless, and that is the part worth stating.
// The master's own RX shares TX's bit clock in full duplex. The satellite input is a SLAVE:
// it takes BCLK and WS off the wire and uses its internal clock only to oversample them,
// with half a percent of margin over what IDF demands (i2s_clock.h, and the test that pins
// it). A hundred parts per million is a fiftieth of that margin.

int I2sStdDriver::apll_refs() const {
    if (!initialized_ || !bus_ || !bus_->master) return 0;
    int n = tx_ ? 1 : 0;            // a master's TX is opened on the PLL and never leaves it
    if (rx_ && rx_apll_ref_) n++;   // the duplex ADC side, unless set_rate() has parked it
    if (s_apll_slave) n++;          // the satellite input, when a wide format put it there
    return n;
}

bool I2sStdDriver::can_trim() const {
    if (!initialized_ || !bus_->master || !tx_ || !rate_ || !mclk_mult_) return false;
    if (apll_refs() < 1) return false;
    // Both ends of the authority, because a servo that can only move one way is not a servo.
    const uint32_t mclk = rate_ * (uint32_t)mclk_mult_;
    return audio_core::i2s_trim_fits(mclk,  audio_core::I2S_TRIM_MAX_PPM)
        && audio_core::i2s_trim_fits(mclk, -audio_core::I2S_TRIM_MAX_PPM);
}

bool I2sStdDriver::trim_ppm(int32_t ppm, int32_t* applied_ppm) {
    trim_attempts_.fetch_add(1, std::memory_order_relaxed);
    if (applied_ppm) *applied_ppm = trim_ppm_;
    if (!initialized_ || !bus_->master || !tx_ || !rate_ || !mclk_mult_) return false;
    if (ppm >  audio_core::I2S_TRIM_MAX_PPM) ppm =  audio_core::I2S_TRIM_MAX_PPM;
    if (ppm < -audio_core::I2S_TRIM_MAX_PPM) ppm = -audio_core::I2S_TRIM_MAX_PPM;

    const uint32_t mclk = rate_ * (uint32_t)mclk_mult_;
    // Refused BEFORE anything is touched, because the failure it avoids cannot be handled
    // afterwards: IDF's tuning path returns ESP_ERR_INVALID_ARG while still holding the
    // channel's mutex when its own division comes out below two, and nothing ever gives that
    // mutex back — the next stop, start or retune of this channel blocks forever.
    if (!audio_core::i2s_trim_fits(mclk, ppm)) {
        if (!trim_warned_) {
            trim_warned_ = true;
            ESP_LOGW(TAG, "[%s] %ld ppm at %lu Hz x%u is not a clock this chip can be tuned "
                          "to; staying on nominal", name(), (long)ppm,
                     (unsigned long)rate_, (unsigned)mclk_mult_);
        }
        return false;
    }

    const int refs = apll_refs();
    if (refs < 1) {
        if (!trim_warned_) {
            trim_warned_ = true;
            ESP_LOGW(TAG, "[%s] will not tune the PLL without knowing who holds it", name());
        }
        return false;
    }

    const int32_t delta = audio_core::i2s_mclk_delta_hz(mclk, ppm);
    const int32_t limit = audio_core::i2s_mclk_delta_hz(mclk, audio_core::I2S_TRIM_MAX_PPM);
    i2s_tuning_config_t cfg = {};
    // 0 ppm goes back through RESET rather than through the arithmetic that produced the
    // offset: IDF kept the exact MCLK it derived at reconfiguration time, and that is a
    // better nominal than anything recomputed from a rate and a multiple.
    cfg.tune_mode      = ppm ? I2S_TUNING_MODE_SET : I2S_TUNING_MODE_RESET;
    cfg.tune_mclk_val  = (int32_t)mclk + delta;
    cfg.max_delta_mclk = limit;
    cfg.min_delta_mclk = -limit;

    for (int i = refs; i < 2; i++) periph_rtc_apll_acquire();
    for (int i = refs; i > 2; i--) periph_rtc_apll_release();
    i2s_tuning_info_t info = {};
    const esp_err_t err = i2s_channel_tune_rate(tx_, &cfg, &info);
    for (int i = refs; i < 2; i++) periph_rtc_apll_release();
    for (int i = refs; i > 2; i--) periph_rtc_apll_acquire();

    if (err != ESP_OK) {
        if (!trim_warned_) {
            trim_warned_ = true;
            ESP_LOGW(TAG, "[%s] clock trim refused: %s", name(), esp_err_to_name(err));
        }
        return false;
    }
    trim_ppm_ = ppm;
    trim_applied_.fetch_add(1, std::memory_order_relaxed);
    // What was actually achieved, not what was asked for. The synthesiser is quantised —
    // about 1.5 ppm a step at 24.576 MHz — so a small request lands on the nearest step it
    // has, and a servo that assumed otherwise would be integrating its own rounding.
    if (applied_ppm) {
        *applied_ppm = mclk ? (int32_t)(((int64_t)info.delta_mclk_hz * 1000000) / mclk) : 0;
    }
    return true;
}

void I2sStdDriver::set_line_input(bool on) {
    // Only a duplex master has a line input next to its output; an RX-only port IS its input.
    if (!initialized_ || !bus_ || !bus_->master || !tx_) return;
    if (on == line_input_) return;
    line_input_ = on;
    // MCLK: only the pin's routing changes; the channel's MCLK keeps running inside the
    // peripheral, so the bit clock and the rate are untouched. IDF routes GPIO only at init
    // and in reconfig_std_gpio, which nothing here calls, so the choice survives every retune.
    if (bus_->mclk != GPIO_NUM_NC) {
        if (on) {
            esp_rom_gpio_connect_out_signal(bus_->mclk, i2s_periph_signal[bus_->port].mck_out_sig,
                                            false, false);
        } else {
            gpio_set_level(bus_->mclk, 0);
            esp_rom_gpio_connect_out_signal(bus_->mclk, SIG_GPIO_OUT_IDX, false, false);
        }
    }
    // The receive DMA: while the output runs, bring RX in or out now; otherwise start()
    // honours line_input_ when it next enables the port. A clocked link keeps it running.
    const bool want_rx = on || link_.valid();
    if (rx_ && running_) {
        if (want_rx && !rx_enabled_) rx_enabled_ = i2s_channel_enable(rx_) == ESP_OK;
        else if (!want_rx && rx_enabled_) rx_enabled_ = i2s_channel_disable(rx_) != ESP_OK;
    }
    ESP_LOGI(TAG, "[%s] line input %s: MCLK %s, receive DMA %s", name(), on ? "on" : "off",
             on ? "driven" : "held low", rx_enabled_ ? "running" : "stopped");
}

modesp::audio::LinkPins I2sStdDriver::link_pins() const {
    modesp::audio::LinkPins p;
    // Only an input port describes a peer's link: RX-only, and the peer on its pins.
    if (!initialized_ || !bus_ || tx_ || !rx_) return p;
    p.bclk = bus_->bclk;
    p.ws   = bus_->ws;
    p.din  = bus_->din;
    return p;
}

bool I2sStdDriver::set_link_master(const modesp::audio::LinkPins* pins) {
    // A duplex master: its receive side runs on the very bit and frame clocks its transmit
    // side makes, which is the whole point — the peer's samples arrive on the clock that
    // plays them, so none can be lost between two clocks and nothing has to be resampled.
    if (!initialized_ || !bus_ || !bus_->master || !tx_ || !rx_) return false;
    const i2s_signal_conn_t& sig = i2s_periph_signal[bus_->port];
    if (pins && pins->valid()) {
        if (link_.valid() && link_.bclk == pins->bclk && link_.ws == pins->ws && link_.din == pins->din)
            return true;
        // Clocks out: the same signals that go to the DAC, fanned out to a second pad each.
        // IDF's own recipe for an output (i2s_gpio_check_and_set); the input is left enabled
        // so the pad can still be read, which the idle slave port and any probe rely on.
        gpio_func_sel((gpio_num_t)pins->bclk, PIN_FUNC_GPIO);
        gpio_input_enable((gpio_num_t)pins->bclk);
        esp_rom_gpio_connect_out_signal(pins->bclk, sig.m_tx_bck_sig, false, false);
        gpio_func_sel((gpio_num_t)pins->ws, PIN_FUNC_GPIO);
        gpio_input_enable((gpio_num_t)pins->ws);
        esp_rom_gpio_connect_out_signal(pins->ws, sig.m_tx_ws_sig, false, false);
        // Pad drive, measured with the satellite counting edges at its pins:
        // this cable is loaded hard enough that drive 0-1 moves neither line at all, and at
        // the default 2 the bit clock couples into WS for ~10 extra edges a frame (940 000 a
        // second where 96 000 belong). WS at 3 arrives clean with BCLK at 2.
        // BCLK stays at 2, the least that arrives, because it is also the least it couples
        // into WS: BCLK at 3 put 40-65 extra edges a second on WS at 96 kHz, each one a
        // misframed slot at the satellite and a click; back at 2 WS counted 96 005-96 007 (the
        // crystals' difference, nothing more). A BCLK that does not arrive at 2 is a bad
        // contact, not a weak pad: the satellite counted 0 BCLK edges until the jumper was
        // re-seated.
        gpio_set_drive_capability((gpio_num_t)pins->ws, GPIO_DRIVE_CAP_3);
        gpio_set_drive_capability((gpio_num_t)pins->bclk, GPIO_DRIVE_CAP_2);
        // Data in from the peer's line instead of the (unfitted) ADC's.
        gpio_func_sel((gpio_num_t)pins->din, PIN_FUNC_GPIO);
        gpio_input_enable((gpio_num_t)pins->din);
        esp_rom_gpio_connect_in_signal(pins->din, sig.data_in_sig, false);
        link_ = *pins;
        if (running_ && !rx_enabled_) rx_enabled_ = i2s_channel_enable(rx_) == ESP_OK;
        ESP_LOGI(TAG, "[%s] clocking the link: BCLK out on GPIO%d, WS out on GPIO%d, data in on "
                      "GPIO%d, receive DMA %s", name(), link_.bclk, link_.ws, link_.din,
                 rx_enabled_ ? "running" : "stopped");
        return true;
    }
    if (!link_.valid()) return true;
    // Hand the pins back: outputs off first, so for no instant do both ends drive them.
    esp_rom_gpio_connect_out_signal(link_.bclk, SIG_GPIO_OUT_IDX, false, false);
    gpio_set_direction((gpio_num_t)link_.bclk, GPIO_MODE_INPUT);
    esp_rom_gpio_connect_out_signal(link_.ws, SIG_GPIO_OUT_IDX, false, false);
    gpio_set_direction((gpio_num_t)link_.ws, GPIO_MODE_INPUT);
    // Receive side back on the board's own data pin.
    if (bus_->din != GPIO_NUM_NC) esp_rom_gpio_connect_in_signal(bus_->din, sig.data_in_sig, false);
    ESP_LOGI(TAG, "[%s] link clock released: GPIO%d and GPIO%d are inputs again", name(),
             link_.bclk, link_.ws);
    link_ = {};
    if (running_ && rx_enabled_ && !line_input_) rx_enabled_ = i2s_channel_disable(rx_) != ESP_OK;
    return true;
}

bool I2sStdDriver::start(uint32_t sample_rate_hz) {
    if (!initialized_) return false;
    // The legacy mono sink can ask for a rate. It goes through set_rate() like everything
    // else, because the clock must have exactly ONE way in.
    //
    // What used to be here built I2S_STD_CLK_DEFAULT_CONFIG and reconfigured both channels
    // with it, which is wrong three times over. I2S_CLK_SRC_DEFAULT is the 40 MHz crystal on
    // this silicon (I2S_CLK_SRC_PLL_160M is gated on chip revision 3.0 and this board is
    // v1.3), so it would drop the DAC off the audio PLL and onto a fractional divider — the
    // jitter the init comment above says a PCM5102A can fail to lock on. It discarded both
    // return values, so a refusal looked like success. And it moved the clock behind the
    // back of the PLL bookkeeping in set_rate(), leaving a second route into the same
    // reference-count fault. Nothing in this firmware calls it; that is luck, not design.
    if (bus_->master && sample_rate_hz && sample_rate_hz != rate_) {
        if (!set_rate(sample_rate_hz, 0)) {
            ESP_LOGW(TAG, "[%s] legacy start refused %lu Hz; staying at %lu",
                     name(), (unsigned long)sample_rate_hz, (unsigned long)rate_);
        }
    }
    return start();
}

/// Adopt the format a peer announced. Slave ports only: a master owns its clock and changes
/// it through set_rate(), where the APLL reference dance lives.
///
/// Both halves matter and only one of them is a clock. The width decides how many bit-clock
/// cycles the peripheral counts into a slot, so a channel opened for 16 bits against a 32-bit
/// stream reads one frame out of every two with the two channels packed into one word — the
/// fault that cost this project a day, and one that no rate change can undo.
bool I2sStdDriver::set_format(uint32_t hz, uint8_t bits) {
    if (!initialized_ || !hz) return false;
    if (bits != 16 && bits != 32) return false;
    if (bus_->master) return false;                 // a master retunes through set_rate()
    if (hz == rate_ && bits == slot_bits_) return true;

    // Clock source by NEED, and the decision is made BEFORE anything is touched.
    //
    // A slave has no clock to emit, but the peripheral still runs an internal MCLK to sample
    // the incoming one, and IDF fixes that at eight times BCLK with the source required to be
    // above 1.99 x MCLK. On this chip revision I2S_CLK_SRC_DEFAULT is the 40 MHz crystal
    // (PLL_F160M needs revision 3), which caps a slave at 78.5 kHz in 16-bit slots and cannot
    // do 32-bit slots at any audio rate.
    //
    // So: the crystal while it suffices, the PLL when the format needs it — but only if the
    // PLL is ALREADY at a frequency that serves it, because a slave cannot move it. The
    // master owns that frequency, it is twice the master's MCLK, and the module retunes the
    // output before asking for a wide format precisely so this test passes.
    //
    // Asking anyway and letting IDF refuse is the trap: the refusal arrives as ESP_ERR_
    // INVALID_ARG from the clock check, but by then the PLL reference has been acquired and
    // is never given back (i2s_std.c:386-398). Two of those and the master's own retune is
    // dead for the rest of the uptime — which is the state this panel was found in.
    const bool xtal_ok = audio_core::i2s_slave_clock_fits(I2S_XTAL_HZ, hz, bits);
    const bool apll_ok = audio_core::i2s_slave_clock_fits(s_apll_hz, hz, bits);
    if (!xtal_ok && !apll_ok) {
        ESP_LOGE(TAG, "[%s] %lu Hz / %u-bit slots needs a source above %lu Hz; the crystal is "
                      "%lu and the PLL is at %lu — refused without touching the port",
                 role_.c_str(), (unsigned long)hz, bits,
                 (unsigned long)(audio_core::i2s_slave_mclk_hz(hz, bits) / 100ull * 199ull),
                 (unsigned long)I2S_XTAL_HZ, (unsigned long)s_apll_hz);
        return false;
    }

    // The contract gives us at least 50 ms of silence after the announcement and before the
    // first sample, which is what makes stopping the channel here safe. Anything still in the
    // DMA belongs to the previous connection and is not worth keeping.
    const bool was_running = running_;
    if (was_running) stop();

    i2s_std_slot_config_t slot = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        bits == 32 ? I2S_DATA_BIT_WIDTH_32BIT : I2S_DATA_BIT_WIDTH_16BIT,
        I2S_SLOT_MODE_STEREO);

    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(hz);
    clk.clk_src = xtal_ok ? I2S_CLK_SRC_DEFAULT : I2S_CLK_SRC_APLL;

    const uint32_t prev_hz   = rate_;
    const uint8_t  prev_bits = slot_bits_;
    const uint32_t prev_div  = slave_bclk_div_;

    // Which of IDF's two calls goes FIRST, and it is not a matter of taste.
    //
    // There is no call that changes width and clock together, and each of the two recomputes
    // the whole clock from the other one's STORED value — reconfig_std_slot re-derives the
    // clock from the source the channel is still on, reconfig_std_clock re-derives it from the
    // width the channel is still at. So the port passes through one of two intermediate states:
    //
    //   clock first -> briefly at (old width, new source)
    //   slot first  -> briefly at (new width, old source)
    //
    // Widening onto the PLL only the first is legal; narrowing back to the crystal only the
    // second is. Getting it wrong is not subtle and not silent: the slot call re-derived the
    // clock from the crystal the channel was still on and answered "sample rate is too large",
    // which is what refused the first real 32-bit stream after the PLL itself was already
    // right. The same predicate that chose the source answers this too, so nothing new has to
    // be remembered — including which source the port is on now, which is whatever that
    // predicate chose for the format it is running.
    //
    // The divider (bclk_div, how many looks per bit the port takes at the wire) is part of
    // the stored clock too, so it is chosen the same way: the most the source allows for the
    // format being entered, but on the clock-first path no more than the intermediate width
    // also allows, and on the slot-first path the stored one must still fit the new width.
    const uint32_t new_sclk  = xtal_ok ? I2S_XTAL_HZ : s_apll_hz;
    const bool     prev_xtal = audio_core::i2s_slave_clock_fits(I2S_XTAL_HZ, prev_hz, prev_bits);
    const uint32_t prev_sclk = prev_xtal ? I2S_XTAL_HZ : s_apll_hz;
    const uint32_t final_div = audio_core::i2s_slave_bclk_div(new_sclk, hz, bits);
    const uint32_t step_div  = audio_core::i2s_slave_bclk_div(new_sclk, hz, prev_bits);
    const uint32_t first_div = step_div < final_div ? step_div : final_div;
    const bool clock_first = first_div >= 8;
    const bool slot_first  = audio_core::i2s_slave_bclk_div(prev_sclk, prev_hz, bits) >= prev_div;
    clk.bclk_div = clock_first ? first_div : final_div;
    if (!clock_first && !slot_first) {
        ESP_LOGE(TAG, "[%s] no way from %lu Hz / %u-bit to %lu Hz / %u-bit that IDF will take "
                      "one step at a time — refused without touching the port", role_.c_str(),
                 (unsigned long)prev_hz, prev_bits, (unsigned long)hz, bits);
        if (was_running) start();
        return false;
    }

    esp_err_t err = ESP_OK;
    if (rx_) {
        if (clock_first) {
            if ((err = i2s_channel_reconfig_std_clock(rx_, &clk)) == ESP_OK)
                err = i2s_channel_reconfig_std_slot(rx_, &slot);
        } else {
            if ((err = i2s_channel_reconfig_std_slot(rx_, &slot)) == ESP_OK)
                err = i2s_channel_reconfig_std_clock(rx_, &clk);
        }
    }
    if (err != ESP_OK) {
        // A failed reconfiguration does NOT leave the channel as it was. Changing the slot
        // width changes the DMA buffer size, and IDF frees the old descriptors BEFORE it
        // allocates the new ones (i2s_std.c, i2s_std_set_slot) — so a failure here leaves the
        // channel with none at all. The previous version of this branch restarted it anyway,
        // on the belief that nothing had changed, and enabled a GDMA channel whose first
        // descriptor was null. That is in the coredump as i2s_rx_channel_start, and it took
        // the panel down the first time the satellite ever announced 32-bit slots.
        ESP_LOGE(TAG, "[%s] cannot take %lu Hz / %u-bit slots: %s (DMA heap %u B, largest block %u)",
                 role_.c_str(), (unsigned long)hz, bits, esp_err_to_name(err),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));

        // Put the descriptors back by asking for the format it already had, which cannot need
        // more memory than it was using a moment ago.
        i2s_std_slot_config_t back_slot = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            prev_bits == 32 ? I2S_DATA_BIT_WIDTH_32BIT : I2S_DATA_BIT_WIDTH_16BIT,
            I2S_SLOT_MODE_STEREO);
        i2s_std_clk_config_t back_clk = I2S_STD_CLK_DEFAULT_CONFIG(prev_hz);
        const bool back_xtal = audio_core::i2s_slave_clock_fits(I2S_XTAL_HZ, prev_hz, prev_bits);
        back_clk.clk_src = back_xtal ? I2S_CLK_SRC_DEFAULT : I2S_CLK_SRC_APLL;
        back_clk.bclk_div = prev_div;

        // And back in the OPPOSITE order to the attempt, which is the order that is legal.
        // Whichever call failed, the half that did go through is the one this order undoes
        // first, and it undoes it from a state the predicates above have already shown to be
        // reachable. Trying both orders instead would be worse than useless: a clock call that
        // fails has already released the PLL reference, and asking again releases it twice.
        const bool restored = rx_ && (clock_first
            ? (i2s_channel_reconfig_std_slot(rx_, &back_slot) == ESP_OK
               && i2s_channel_reconfig_std_clock(rx_, &back_clk) == ESP_OK)
            : (i2s_channel_reconfig_std_clock(rx_, &back_clk) == ESP_OK
               && i2s_channel_reconfig_std_slot(rx_, &back_slot) == ESP_OK));

        if (restored) s_apll_slave = !back_xtal;
        if (restored) {
            if (was_running) start();
        } else {
            // Nothing safe left to enable. Stay stopped: read() returns 0, the module reports
            // underruns, and the panel keeps playing everything that is not Bluetooth.
            ESP_LOGE(TAG, "[%s] and could not be put back — the port stays down", role_.c_str());
        }
        return false;
    }

    slot_bits_   = bits;
    rate_        = hz;
    slave_bclk_div_ = clk.bclk_div;
    // On the PLL this port pins its frequency, and the master may not retune until it lets go.
    s_apll_slave = !xtal_ok;
    ESP_LOGI(TAG, "[%s] now reading %lu Hz, %u-bit slots, clocked from the %s, %lu looks per bit",
             role_.c_str(), (unsigned long)hz, bits,
             xtal_ok ? "crystal" : "PLL (which the output now holds still)",
             (unsigned long)slave_bclk_div_);
    return was_running ? start() : true;
}

size_t I2sStdDriver::write(const int32_t* stereo, size_t frames, uint32_t timeout_ms) {
    if (!running_ || !tx_) return 0;
    size_t bytes = 0;
    i2s_channel_write(tx_, stereo, frames * 2 * sizeof(int32_t), &bytes, timeout_ms);
    if (bytes < frames * 2 * sizeof(int32_t)) tx_short_.fetch_add(1, std::memory_order_relaxed);
    const size_t sent = bytes / (2 * sizeof(int32_t));
    tx_pos_frames_ = (uint32_t)((tx_pos_frames_ + sent) % DMA_FRAMES);
    return sent;
}

size_t I2sStdDriver::read(int32_t* stereo, size_t frames, uint32_t timeout_ms) {
    if (!running_ || !rx_) return 0;

    const size_t sample_bytes = (slot_bits_ == 16) ? sizeof(int16_t) : sizeof(int32_t);

    // One call, one timeout, straight into the caller's buffer. The 16-bit case fits because
    // the caller always provides room for int32 samples, which is twice what the channel
    // hands back.
    //
    // The return code is ignored on purpose, exactly as the write path ignores it: IDF fills
    // bytes_read from the DMA buffers and THEN reports ESP_ERR_TIMEOUT when the next
    // descriptor does not arrive, so a timeout routinely carries real frames. Testing the
    // code first and discarding them put a hole in the stream every time the peer paused
    // mid-block — a click, from the very function written to remove clicks.
    size_t bytes = 0;
    i2s_channel_read(rx_, stereo, frames * 2 * sample_bytes, &bytes, timeout_ms);
    const size_t got = bytes / (2 * sample_bytes);
    if (got < frames) rx_short_.fetch_add(1, std::memory_order_relaxed);

    if (sample_bytes == sizeof(int16_t)) {
        // A 16-bit channel hands back two bytes per sample and everything above this driver
        // is int32 end to end. Widen in place, walking DOWN: the word written at index i
        // covers bytes 4i..4i+3, which can only overlap source half-words at indices at or
        // above i, and those have already been converted.
        //
        // memcpy rather than a pointer cast because reading an int32 buffer through an int16
        // pointer is not something the standard permits, and the shift is built in unsigned
        // because shifting a negative signed value left is undefined before C++20 — half of
        // all audio samples are negative, so a sanitiser build would abort on the first block.
        // Both compile to the same load and shift.
        for (size_t i = got * 2; i-- > 0;) {
            int16_t s16;
            memcpy(&s16, reinterpret_cast<const char*>(stereo) + i * sizeof(int16_t), sizeof s16);
            stereo[i] = (int32_t)((uint32_t)(uint16_t)s16 << 16);
        }
    }
    return got;
}

size_t I2sStdDriver::write(const int16_t* pcm, size_t frames, uint32_t timeout_ms) {
    // Legacy path: 16-bit mono → 32-bit stereo, in chunks, no heap.
    if (!running_ || !tx_) return 0;
    int32_t chunk[64 * 2];
    size_t done = 0;
    while (done < frames) {
        size_t n = frames - done;
        if (n > 64) n = 64;
        for (size_t i = 0; i < n; i++) {
            int32_t s = (int32_t)pcm[done + i] << 16;
            chunk[2 * i] = s;
            chunk[2 * i + 1] = s;
        }
        size_t w = write(chunk, n, timeout_ms);
        done += w;
        if (w < n) break;
    }
    return done;
}

void I2sStdDriver::stop() {
    if (!running_) return;
    if (tx_) i2s_channel_disable(tx_);
    if (rx_ && rx_enabled_) i2s_channel_disable(rx_);
    rx_enabled_ = false;
    running_ = false;
}

// ═══════════════════════════════════════════════════════════════
// Driver factory + registration (optional via CONFIG_MODESP_DRIVER_I2S_STD)
// ═══════════════════════════════════════════════════════════════

#include "modesp/hal/driver_registry.h"
#include "modesp/hal/hal.h"
#include "etl/string_view.h"

namespace {
I2sStdDriver s_pool[modesp::MAX_I2S_BUSES];
size_t       s_n = 0;

modesp::audio::IAudioSink* i2s_std_factory(const modesp::Binding& b, modesp::HAL& hal) {
    if (s_n >= modesp::MAX_I2S_BUSES) {
        ESP_LOGE(TAG, "I2S pool exhausted");
        return nullptr;
    }
    auto* bus = hal.find_i2s_bus(
        etl::string_view(b.hardware_id.c_str(), b.hardware_id.size()));
    if (!bus) {
        ESP_LOGE(TAG, "I2S bus '%s' not found in board.json", b.hardware_id.c_str());
        return nullptr;
    }
    auto& drv = s_pool[s_n++];
    drv.configure(b.role.c_str(), bus);
    return &drv;
}
} // namespace

MODESP_REGISTER_AUDIO(i2s_std, &i2s_std_factory)
