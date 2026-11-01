#include "bt_module.h"
#include "bt_cover.h"
#include "bt_settings.h"
#include "device_name.h"
#include "bt_web.h"
#include "sat_link/cover_wire.h"
#include "sat_link/stream_control.h"

#include "modesp/hal/hal.h"
#include "modesp/types.h"
#include "modesp/utf8.h"
#include "modesp/hal/hal_types.h"
#include "modesp/hal/byte_link.h"
#include "modesp/hal/driver_registry.h"
#include "modesp/liveness.h"
#include "sat_link/sat_link.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "modesp/net/http_routes.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"

#include <atomic>
#include <new>
#include <cstdio>
#include <cstring>

static const char* TAG = "Bt";

namespace {

constexpr uint32_t PING_EVERY_MS   = 2000;
constexpr uint32_t HELLO_EVERY_MS  = 1000;   ///< the repeat while unlinked; also the reply rate limit, see say_hello()
constexpr uint32_t LINK_TIMEOUT_MS = 6000;   ///< three missed pings and it is not there
constexpr uint32_t REPORT_EVERY_MS = 2000;   ///< only while the link is down
/// How long a pairing request stays open here without its PAIR_RESULT. The contract sends
/// exactly one result and never repeats it, so a lost frame, or a satellite that restarted
/// inside LINK_TIMEOUT_MS, would leave the dialog up for good. Kinds 0 and 1: the 25 s reply
/// window, plus the 60 s the satellite allows a silent stack after consent, plus slack.
/// Kind 2 has no reply timeout at all; two minutes is for typing six digits on a phone.
constexpr uint32_t PAIR_ANSWER_LIMIT_MS  = 90000;
constexpr uint32_t PAIR_PASSKEY_LIMIT_MS = 120000;
/// How long a STREAM_READY stays worth repeating after the last STREAM_PREPARE that asked
/// for it. The contract has the satellite retrying PREPARE every 250 ms while it waits, so
/// one second is about four retries — long enough that a lost reply is recovered, short
/// enough that the repeating stops when the asking does.
constexpr uint32_t READY_REPEAT_MS = 1000;

constexpr size_t READ_CHUNK = 512;

/// `audio.source` when Bluetooth is the one being played, as modules/audio/manifest.json
/// lists the options.
constexpr int32_t AUDIO_SRC_BT = 3;

/// How often the "somebody else is playing, stop" PAUSE is repeated while the condition
/// holds. Normally exactly one goes out: the satellite leaves BT_STREAMING within about a
/// second of the phone acting on it, and the condition is then false. The interval exists
/// for the phone that ignores AVRCP PAUSE, so that it costs one command every three
/// seconds instead of one per main-loop tick for as long as it stays connected.
constexpr uint32_t AUTO_PAUSE_EVERY_MS = 3000;

// ── the wire task ──────────────────────────────────────────────────────────────────────────
//
// Until now the only thing draining the satellite's wire was on_update() on the priority-1
// `main` task, polling it a hundred times a second. At 460800 baud the 2 KB driver ring was
// 44 ms of wire, so a main loop late by five ticks lost bytes — and a lost byte costs the
// whole frame it was in, silently, because nothing was watching for the overflow either.
//
// So the bytes come off the wire here, on a task that does nothing else, and COMPLETE frames
// go to on_update() through a ring buffer. handle() still runs on `main`, which is what keeps
// every state_set on the main task exactly as before: this moves the reading, not the acting.

constexpr uint32_t UART_WAIT_MS     = 1000;   ///< also how fast the task notices `run = false`
constexpr int      SAT_TASK_PRIO    = 8;      ///< above the producers, far below audio
constexpr uint32_t SAT_TASK_STACK   = 4096;
/// Six frames of the largest kind: a 1016-byte cover part plus its two wire header bytes,
/// our four-byte length header and the ring's own eight, rounded up for alignment. At
/// 460800 baud one such frame takes 22 ms to arrive and on_update() drains every 10 ms, so
/// the depth is there for main-loop excursions, not for throughput.
constexpr size_t   FRAME_RING_BYTES = 6144;

/// One frame as it sits in the ring: what the wire carried, plus the length the ring item
/// would otherwise only imply.
struct RingHdr {
    uint8_t  type;
    uint8_t  seq;
    uint16_t len;
};

/// Everything the wire task owns. A file static rather than members because it needs
/// FreeRTOS handles and atomics in a header that other translation units include, and
/// because there is exactly one satellite link — the same shape the player and radio
/// modules use for their own tasks.
struct Wire {
    TaskHandle_t    task   = nullptr;
    RingbufHandle_t ring   = nullptr;
    volatile bool   run    = false;

    /// Written by the wire task, read by the main task for the report. Monotonic counters:
    /// a torn read costs one stale log line and nothing else. Bytes on and off the wire are
    /// NOT here: the link counts those, and two counters for one quantity can only disagree.
    std::atomic<uint32_t> zeros{0};       ///< delimiters; none at all means nothing is framing
    std::atomic<uint32_t> ring_drops{0};  ///< good frames the ring had no room for

    uint8_t               first[32]{};    ///< the first bytes ever seen, for one diagnostic
    std::atomic<uint8_t>  first_len{0};   ///< release-stored after `first` is filled

    /// Bumped once per turn of the wire loop. The queue wait times out, so this moves once
    /// a second even when the satellite says nothing at all — a counter that only moved on
    /// traffic would report an idle link as a wedged task.
    modesp::liveness::Counter beat{0};
};

Wire w;

// AVRCP absolute volume is 0..127 and ours is 0..100 percent. Rounded both ways, so a value
// that makes a round trip lands back where it started instead of walking a step per trip.
inline int32_t from_avrcp(uint8_t v) { return ((int32_t)v * 100 + 63) / 127; }
inline uint8_t to_avrcp(int32_t v)   { return (uint8_t)((v * 127 + 50) / 100); }


/// What we can do, as the contract words it.
///
/// Bit 0 says the I2S can be relocked on the fly from STREAM_FMT, and it is set again because
/// that is now true of both halves of the announcement. The rate moves the input channel AND
/// the output clock, so the converter is left with drift to absorb rather than a rate change;
/// the WIDTH reconfigures the slot, which is the half that used to be silently ignored and
/// the half no clock change can fix.
///
/// Per the contract this bit is what lets the satellite offer a phone more than 44.1 kHz.
///
/// Cover art is enabled only after its worker is ready. Bit 2 (the expander until 1.9) is
/// never set: 1.10 retired it.
///
/// Bit 3 is set too, and it is the one the satellite has been waiting on. set_format()
/// reconfigures the input channel to the announced slot width and picks a clock source that
/// can carry it, so a 32-bit stream is read as 32 bits rather than as half the frames with
/// the two channels glued together. With it the satellite can stop dithering 24 down to 16.
uint32_t our_caps = sat::HOST_CAP_RELOCK_I2S | sat::HOST_CAP_WIDE_SLOTS | FMT_HOST_CAP | sat::HOST_CAP_96KHZ |
                    /* 1.9: we clock the link, see HOST_CAP_I2S_MASTER */ sat::HOST_CAP_I2S_MASTER;

/// What goes into HELLO now: the fixed bits, and 1.8's PAIR_CONFIRM while a screen is there
/// to confirm on (bt_settings::set_confirmer). The satellite switches its pairing model on
/// the last HELLO it heard, so a change of this word is announced at once (on_update).
uint32_t hello_caps() {
    return our_caps | (bt_settings::confirmer() ? (uint32_t)sat::HOST_CAP_PAIR_CONFIRM : 0u);
}

/// A frame string into a fixed field, at most `cap - 1` bytes, cut on a character boundary.
/// The satellite promises at most 32 bytes cut that way already; this is for one that does
/// not, so a screen never gets half a letter.
void copy_utf8(char* dst, size_t cap, const sat::Str& s) {
    modesp::utf8_copy(dst, cap, s.p, s.len);
}

const char* state_name(uint8_t s) {
    switch (s) {
        case sat::BT_OFF:          return "off";
        case sat::BT_IDLE:         return "idle";
        case sat::BT_DISCOVERABLE: return "discoverable";
        case sat::BT_CONNECTING:   return "connecting";
        case sat::BT_CONNECTED:    return "connected";
        case sat::BT_STREAMING:    return "streaming";
        default:                   return "?";
    }
}

/// TRACK_POS.state in the words every other source on this player uses for `*.state`.
///
/// One screen reads all of them and compares against "playing", so Bluetooth answering in a
/// vocabulary of its own would mean a second comparison in the screen for no gain. Seeking
/// counts as playing: the phone is moving through the track, and a pause glyph appearing
/// while a finger is on the phone's scrubber would be wrong in both directions.
const char* play_state_name(uint8_t s) {
    switch (s) {
        case sat::PLAY_PLAYING:
        case sat::PLAY_FWD_SEEK:
        case sat::PLAY_REV_SEEK: return "playing";
        case sat::PLAY_PAUSED:   return "paused";
        default:                 return "idle";   // PLAY_STOPPED, and anything we do not know
    }
}

const char* codec_name(uint8_t c) {
    switch (c) {
        case sat::CODEC_SBC:      return "SBC";
        case sat::CODEC_AAC:      return "AAC";
        case sat::CODEC_APTX:     return "aptX";
        case sat::CODEC_APTX_HD:  return "aptX HD";
        case sat::CODEC_LHDC_V5:  return "LHDC V5";
        default:                  return "";
    }
}

uint32_t now_ms() { return (uint32_t)(esp_timer_get_time() / 1000); }

/// Frame strings are not NUL-terminated and point into the reader's buffer, so anything kept
/// is copied out before it is stored.
///
/// Each call says how wide the key it feeds is, and the text is cut here to that width —
/// on a character boundary, like every other string from the link — rather than in the store,
/// which would log a warning for every long name. It used to be a byte count, and a Cyrillic
/// title ended in half a letter: "…Слаане�" on the web (12.10.2026).
///
/// bt.title is marked `long` in the manifest (256 bytes), as airplay.title is: the 64-byte
/// inline value cut an audiobook's chapter name short on the panel and the web (12.10.2026).
/// What the satellite sends is the real limit now, 127 bytes. bt.artist/album and the rest
/// stay at the inline width.
/// One file-static buffer rather than three stack copies: only the module task runs this path
/// and state_set copies immediately. Each call overwrites the last, so store before calling
/// it again.
char s_str[MODESP_MAX_LONG_STRING_LENGTH + 1];

const char* cstr(const sat::Str& s, size_t width = MODESP_MAX_STRING_VALUE_LENGTH) {
    copy_utf8(s_str, width < sizeof s_str ? width + 1 : sizeof s_str, s);
    return s_str;
}

/// Prefers PSRAM: ~2 KB apiece, touched only from the module task, never by DMA or an ISR.
template <class T>
T* make() {
    void* p = heap_caps_malloc(sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_malloc(sizeof(T), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return p ? new (p) T() : nullptr;
}

template <class T>
void unmake(T* p) {
    if (p) { p->~T(); heap_caps_free(p); }
}

#include "bt_http.inc"

}  // namespace

BtModule::BtModule()
    // NORMAL, not LOW: ModulePriority orders on_init(), and on_init() is where the first
    // HELLO goes out. At LOW this module was initialised after the screen, the card and the
    // network, and the satellite resolves its A2DP endpoints once, about 500 ms after its
    // own start — so a greeting that arrives after that window leaves the endpoint at
    // 44.1 kHz for the whole session. NORMAL puts it alongside the audio core, which is
    // where it belongs anyway: it is not logging or monitoring.
    //
    // Nothing else moves. bind_all() runs before either init phase and visits every
    // registered module, so on_bind() still gets the HAL; the audio module is registered
    // first and the sort is stable between equal priorities, so it still initialises first;
    // and the satellite is spoken to over a UART that the HAL opened in step 5.
    : BaseModule("bt", modesp::ModulePriority::NORMAL)
{}

BtModule::~BtModule() {
    unmake(rx_);
    unmake(tx_);
}

void BtModule::on_bind(modesp::DriverManager&, const modesp::BindingTable& bindings,
                       modesp::HAL& hal) {
    hal_ = &hal;
    // The wire to the satellite is a bound driver, like every other driver-backed
    // peripheral: bindings.json says which hardware plays the `sat_link` role and which
    // driver mediates it, and neither the board id nor the word UART appears here.
    for (const auto& b : bindings.bindings) {
        if (!(b.module_name == "bt")) continue;                // only our bindings
        const char* type = b.driver_type.c_str();
        if (!modesp::DriverRegistry::has_link(type)) continue;  // sensors are DriverManager's
        if (!(b.role == "sat_link")) {
            ESP_LOGW(TAG, "unknown link role '%s' ignored", b.role.c_str());
            continue;
        }
        auto* l = modesp::DriverRegistry::create_link(type, b, hal);
        if (!l) {
            ESP_LOGW(TAG, "link '%s' [hw=%s] create failed", type, b.hardware_id.c_str());
            continue;
        }
        if (link_) { ESP_LOGW(TAG, "second 'sat_link' ignored"); continue; }
        link_ = l;
        ESP_LOGI(TAG, "sat_link '%s' [hw=%s]", type, b.hardware_id.c_str());
    }
    // The BT input port belongs to the audio module, which opens it; this module only reads
    // the slot width off it to tell the satellite what to send. So it asks for the port the
    // audio module was given for Bluetooth, by role, rather than naming it.
    bindings.hardware_for("audio", "port_bt", i2s_id_, sizeof i2s_id_);
}

bool BtModule::on_init() {
    state_set("bt.link", false);
    state_set("bt.state", "off");
    state_set("bt.rtt_ms", (int32_t)0);
    state_set("bt.rate_hz", (int32_t)0);
    // A5: the web's view of the settings and of a pairing. Registered before anything below
    // can give up: a board without the satellite still answers, with "linked": false.
    state_set("bt.cfg_rev", (int32_t)0);
    state_set("bt.pair_seq", (int32_t)0);
    state_set("bt.pair_active", false);
    state_set("bt.pair_kind", (int32_t)0);
    state_set("bt.pair_name", "");
    state_set("bt.pair_by", (int32_t)0);
    state_set("bt.pair_result", (int32_t)0xFF);
    if (!modesp::http_routes::add(register_bt_routes)) {
        ESP_LOGW(TAG, "no room in the route table — /api/bt will 404");
    }
    sat::stream_control().clear();
    modern_format_ = false;

    if (!hal_) {
        ESP_LOGW(TAG, "no HAL — the link stays down");
        return true;
    }
    if (!link_) {
        ESP_LOGW(TAG, "no 'sat_link' binding for this module — Bluetooth disabled");
        return true;
    }
    if (!link_->init()) {
        // Not an error: a board without the satellite wired is a board that plays from the
        // card and the network, and it should not fail to boot over a missing accessory.
        // The driver has already said which wire it is and what was wrong with it.
        ESP_LOGW(TAG, "the satellite's wire did not come up — Bluetooth disabled");
        link_ = nullptr;
        return true;
    }
    rx_ = make<sat::Reader>();
    tx_ = make<sat::Writer>();
    if (!rx_ || !tx_) {
        ESP_LOGE(TAG, "no room for the link buffers (%u B)",
                 (unsigned)(sizeof(sat::Reader) + sizeof(sat::Writer)));
        unmake(rx_); rx_ = nullptr;
        unmake(tx_); tx_ = nullptr;
        link_ = nullptr;
        return true;
    }

    if (i2s_id_[0])
        if (modesp::I2SBusResource* i2s = hal_->find_i2s_bus(i2s_id_))
            slot_bits_ = i2s->slot_bits;

    // ── the wire task ──
    // Internal RAM for both the ring and the stack: this task runs while the flash cache is
    // off during an NVS or LittleFS write, and a PSRAM stack dies there (docs/CONCEPT.md,
    // "Board constraints").
    if (!link_->can_wait())
        ESP_LOGW(TAG, "this wire cannot report an overflow — losses will not be counted");
    w.ring = xRingbufferCreateWithCaps(FRAME_RING_BYTES, RINGBUF_TYPE_NOSPLIT,
                                       MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (w.ring && link_->can_wait()) {
        w.run = true;
        if (xTaskCreatePinnedToCore(&BtModule::uart_task_entry, "sat_uart", SAT_TASK_STACK,
                                    this, SAT_TASK_PRIO, &w.task, 0) != pdPASS) {
            w.run = false;
            w.task = nullptr;
        } else {
            modesp::liveness::watch("sat_uart", &w.beat, 5000, this);
        }
    }
    if (!w.task) {
        // Not fatal. on_update() still polls the port the way it always did; the link is
        // then as fragile as it was before, which is better than no Bluetooth at all.
        ESP_LOGE(TAG, "no wire task (ring %s, wake-ups %s) — falling back to polling the "
                      "wire from the main loop",
                 w.ring ? "ok" : "failed", link_->can_wait() ? "ok" : "missing");
        if (w.ring) { vRingbufferDeleteWithCaps(w.ring); w.ring = nullptr; }
    }

    if (bt_cover::init()) our_caps |= sat::HOST_CAP_WANT_COVER | COVER_STREAM_CAP;
    ESP_LOGI(TAG, "link on %s, proto %u.%u, caps 0x%08lx",
             link_->describe(),
             (unsigned)sat::PROTO_MAJOR, (unsigned)sat::PROTO_MINOR,
             (unsigned long)our_caps);
    probe_rx();
    say_hello();
    return true;
}

void BtModule::on_update(uint32_t dt_ms) {
    if (!link_ || !rx_) return;

    since_rx_ms_     += dt_ms;
    since_ping_ms_   += dt_ms;
    since_hello_ms_  += dt_ms;
    since_report_ms_ += dt_ms;
    vol_gate_.tick(dt_ms);

    if (!w.task) read_uart_inline();   // no wire task on this board — poll as we used to
    drain_frames();                    // frames the wire task collected; handle() runs here

    bt_cover::tick(now_ms());
    // Protocol 1.7. A cover that lost one frame used to be lost for good — the bench log of
    // 21.09.2026 shows a 53 KiB image thrown away over 501 missing bytes. Ask for it again,
    // but only of a satellite that said it would answer: an older one never sets the bit,
    // and against it this line does nothing at all, which is the 1.6 behaviour exactly.
    if (linked_ && (peer_caps_ & sat::DEV_CAP_COVER_RETRY)) {
        uint32_t nonce = 0, generation = 0;
        if (bt_cover::take_retry(nonce, generation)) {
            send(sat::build_cover_retry(*tx_, nonce, generation));
            ESP_LOGI(TAG, "cover %lu/%lu came through broken — asking again",
                     (unsigned long)nonce, (unsigned long)generation);
        }
    }
    format_reply_ms_ += dt_ms;
    // The repeat is bounded by a budget that every STREAM_PREPARE refills. The contract has
    // the host repeating its result at 100 ms intervals "to recover a lost reply", and the
    // satellite retrying PREPARE every 250 ms "while waiting" — so the answer is worth
    // repeating exactly as long as the question is still being asked. Unbounded, this sent
    // a READY ten times a second for ever once any result existed, for a format that had
    // settled minutes earlier, on a link that also carries cover art.
    if (linked_ && modern_format_ && ready_budget_ms_ && format_reply_ms_ >= 100) {
        sat::StreamRequest request;
        uint8_t status;
        if (sat::stream_control().snapshot(request) && request.format.nonce &&
            sat::stream_control().result(request.revision, status)) {
            fmt_ready_t ready{request.format.nonce, request.format.generation, status};
            uint8_t payload[9]; fmt_encode_ready(payload, &ready);
            send(tx_->frame(FMT_READY, payload, sizeof payload));
            format_reply_ms_ = 0;
        }
    }
    if (ready_budget_ms_)
        ready_budget_ms_ = (dt_ms >= ready_budget_ms_) ? 0 : ready_budget_ms_ - dt_ms;
    if (linked_ && since_rx_ms_ >= LINK_TIMEOUT_MS) link_lost("silent");

    // A link that is UP reported nothing at all, so a partner complaining about unanswered
    // pings could only be met with a shrug. It reports on the same beat as a down one now.
    if (linked_ && since_report_ms_ >= REPORT_EVERY_MS * 5) {
        since_report_ms_ = 0;
        publish_wire_counters();
        const modesp::link::LinkStats st = link_->stats();
        ESP_LOGI(TAG, "link up: rx %lu bytes / %lu frames (crc %lu, cobs %lu, oversize %lu, "
                      "lost %lu, overrun %lu, ring full %lu), tx %lu bytes, %lu pongs sent, "
                      "rtt %ld ms, %lu ms since a frame",
                 (unsigned long)st.rx_bytes, (unsigned long)frames_,
                 (unsigned long)rx_->bad_crc(), (unsigned long)rx_->bad_cobs(),
                 (unsigned long)rx_->too_big(), (unsigned long)rx_->seq_gaps(),
                 (unsigned long)st.overruns, (unsigned long)w.ring_drops.load(),
                 (unsigned long)st.tx_bytes,
                 (unsigned long)pongs_, (long)read_int("bt.rtt_ms", -1),
                 (unsigned long)since_rx_ms_);
    }

    // A pairing whose result never came closes here, before the requests (a late answer to
    // it is then not sent) and before publish_settings() (every web tab hears of it now). The
    // satellite's repeat of an active request after a HELLO has the same seq, so it does not
    // restart the clock.
    {
        bt_settings::Pairing pr;
        bt_settings::pairing(pr);
        if (!pr.active) {
            pair_open_ms_ = 0;
        } else if (pr.seq != pair_seq_seen_) {
            pair_seq_seen_ = pr.seq;
            pair_open_ms_ = 0;
        } else if ((pair_open_ms_ += dt_ms) >= (pr.kind == sat::PAIR_PASSKEY ? PAIR_PASSKEY_LIMIT_MS
                                                                              : PAIR_ANSWER_LIMIT_MS)) {
            bt_settings::pair_abandon();
            ESP_LOGW(TAG, "pairing %lu: no result from the satellite, closed here",
                     (unsigned long)pr.seq);
        }
    }

    apply_transport_cmd();
    apply_settings_requests();
    publish_settings();

    // A screen came up (or went) after the last HELLO: the satellite picks its pairing model
    // from the last one it heard, so it is told now rather than at the next reconnect.
    if (linked_ && hello_caps() != hello_caps_sent_) {
        since_hello_ms_ = HELLO_EVERY_MS;
        say_hello();
    }

    if (linked_) {
        // Our knob drives the phone's slider, the same way the phone's drives ours. The
        // first pass after a link comes up, after a phone connects and after a level the
        // phone set was not taken pushes what the panel already has, so the phone shows the
        // volume the room is actually at.
        const int32_t vol = read_int("audio.volume", -1);
        if (vol >= 0 && vol != vol_echo_) {
            vol_echo_ = vol;
            send(sat::build_volume(*tx_, to_avrcp(vol)));
        }
        if (since_ping_ms_ >= PING_EVERY_MS) {
            since_ping_ms_ = 0;
            send(sat::build_ping(*tx_, now_ms()));
        }
        gate_on_source(dt_ms);
    } else {
        // Keep saying hello, once a second, until the satellite's HELLO arrives (protocol
        // section 3): it may be powered later than us, or reflashed, and a greeting of ours
        // that landed inside its one-second reply window got no answer of its own.
        if (since_hello_ms_ >= HELLO_EVERY_MS) say_hello();
        if (since_report_ms_ >= REPORT_EVERY_MS) report();
    }
}

void BtModule::on_stop() {
    bt_cover::clear();
    // The task is asked to stop and not waited for: it can be up to UART_WAIT_MS inside the
    // queue wait, and on_stop() runs on the main loop. The ring and the queue are left
    // allocated on purpose — freeing them here would race the task that is still using them,
    // and on_stop() means the system is going down, not that the memory is needed elsewhere.
    modesp::liveness::unwatch("sat_uart");
    w.run = false;
    if (link_) link_->flush_rx();
}

// ═══════════════════════════════════════════════════════════════
// The wire: bytes off the UART on their own task, frames to the main loop
// ═══════════════════════════════════════════════════════════════

void BtModule::uart_task_entry(void* arg) { static_cast<BtModule*>(arg)->uart_loop(); }

void BtModule::uart_loop() {
    uint8_t buf[READ_CHUNK];

    ESP_LOGI(TAG, "wire task on core %d, %u-byte frame ring",
             (int)xPortGetCoreID(), (unsigned)FRAME_RING_BYTES);

    while (w.run) {
        // A timeout rather than an endless wait, so a stop is noticed without a wake-up
        // message and a silent satellite still lets this loop turn over — which is also
        // what makes the counter below a heartbeat rather than a traffic meter.
        const modesp::link::Rx got = link_->wait_rx(UART_WAIT_MS);
        modesp::liveness::tick(w.beat);

        if (got == modesp::link::Rx::Lost) {
            // The wire dropped bytes, so whatever frame was in flight is incomplete no
            // matter what we do now. The driver has already thrown the remainder away; the
            // framer is reset here so the next delimiter puts us back in step — which is the
            // whole reason the contract uses COBS instead of a length header.
            if (rx_) rx_->reset();
            continue;
        }

        // Read whatever is there, whichever way the wait ended and even on a timeout: a
        // wake-up is a hint about one interrupt, not a statement about the buffer, and
        // reading until it is empty is cheaper than trusting the arithmetic.
        for (;;) {
            const int n = link_->read(buf, sizeof buf);
            if (n <= 0) break;
            // A stream that never shows a 0x00 never completes a frame and never trips a
            // counter either: the reader just fills and waits. Counting delimiters
            // separately is what tells "the partner is talking and we disagree" from
            // "this pin is floating".
            uint32_t zeros = 0;
            for (int i = 0; i < n; i++) if (!buf[i]) zeros++;
            if (zeros) w.zeros.fetch_add(zeros, std::memory_order_relaxed);
            if (!w.first_len.load(std::memory_order_relaxed)) {
                const uint8_t k = (uint8_t)(n < (int)sizeof w.first ? n : (int)sizeof w.first);
                memcpy(w.first, buf, k);
                w.first_len.store(k, std::memory_order_release);   // after the bytes
            }
            rx_->feed(buf, (size_t)n, &BtModule::on_frame, this);
            if ((size_t)n < sizeof buf) break;
        }
    }

    ESP_LOGI(TAG, "wire task stopped");
    w.task = nullptr;
    vTaskDelete(nullptr);
}

/// The path for a board where the ring or the task could not be made: exactly what
/// on_update() used to do, running on the main task.
void BtModule::read_uart_inline() {
    uint8_t buf[READ_CHUNK];
    for (;;) {
        const int n = link_->read(buf, sizeof buf);
        if (n <= 0) break;
        uint32_t zeros = 0;
        for (int i = 0; i < n; i++) if (!buf[i]) zeros++;
        if (zeros) w.zeros.fetch_add(zeros, std::memory_order_relaxed);
        if (!w.first_len.load(std::memory_order_relaxed)) {
            const uint8_t k = (uint8_t)(n < (int)sizeof w.first ? n : (int)sizeof w.first);
            memcpy(w.first, buf, k);
            w.first_len.store(k, std::memory_order_release);
        }
        rx_->feed(buf, (size_t)n, &BtModule::on_frame, this);
        if ((size_t)n < sizeof buf) break;
    }
}

void BtModule::drain_frames() {
    if (!w.ring) return;
    for (;;) {
        size_t size = 0;
        void*  item = xRingbufferReceive(w.ring, &size, 0);
        if (!item) break;
        if (size >= sizeof(RingHdr)) {
            RingHdr h{};
            memcpy(&h, item, sizeof h);
            sat::Frame f;
            f.type    = h.type;
            f.seq     = h.seq;
            f.payload = static_cast<const uint8_t*>(item) + sizeof(RingHdr);
            f.len     = h.len;
            // Everything downstream of here — including every state_set and the cover
            // assembly — runs on the main task, exactly as it did when this module read the
            // UART itself. The payload stays valid until the item is returned below, which
            // is why handle() is called before that and not after.
            handle(f);
        }
        vRingbufferReturnItem(w.ring, item);
    }
}

void BtModule::say_hello() {
    // HELLO is both the greeting and the reply to a greeting, which on its own is a loop.
    // Rate-limiting it is what ends the exchange: whoever would reply within a second of
    // having spoken stays quiet, and a satellite that reboots still gets an answer.
    if (since_hello_ms_ < HELLO_EVERY_MS) return;
    since_hello_ms_ = 0;
    // The satellite prints this in its own log, so it says which firmware it is talking to.
    char build[48];
    const esp_app_desc_t* d = esp_app_get_description();
    snprintf(build, sizeof build, "player %s", d ? d->version : "?");
    hello_caps_sent_ = hello_caps();
    send(sat::build_hello(*tx_, hello_caps_sent_, build));
}

/// The settings' revision and the pairing, for the web (A5). The code itself is not here: the
/// WebSocket is open to the whole network, so a tab fetches it from GET /api/bt, behind the
/// login (bt_web.h).
void BtModule::publish_settings() {
    const uint32_t rev = bt_settings::revision();
    if (rev != pub_cfg_rev_) {
        pub_cfg_rev_ = rev;
        state_set("bt.cfg_rev", (int32_t)rev);
    }
    bt_settings::Pairing p;
    bt_settings::pairing(p);
    const uint8_t by = (uint8_t)p.by;
    if (p.seq == pub_pair_seq_ && p.active == pub_pair_active_ && by == pub_pair_by_ &&
        p.result == pub_pair_result_) return;
    if (p.seq != pub_pair_seq_) {
        state_set("bt.pair_kind", (int32_t)p.kind);
        state_set("bt.pair_name", p.name);
    }
    pub_pair_seq_ = p.seq;
    pub_pair_active_ = p.active;
    pub_pair_by_ = by;
    pub_pair_result_ = p.result;
    state_set("bt.pair_by", (int32_t)by);
    state_set("bt.pair_result", (int32_t)p.result);
    state_set("bt.pair_active", p.active);
    state_set("bt.pair_seq", (int32_t)p.seq);   // last: a tab that sees it move has the rest
}

/// Protocol 1.8: what the screen asked for, onto the wire. Main loop, like every send.
///
/// Disconnect works with any satellite (CMD_RADIO is 1.0); everything else needs one that
/// said DEV_CAP_BT_SETTINGS, and is dropped with a line in the log otherwise — the screen
/// only offers it when the bit is there, so reaching here without it means the satellite
/// went away between the tap and this tick.
void BtModule::apply_settings_requests() {
    bt_settings::Pending p;
    if (!bt_settings::take(p)) return;
    if (!linked_) {
        ESP_LOGW(TAG, "settings request dropped: no link to the satellite");
        return;
    }
    const bool settings = (peer_caps_ & sat::DEV_CAP_BT_SETTINGS) != 0;
    bt_settings::Config v;       // the switches only: this is the main task's stack
    bt_settings::config(v);

    if (p.disconnect) {
        // The contract has a 1.8 host send CMD_RADIO only to disconnect, with radio and
        // visibility as BT_CONFIG last said. A 1.7 satellite has no BT_CONFIG and starts
        // discoverable, so that is what it is told to stay.
        const bool radio = (settings && v.known) ? v.radio : true;
        const bool disc  = (settings && v.known) ? v.discoverable : true;
        send(sat::build_radio(*tx_, radio, disc, true));
        ESP_LOGI(TAG, "phone disconnect requested");
    }
    if (!settings) {
        if (p.config || p.rename || p.forget || p.reply || p.refresh)
            ESP_LOGW(TAG, "settings request dropped: the satellite does not speak 1.8");
        return;
    }
    if (p.reply) {
        bt_settings::Pairing pr;
        bt_settings::pairing(pr);
        if (pr.active && pr.seq == p.reply_seq && pr.kind != sat::PAIR_PASSKEY) {
            send(sat::build_pair_reply(*tx_, pr.addr, p.accept));
            ESP_LOGI(TAG, "pairing %s %s", p.accept ? "accepted" : "refused",
                     pr.by == bt_settings::Source::WEB ? "in the web" : "on the panel");
        }
    }
    if ((p.config || p.rename) && !v.known) {
        // The whole state below would come from a view no BT_CONFIG has filled — after a
        // panel boot, all off — and the satellite keeps what it is sent in NVS: radio off,
        // hidden, SBC only. The refresh brings the real state, the screen repaints, and the
        // next try is built from it. Not re-queued: that would ask for a refresh every tick.
        ESP_LOGW(TAG, "settings change dropped: no BT_CONFIG from the satellite yet, asking for it");
        p.refresh = true;
    } else if (p.config || p.rename) {
        // BT_CONFIG_SET is the whole state: a rename carries the switches as they are, and a
        // switch carries an empty name, which the contract reads as "keep".
        const bool    radio = p.config ? p.radio : v.radio;
        const bool    disc  = p.config ? p.discoverable : v.discoverable;
        const bool    autoc = p.config ? p.auto_reconnect : v.auto_reconnect;
        const uint8_t codec = p.config ? p.codecs : v.codecs_enabled;
        const size_t  n = sat::build_bt_config_set(*tx_, radio, disc, autoc, codec,
                                                   p.rename ? p.name : "");
        if (!n) ESP_LOGW(TAG, "device name refused: longer than %u bytes", (unsigned)sat::BT_NAME_MAX);
        send(n);
        // «Ім'я» names the whole panel (device_name.h): AirPlay and DLNA take it at their next start
        if (n && p.rename) device_name::keep(p.name);
    }
    if (p.forget) {
        send(sat::build_bond_remove(*tx_, p.forget_all ? nullptr : p.forget_addr));
        ESP_LOGI(TAG, "forget %s", p.forget_all ? "every phone" : "one phone");
    }
    if (p.refresh) send(sat::build_bt_refresh(*tx_));
}

void BtModule::probe_rx() {
    // Only the wire can answer this: no meter at the other end of a cable can say what the
    // die sees, and between the two sit a removed 0-ohm link and an assumption about which
    // pad this is. The driver pulls the line both ways and reports what it refused to follow.
    last_pin_ = link_->probe_rx();
    const char* words = modesp::link::rx_pin_words(last_pin_);
    if (last_pin_ == modesp::link::RxPin::High) ESP_LOGI(TAG, "%s: RX %s", link_->describe(), words);
    else                                        ESP_LOGW(TAG, "%s: RX %s", link_->describe(), words);
}

void BtModule::send(size_t wire_len) {
    if (!link_ || !tx_ || !wire_len) return;
    link_->write(reinterpret_cast<const uint8_t*>(tx_->wire()), wire_len);
}

/// One AVRCP passthrough command per queued press.
///
/// A posted command rather than a direct call because the screen, the web API and this module
/// share no other channel, and the codes are player.cmd's, so the panel's transport row sends
/// the same numbers whichever source it happens to be driving.
///
/// A loop over the queue, not one read of a key: two presses inside one tick are two
/// commands. As a shared-state key the second overwrote the first before this ever ran —
/// press next twice quickly and the phone skipped one track.
void BtModule::apply_transport_cmd() {
    modesp::BaseModule::Command c;
    while (next_command(c)) apply_one_transport(c.value);
}

void BtModule::apply_one_transport(int32_t cmd) {
    if (!cmd) return;
    if (!linked_) {
        // Consumed and dropped, not kept for later. The panel cannot produce one of these
        // without a link — the buttons are dead until a phone is attached — but the web API
        // can, and a command that waited out a satellite reboot would start the music
        // minutes after whoever asked for it had given up and walked away.
        ESP_LOGW(TAG, "transport %ld dropped: no link to the satellite", (long)cmd);
        return;
    }

    uint8_t tr = 0xFF;
    switch (cmd) {
    case 1: tr = sat::TR_PLAY;  break;
    case 2: tr = sat::TR_PAUSE; break;
    case 3: tr = sat::TR_STOP;  break;
    case 4: tr = sat::TR_NEXT;  break;
    case 5: tr = sat::TR_PREV;  break;
    case 6: {
        // Resolved here and not on the screen: this module is the one that knows what the
        // phone last said it was doing, and the screen would have to read the same key
        // back to find out. Nothing known yet means play, which is what a first press on a
        // phone that has sent no TRACK_POS should do.
        char st[12]{};
        read_string("bt.play_state", st, sizeof st);
        tr = (strcmp(st, "playing") == 0) ? sat::TR_PAUSE : sat::TR_PLAY;
        break;
    }
    default: return;   // not a command this contract has
    }
    send(sat::build_transport(*tx_, tr));
    // Whoever pressed a button has settled the question of who stopped the music, so the
    // automatic resume in gate_on_source() has nothing left to undo.
    auto_paused_ = false;
    if (tr == sat::TR_PLAY) stall_.on_play();
    else if (tr == sat::TR_PAUSE || tr == sat::TR_STOP) stall_.on_user_pause();
}

/// The three numbers the 24 h soak is graded on, where a dashboard can reach them.
/// Diagnostics, so they are set quietly: a counter that does not move must not wake the
/// WebSocket delta a hundred times a second.
void BtModule::publish_wire_counters() {
    // The overrun count belongs to the wire, not to this module: it is the one thing only
    // the transport can see, and it is what tells a lost frame from a silent partner.
    state_set("bt.uart_ovf", (int32_t)(link_ ? link_->stats().overruns : 0), false);
    state_set("bt.rx_drops", (int32_t)w.ring_drops.load(std::memory_order_relaxed), false);
    state_set("bt.seq_gaps", (int32_t)(rx_ ? rx_->seq_gaps() : 0), false);
}

void BtModule::report() {
    since_report_ms_ = 0;
    publish_wire_counters();

    // Frames arriving but no HELLO among them is a specific fault with a specific fix, and
    // it should not have to be inferred from a frame count. The contract has HELLO answering
    // HELLO; without the reply we never learn the other side's protocol version or its caps,
    // and the compatibility check the exchange exists for never happens.
    // Gated on "no HELLO has EVER arrived", not on "not linked". frames_ is cumulative and
    // link_lost() does not reset it, so the old condition also fired after a link that came
    // up and went quiet, and after a version refusal where a HELLO plainly did arrive — both
    // times asserting the one thing that was not true. And it returned, suppressing the
    // counter line whose own comment calls it the only thing that tells a dead wire from a
    // silent partner: in the genuine cold-start case where HELLOs arrive but fail CRC,
    // bad_crc() would be climbing and this message hid it while naming the opposite cause.
    if (frames_ && !hello_seen_)
        ESP_LOGW(TAG, "%lu frames in and not one HELLO: the satellite hears ours but does not "
                      "answer it — the contract has HELLO replying to HELLO",
                 (unsigned long)frames_);
    // The only thing that tells a dead wire from a silent partner, so it says both sides.
    // pongs is here to separate "we are not answering" from "they are not asking": a partner
    // reporting unanswered pings cannot be met with a shrug, and the two faults are on
    // different sides.
    const modesp::link::LinkStats st = link_->stats();
    ESP_LOGW(TAG, "link down: rx %lu bytes (%lu delimiters) / %lu frames (crc %lu, cobs %lu, "
                  "oversize %lu, lost %lu, overrun %lu, ring full %lu), tx %lu bytes, "
                  "%lu pings answered",
             (unsigned long)st.rx_bytes, (unsigned long)w.zeros.load(),
             (unsigned long)frames_,
             (unsigned long)rx_->bad_crc(), (unsigned long)rx_->bad_cobs(),
             (unsigned long)rx_->too_big(), (unsigned long)rx_->seq_gaps(),
             (unsigned long)st.overruns, (unsigned long)w.ring_drops.load(),
             (unsigned long)st.tx_bytes, (unsigned long)pongs_);

    // Once, and only while nothing has framed: the first bytes off the wire, as hex and as
    // text. Whatever is talking on this pin names itself here, and guessing stops.
    const uint8_t first_len = w.first_len.load(std::memory_order_acquire);
    if (first_len && !frames_ && !dumped_) {
        dumped_ = true;
        char hex[3 * 32 + 1], txt[32 + 1];
        size_t h = 0;
        for (uint8_t i = 0; i < first_len; i++) {
            h += (size_t)snprintf(hex + h, sizeof hex - h, "%02X ", w.first[i]);
            txt[i] = (w.first[i] >= 32 && w.first[i] < 127) ? (char)w.first[i] : '.';
        }
        txt[first_len] = '\0';
        ESP_LOGW(TAG, "first %u bytes: %s| %s", first_len, hex, txt);
    }
}

void BtModule::link_lost(const char* why) {
    bt_cover::clear();
    linked_ = false;
    state_set("bt.link", false);
    state_set("bt.state", "off");
    state_set("bt.peer", "");
    // The announced stream format belongs to the connection that announced it. Leaving it
    // behind let a stale rate drive the converter's ratio after the satellite came back on a
    // different one, and the servo has nowhere near the authority to argue with an 8.8 %
    // error. Zero means "measure it" and the measurement is self-correcting.
    state_set("bt.rate_hz", (int32_t)0);
    sat::stream_control().clear();
    // Nobody left to be a slave: the link's clock lines go back to being inputs.
    sat::stream_control().set_host_clocks(false);
    modern_format_ = false;
    // The capabilities belonged to that satellite. A replacement may be an older build, and
    // assuming the old bits would send it a message it has never heard of.
    peer_caps_ = 0;
    ready_budget_ms_ = 0;     // nobody is asking any more
    // Nothing is streaming down a link that is gone, and a pause placed over a link that
    // then dropped is not ours to undo when it comes back — the phone may have been
    // unpaired, replaced or restarted in between.
    streaming_   = false;
    auto_paused_ = false;
    // And the volume must be pushed again on the next link, or a reconnecting phone shows its
    // own slider while the room plays at the panel's level.
    vol_echo_ = -1;
    vol_gate_.link_lost();
    // 1.8: the settings were that satellite's, and a pairing it asked about will not be
    // answered over a link that is gone.
    bt_settings::set_link(false, false);
    bt_settings::pair_abandon();
    ESP_LOGW(TAG, "link lost (%s)", why);
}

void BtModule::on_frame(void* ctx, const sat::Frame& f) {
    auto* self = static_cast<BtModule*>(ctx);

    // No ring means no wire task, which means this is already the main task inside
    // read_uart_inline() — act on the frame directly, the way this module always did.
    if (!w.ring) { self->handle(f); return; }

    const size_t total = sizeof(RingHdr) + f.len;
    void*        slot  = nullptr;
    // Never blocks. A full ring means the main loop has not run for long enough to matter,
    // and by then the frame is stale anyway — counting the drop is worth more than waiting
    // here, which would push the backpressure onto the wire and cause a driver overrun.
    if (xRingbufferSendAcquire(w.ring, &slot, total, 0) != pdTRUE) {
        w.ring_drops.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const RingHdr h{f.type, f.seq, (uint16_t)f.len};
    memcpy(slot, &h, sizeof h);
    if (f.len) memcpy(static_cast<uint8_t*>(slot) + sizeof h, f.payload, f.len);
    xRingbufferSendComplete(w.ring, slot);
}

void BtModule::handle(const sat::Frame& f) {
    frames_++;
    since_rx_ms_ = 0;

    switch (f.type) {
    case sat::MSG_HELLO: {
        sat::Hello h;
        if (!sat::parse(f, h)) break;
        hello_seen_ = true;
        peer_minor_ = h.minor;
        // Sanitised the way the satellite sanitises ours: a capability the peer's own
        // version predates cannot be honoured whatever the bit says, and the contract puts
        // the retry behind the BIT rather than the version because a 1.7 satellite may
        // still decline the OBEX traffic.
        peer_caps_ = sat::device_caps(h.minor, h.caps);
        if (peer_minor_ < 3) modern_format_ = false;
        if (h.major != sat::PROTO_MAJOR) {
            // The contract is explicit that this does not stop the audio: I2S does not go
            // through this link, and a device that plays without a screen beats one that
            // sulks. So we say it once and keep answering pings.
            ESP_LOGE(TAG, "satellite speaks protocol %u.%u, we speak %u.%u — link refused",
                     h.major, h.minor, sat::PROTO_MAJOR, sat::PROTO_MINOR);
            bt_cover::clear();
            linked_ = false;
            sat::stream_control().clear();
            sat::stream_control().set_host_clocks(false);
            modern_format_ = false;
            peer_caps_ = 0;
            state_set("bt.link", false);
            bt_settings::set_link(false, false);
            break;
        }
        // 1.9: drive BCLK/WS only while this HELLO says the satellite is a slave. Cleared
        // the moment a HELLO stops saying it, so two drivers never share the wire for
        // longer than one greeting.
        {
            const bool slave = (peer_caps_ & sat::DEV_CAP_I2S_SLAVE) != 0 &&
                               (hello_caps() & sat::HOST_CAP_I2S_MASTER) != 0;
            if (slave != sat::stream_control().host_clocks())
                ESP_LOGI(TAG, "link clock: %s", slave ? "ours (satellite is an I2S slave)"
                                                     : "the satellite's (legacy master)");
            sat::stream_control().set_host_clocks(slave);
            state_set("bt.link_master", slave);
        }
        const char* build = cstr(h.build);
        const bool was = linked_;
        linked_ = true;
        state_set("bt.link", true);
        state_set("bt.sat", build);
        bt_settings::set_link(true, (peer_caps_ & sat::DEV_CAP_BT_SETTINGS) != 0);
        if (!was) {
            ESP_LOGI(TAG, "link up: %s, proto %u.%u, caps 0x%08lx (%lu bytes to get here)",
                     build, h.major, h.minor, (unsigned long)h.caps,
                     (unsigned long)link_->stats().rx_bytes);
            if (!(h.caps & sat::DEV_CAP_A2DP))
                ESP_LOGI(TAG, "satellite reports no A2DP — a link-only build");
            name_given_ = false;   // maybe another satellite: it may be given the panel's name once
        }
        // The satellite acts on no command from a host that has not greeted it since it
        // started or since we went quiet (protocol section 3), and a greeting of ours from
        // under a second ago may have reached a satellite that was still booting. So a link
        // coming up gets our HELLO now, ahead of the first command, past the reply limit.
        // The satellite does not answer it inside its own one-second window: no loop.
        if (!was) since_hello_ms_ = HELLO_EVERY_MS;
        say_hello();
        break;
    }

    case sat::MSG_PING: {
        sat::Ping p;
        if (sat::parse(f, p)) {
            const size_t n = sat::build_pong(*tx_, p.tick_ms, now_ms());
            send(n);
            if (n) pongs_++;
        }
        break;
    }

    case sat::MSG_PONG: {
        sat::Pong p;
        if (!sat::parse(f, p)) break;
        const uint32_t rtt = now_ms() - p.echo_ms;
        state_set("bt.rtt_ms", (int32_t)rtt);
        break;
    }

    case sat::MSG_BT_STATE: {
        sat::BtState s;
        if (!sat::parse(f, s)) break;
        state_set("bt.state", state_name(s.state));
        // Whether A2DP audio is actually flowing, which is the condition item 12.6 acts on.
        // BT_STREAMING and not TRACK_POS: a phone with no AVRCP metadata never sends a
        // TRACK_POS at all, and it is still decoding into our I2S port.
        streaming_ = (s.state == sat::BT_STREAMING);
        // A phone that has just connected is told the room's level, and the one it brings
        // along is not taken (bt_volume_gate.h).
        if (vol_gate_.on_state(s.state >= sat::BT_CONNECTED)) vol_echo_ = -1;
        if (s.state < sat::BT_CONNECTED) {
            bt_cover::clear();
            sat::stream_control().clear();
            state_set("bt.rate_hz", (int32_t)0);
            state_set("bt.bits", (int32_t)0);
            // The track left with the phone. A title and a frozen progress bar left behind
            // is how a disconnected player looks exactly like a playing one — the same
            // reason the cover above it is cleared here.
            state_set("bt.title", "");
            state_set("bt.artist", "");
            state_set("bt.album", "");
            state_set("bt.pos_s", (int32_t)0);
            state_set("bt.dur_s", (int32_t)0);
            state_set("bt.play_state", "idle");
            // A pause we placed on the phone that has just left must not be resumed on the
            // next phone that connects.
            auto_paused_ = false;
            stall_.on_phone_gone();
        }
        state_set("bt.codec", codec_name(s.codec));
        const char* peer = cstr(s.name);
        state_set("bt.peer", peer);
        ESP_LOGI(TAG, "%s%s%s%s%s", state_name(s.state),
                 peer[0] ? ": " : "", peer,
                 codec_name(s.codec)[0] ? ", " : "", codec_name(s.codec));
        break;
    }

    case COVER_RESET:
        if (linked_ && peer_minor_ >= 5) bt_cover::reset(f.payload,f.len);
        break;
    case COVER_DATA:
        if (linked_ && peer_minor_ >= 5) bt_cover::data(f.payload,f.len,now_ms(),peer_minor_>=6);
        break;
    case COVER_END:
        if (linked_ && peer_minor_ >= 6) bt_cover::end(f.payload,f.len);
        break;

    case sat::MSG_TRACK_META: {
        sat::TrackMeta m;
        if (!sat::parse(f, m)) break;
        // The first one after a connection is very often empty, by measurement and by
        // contract. Empty is a legal value, not a short read: write it through so a stale
        // title from the previous phone does not sit on the screen.
        // One field at a time: cstr() hands back the same scratch every call, and state_set
        // copies before the next one overwrites it.
        state_set("bt.title",  cstr(m.title, MODESP_MAX_LONG_STRING_LENGTH));
        state_set("bt.artist", cstr(m.artist));
        state_set("bt.album",  cstr(m.album));
        break;
    }

    case sat::MSG_TRACK_POS: {
        sat::TrackPos p;
        if (!sat::parse(f, p)) break;
        // Both millisecond fields have already been through the contract's sanity rule in
        // parse(), so what is left is either a believable position or a zero meaning
        // "unknown" — which is the same thing the file player publishes for a stream whose
        // length it cannot work out, and the screen already knows not to draw a bar for it.
        //
        // Seconds, because that is the unit of every other position key here and the step
        // the bar moves in. Truncated: 1.9 s into a track is still second one, and rounding
        // up would let the bar reach the end before the track does.
        state_set("bt.pos_s", (int32_t)(p.elapsed_ms / 1000u));
        state_set("bt.dur_s", (int32_t)(p.total_ms / 1000u));
        state_set("bt.play_state", play_state_name(p.state));
        break;
    }

    case FMT_PREPARE: {
        if (!linked_ || peer_minor_ < 3) break;
        fmt_request_t request{};
        if (!fmt_decode(f.payload, f.len, &request)) break;
        modern_format_ = true;
        // Every PREPARE refills the budget, first or retry, refused or accepted: the
        // satellite asking again is the only evidence that it has not heard the answer.
        ready_budget_ms_ = READY_REPEAT_MS;
        if (!sat::stream_control().publish(request)) {
            fmt_ready_t refused{request.nonce, request.generation, FMT_REFUSED};
            uint8_t payload[9]; fmt_encode_ready(payload, &refused);
            send(tx_->frame(FMT_READY, payload, sizeof payload));
            break;
        }
        state_set("bt.rate_hz", (int32_t)request.rate_hz);
        state_set("bt.bits", (int32_t)request.bits);
        break;
    }
    case sat::MSG_STREAM_FMT: {
        sat::StreamFmt s;
        if (!sat::parse(f, s)) break;
        if (modern_format_) break; // negotiated PREPARE is the authority
        sat::stream_control().publish({0, 0, s.rate_hz, s.bits, s.channels});
        state_set("bt.rate_hz", (int32_t)s.rate_hz);
        state_set("bt.bits", (int32_t)s.bits);
        ESP_LOGI(TAG, "stream: %lu Hz, %u-bit slots, %u ch",
                 (unsigned long)s.rate_hz, s.bits, s.channels);
        // Both fields are acted on now: the audio module moves the input channel to this
        // width and this rate, and takes the output clock with it. Announcing a width the
        // port cannot take is the one case left, and it is loud rather than silent — the
        // symptom of getting it wrong quietly was half the frames and the two channels glued
        // into one word, which sounds like a bad codec and took a day to find by ear.
        if (s.bits && s.bits != 16 && s.bits != 32)
            ESP_LOGE(TAG, "stream announces %u-bit slots, which this port cannot take — audio "
                          "from this connection will be wrong", s.bits);
        break;
    }

    case sat::MSG_VOLUME_STATE: {
        sat::Volume v;
        if (!sat::parse(f, v)) break;
        state_set("bt.volume", (int32_t)v.volume);
        // The satellite runs the AVRCP target role and deliberately does NOT attenuate: the
        // phone now sends full scale and the number separately, so whoever scales it loses
        // resolution, and our DSP has the most headroom to lose it in. Applying it is ours.
        //
        // Only when the phone is the one that moved it. source = 0 is our own change coming
        // back around, and acting on that is how a volume control walks away on its own.
        // And only a level somebody chose while listening to Bluetooth: not the one a phone
        // restores as it connects, and not one set while another source plays. Those are
        // answered with the room's level instead (bt_volume_gate.h).
        if (v.source == 1) {
            const int32_t ours = from_avrcp(v.volume);
            if (vol_gate_.take(read_int("audio.source", -1) == AUDIO_SRC_BT)) {
                vol_echo_ = ours;                // do not send this straight back
                state_set("audio.volume", ours);
                ESP_LOGI(TAG, "volume %u on the phone -> %ld%%", v.volume, (long)ours);
            } else if (!vol_gate_.answer()) {
                // Another source plays: the press is the phone's own business, and its slider
                // is not thrown back. Picking Bluetooth tells the phone the room's level.
                ESP_LOGI(TAG, "volume %u on the phone not taken (Bluetooth is not the source)",
                         v.volume);
            } else if (ours != read_int("audio.volume", -1)) {
                // Connecting: the next pass tells the phone ours. Only when it differs: a phone
                // that answers our level by setting it would otherwise be answered again.
                vol_echo_ = -1;
                ESP_LOGI(TAG, "volume %u on the phone not taken (it is connecting), the room "
                              "stays at %ld%%", v.volume, (long)read_int("audio.volume", -1));
            }
        }
        break;
    }

    case sat::MSG_LOG: {
        sat::LogLine g;
        if (!sat::parse(f, g)) break;
        const char* text = cstr(g.text);
        switch (g.level) {
            case 0:  ESP_LOGE(TAG, "sat: %s", text); break;
            case 1:  ESP_LOGW(TAG, "sat: %s", text); break;
            case 3:  ESP_LOGD(TAG, "sat: %s", text); break;
            default: ESP_LOGI(TAG, "sat: %s", text); break;
        }
        break;
    }

    // ── 1.8: settings and pairing, from a satellite that said it speaks them ──
    case sat::MSG_PAIR_REQUEST: {
        if (!linked_ || !(peer_caps_ & sat::DEV_CAP_BT_SETTINGS)) break;
        sat::PairRequest r;
        if (!sat::parse(f, r)) break;
        bt_settings::pair_begin(r.addr, r.kind, r.passkey, r.name.p, r.name.len);
        // The passkey is not logged: it is the one thing that proves the phone is the one
        // in the room, and the log leaves the room.
        ESP_LOGI(TAG, "pairing request, kind %u", r.kind);
        break;
    }
    case sat::MSG_PAIR_RESULT: {
        if (!linked_ || !(peer_caps_ & sat::DEV_CAP_BT_SETTINGS)) break;
        sat::PairResult r;
        if (!sat::parse(f, r)) break;
        bt_settings::pair_end(r.addr, r.status);
        ESP_LOGI(TAG, "pairing result %u", r.status);
        break;
    }
    case sat::MSG_BT_CONFIG: {
        if (!linked_ || !(peer_caps_ & sat::DEV_CAP_BT_SETTINGS)) break;
        sat::BtConfig c;
        if (!sat::parse(f, c)) break;
        char name[bt_settings::NAME_CAP];
        copy_utf8(name, sizeof name, c.name);
        bt_settings::set_config(c.radio != 0, c.discoverable != 0, c.auto_reconnect != 0,
                                c.codecs_supported, c.codecs_enabled, name, strlen(name));
        // The panel's one name (device_name.h): the first the satellite tells is kept — somebody
        // gave it that name — and after that a satellite that comes up with another (reflashed,
        // swapped) is given the panel's back, once. A rename that crossed this report is sent
        // again, which changes nothing. Only once: a name the satellite does not apply leaves its
        // old one in the answer (LINK_PROTOCOL.md, BT_CONFIG_SET), and answering every such
        // BT_CONFIG bounced the two frames off each other at wire speed. A report that agrees
        // ends the wait, so a satellite that later comes up with another name gets one more.
        if (name[0]) {
            if (!device_name::kept()) {
                device_name::keep(name);
            } else {
                char mine[device_name::CAP];
                device_name::get(mine, sizeof mine);
                if (strcmp(mine, name) == 0) {
                    name_given_ = false;
                } else if (!name_given_) {
                    name_given_ = true;
                    ESP_LOGI(TAG, "satellite named \"%s\", the panel \"%s\": giving it the panel's, "
                                  "once on this link", name, mine);
                    bt_settings::request_name(mine);
                }
            }
        }
        break;
    }
    case sat::MSG_BOND_LIST: {
        if (!linked_ || !(peer_caps_ & sat::DEV_CAP_BT_SETTINGS)) break;
        // ~900 bytes of scratch, so PSRAM and made once — not the main task's stack.
        struct Scratch { sat::Bond raw[sat::BOND_MAX]; bt_settings::Bond out[bt_settings::BONDS]; };
        static Scratch* s = make<Scratch>();
        if (!s) break;
        size_t  count = 0;
        uint8_t total = 0;
        if (!sat::parse_bond_list(f, s->raw, sat::BOND_MAX, count, total)) break;
        if (count > sat::BOND_MAX) count = sat::BOND_MAX;
        for (size_t i = 0; i < count; i++) {
            memcpy(s->out[i].addr, s->raw[i].addr, 6);
            s->out[i].flags = s->raw[i].flags;
            copy_utf8(s->out[i].name, sizeof s->out[i].name, s->raw[i].name);
        }
        bt_settings::set_bonds(total, s->out, (uint8_t)count);
        break;
    }

    default:
        // Anything else is dropped, including a 1.9 satellite's GPIO_MAP: 1.10 retired
        // the expander's codes 0x30..0x35 (docs/LINK_PROTOCOL.md, section 11).
        break;
    }
}

/// Stop the phone while the player is listening to something else, and pick it up again.
///
/// Nothing in this player reads I2S-B unless Bluetooth is the selected source, and the
/// satellite has no way to know which source that is: it keeps the A2DP connection, keeps
/// decoding LHDC and keeps clocking samples into a port nobody is reading. One AVRCP PAUSE
/// stops all of it, and the phone's own screen then agrees with the room.
void BtModule::gate_on_source(uint32_t dt_ms) {
    if (since_auto_pause_ms_ < AUTO_PAUSE_EVERY_MS) since_auto_pause_ms_ += dt_ms;

    // Guarded on the key EXISTING rather than on its value, because a build without the
    // audio module must not read a default of 0 as "not Bluetooth" and pause a phone the
    // user is listening to through some other path.
    if (!state_get("audio.source").has_value()) return;
    const bool selected = read_int("audio.source", 0) == AUDIO_SRC_BT;
    // Picked: the phone's slider learns the room's level, so its next press starts from there
    // (it was left alone while another source played — bt_volume_gate.h).
    if (vol_gate_.on_selected(selected)) vol_echo_ = -1;
    auto phone_playing = [this] {
        char st[12]{};
        read_string("bt.play_state", st, sizeof st);
        return strcmp(st, "playing") == 0;
    };
    // A stall needs Bluetooth selected and nothing streaming, so the key is read only then.
    const bool kick = stall_.tick(dt_ms, selected, selected && !streaming_ && phone_playing(),
                                  streaming_);

    if (!selected) {
        if (streaming_ && since_auto_pause_ms_ >= AUTO_PAUSE_EVERY_MS) {
            // Pausing is driven by BT_STREAMING, because that is the thing being wasted.
            // Being ELIGIBLE FOR A RESUME is driven by the phone having said it was
            // playing, which is a narrower question: plenty of phones keep the A2DP stream
            // open while paused, and resuming one of those would start music the user had
            // stopped on the phone itself. A phone that sends no TRACK_POS at all reads as
            // "not known to be playing", which errs towards leaving it alone.
            //
            // Once true it stays true until something settles the question, because our own
            // first PAUSE turns play_state into "paused" and a retry would otherwise
            // withdraw the resume it had just earned.
            auto_paused_ = auto_paused_ || phone_playing();
            since_auto_pause_ms_ = 0;
            send(sat::build_transport(*tx_, sat::TR_PAUSE));
            ESP_LOGI(TAG, "paused the phone: the selected source is not Bluetooth (%s)",
                     auto_paused_ ? "will resume on return" : "no resume, it was not playing");
        }
        return;
    }

    // Coming back to Bluetooth resumes ONLY the pause this module placed.
    //
    // Playing again on return was considered and deliberately narrowed to this. A phone is
    // somebody else's device and the panel does not know what it has been doing since — a
    // podcast, a call, navigation — so "selected Bluetooth" is a request to listen, not an
    // instruction to start. Worse, an unconditional resume would undo the user's own pause:
    // pause from this screen, glance at the radio, come back, and the music is playing
    // again. `auto_paused_` is set by the branch above and cleared by every button press,
    // every disconnect and every lost link, so the only thing that can be resumed here is a
    // pause the player imposed on a phone that was playing at the time.
    if (auto_paused_) {
        auto_paused_ = false;
        since_auto_pause_ms_ = AUTO_PAUSE_EVERY_MS;   // ready to pause again immediately
        send(sat::build_transport(*tx_, sat::TR_PLAY));
        stall_.on_play();
        ESP_LOGI(TAG, "Bluetooth selected again — resuming the pause we placed");
    } else if (kick) {
        // The same resume, for a pause this module no longer remembers placing: the phone
        // says it plays, so the PLAY starts nothing there, and the satellite starts the
        // stream it suspended (bt_stall_kick.h).
        send(sat::build_transport(*tx_, sat::TR_PLAY));
        stall_.on_play();
        ESP_LOGI(TAG, "the phone says it plays and no stream came in %lu s: PLAY, so the "
                      "satellite starts a stream it suspended",
                 (unsigned long)(bt_stall::Kick::HOLD_MS / 1000));
    }
}
