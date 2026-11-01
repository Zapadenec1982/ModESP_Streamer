/**
 * @file bt_module.h
 * @brief Our end of the link to the Bluetooth satellite.
 *
 * Bluetooth for this player lives on a second ESP32. It decodes A2DP and clocks the audio
 * into I2S-B as master; this module owns what crosses the wire that carries everything else —
 * what is playing, what the phone did to the volume, and which transport button we want
 * pressed.
 *
 * The wire itself is a bound driver (drivers/uart_link) behind modesp/hal/byte_link.h, named
 * by bindings.json under the role `sat_link` (item 8.9). This file therefore knows that bytes
 * arrive and that some are sometimes lost; it does not know what a baud rate is.
 *
 * The wire format is docs/LINK_PROTOCOL.md and lives in components/sat_link, which has no
 * ESP-IDF dependency and is covered by the host tests. What is left here is the part that
 * cannot be tested on a host: when to speak, when to give up, and what to do with what
 * arrives.
 */

#pragma once

#include "modesp/base_module.h"
#include "bt_stall_kick.h"
#include "bt_volume_gate.h"
#include <cstdint>

namespace modesp { class DriverManager; struct BindingTable; class HAL; }
namespace modesp::link { class IByteLink; enum class RxPin : uint8_t; }
namespace sat { class Reader; class Writer; struct Frame; }

class BtModule : public modesp::BaseModule {
public:
    BtModule();
    ~BtModule() override;

    void on_bind(modesp::DriverManager& drivers, const modesp::BindingTable& bindings,
                 modesp::HAL& hal) override;
    bool on_init() override;
    void on_update(uint32_t dt_ms) override;
    void on_stop() override;

private:
    /// Reader callback. Runs on the `sat_uart` task, so it does NOT act on the frame — it
    /// copies it into the ring for on_update() to act on, which is what keeps every
    /// state_set on the main task. Falls back to handling inline when there is no ring.
    static void on_frame(void* ctx, const sat::Frame& f);
    void handle(const sat::Frame& f);
    /// Main-loop side: hand every complete frame the wire task collected to handle().
    void drain_frames();
    /// The old polling path, kept for the board where the task or the ring could not be
    /// allocated: a player without Bluetooth is worse than one whose UART is polled.
    void read_uart_inline();
    static void uart_task_entry(void* arg);
    void uart_loop();
    void send(size_t wire_len);
    /// AVRCP passthrough for whatever wrote `bt.cmd` — the panel's transport row, or the
    /// web API. Main loop only, like everything else here that touches SharedState.
    void apply_transport_cmd();
    void apply_one_transport(int32_t cmd);
    /// Protocol 1.8: the screen's settings requests (bt_settings.h) onto the wire.
    void apply_settings_requests();
    /// Keep the phone quiet while the player is listening to another source, and pick it up
    /// again when it comes back.
    void gate_on_source(uint32_t dt_ms);
    void say_hello();
    void link_lost(const char* why);
    void report();
    /// UART overruns, dropped frames and sequence gaps into SharedState.
    void publish_wire_counters();
    /// The settings' revision and the pairing request into SharedState, for every web tab
    /// (bt_http.inc, A5): a change of either, not every tick.
    void publish_settings();
    /// Ask the link what its receive line is electrically attached to, and say so once.
    void probe_rx();

    modesp::HAL*              hal_ = nullptr;
    /// The wire to the satellite, from the `sat_link` binding. Null when this board has none,
    /// or when its driver is not in this build — both of which are a player without
    /// Bluetooth, not a player that fails to boot.
    modesp::link::IByteLink*  link_ = nullptr;
    /// Board id resolved from bindings.json in on_bind(); empty when this board has no
    /// Bluetooth input port. See BindingTable::hardware_for().
    /// 17 = modesp::MAX_HAL_ID_LEN + 1, spelled here so this header keeps forward-declaring
    /// the HAL rather than including it.
    char                      i2s_id_[17]  = "";
    /// The slot width the I2S-B channel was opened with, so a STREAM_FMT that announces a
    /// different one is caught here rather than heard as noise.
    uint8_t                   slot_bits_ = 0;

    /// Together these are about 4 KB of buffers, so they go to PSRAM when there is any:
    /// nothing here is touched by an ISR or by DMA, and internal RAM on this board is the
    /// scarce one.
    sat::Reader* rx_ = nullptr;
    sat::Writer* tx_ = nullptr;

    uint8_t peer_minor_ = 0;
    /// The capability word our last HELLO carried: 1.8's PAIR_CONFIRM comes and goes with the
    /// screen, and a change is announced with a fresh HELLO.
    uint32_t hello_caps_sent_ = 0;
    /// The satellite's HELLO capabilities, already sanitised against its minor: a bit the
    /// peer's own version cannot support is cleared on arrival, so every later test is one
    /// bit test and nobody has to remember which version introduced which bit.
    uint32_t peer_caps_ = 0;
    bool modern_format_ = false;
    uint32_t format_reply_ms_ = 0;
    /// How much longer a STREAM_READY is worth repeating. Refilled by every STREAM_PREPARE
    /// and spent by time, so the repeating stops when the satellite stops asking.
    uint32_t ready_budget_ms_ = 0;
    bool     linked_       = false;
    bool     hello_seen_   = false;  ///< a HELLO arrived at some point, whatever came of it
    /// A2DP audio is flowing (BT_STREAMING), which is what makes pausing worth doing when
    /// the player is listening to something else.
    bool     streaming_    = false;
    /// Since the last automatic PAUSE went out. Saturating, and it starts saturated, so the
    /// first one is never delayed and the counter cannot wrap back under the interval after
    /// forty-nine days of not needing to pause anything.
    uint32_t since_auto_pause_ms_ = 0xFFFFFFFFu;
    uint32_t since_rx_ms_  = 0;   ///< since the last good frame of any kind
    uint32_t since_ping_ms_= 0;
    uint32_t since_hello_ms_ = 0; ///< rate-limits HELLO, which is also the reply to HELLO
    uint32_t since_report_ms_= 0;

    uint32_t frames_   = 0;
    uint32_t pongs_    = 0;   ///< replies actually put on the wire, not just attempted
    /// The last volume this module and the satellite agreed on, in our own 0..100. It exists
    /// to break the echo: a value we just received must not be sent straight back, and a
    /// value we just sent must not come back as a fresh instruction.
    int32_t  vol_echo_ = -1;
    /// Which levels the phone sets become the room's: none while it connects, and none while
    /// Bluetooth is not the selected source (bt_volume_gate.h).
    bt_volume::Gate vol_gate_;
    /// The phone is paused because THIS module paused it, not because anybody asked. It is
    /// the only pause that may be undone automatically, and it is cleared by a button press,
    /// by a disconnect and by a lost link — for the same reason vol_echo_ exists above, that
    /// a control must not act on its own idea of what the user wanted.
    bool     auto_paused_ = false;
    /// The PLAY for a phone that plays while no stream comes, when the pause behind it is one
    /// auto_paused_ no longer remembers (bt_stall_kick.h).
    bt_stall::Kick stall_;
    bool     dumped_    = false;   ///< the first-bytes diagnostic has been printed

    /// Counters written by the `sat_uart` task and read here for the report live in the
    /// .cpp next to that task, not in this header: they need FreeRTOS handles and atomics,
    /// and nothing outside the module may touch them.

    /// What the receive line was last found attached to, so only CHANGES get a line while a
    /// harness is being fixed. Value-initialised rather than named: an opaque enum
    /// declaration gives a complete type but no visible enumerators, and RxPin{} is Unknown.
    modesp::link::RxPin last_pin_{};

    /// What publish_settings() last said, so it says only changes.
    uint32_t pub_cfg_rev_ = 0;
    uint32_t pub_pair_seq_ = 0;
    bool     pub_pair_active_ = false;
    uint8_t  pub_pair_by_ = 0;
    uint8_t  pub_pair_result_ = 0xFF;

    /// The pairing request being timed, and how long it has been open without a result: the
    /// contract sends that result once, and nothing else would ever close a lost one.
    uint32_t pair_seq_seen_ = 0;
    uint32_t pair_open_ms_  = 0;

    /// The panel's name went to a satellite that reported another, and no BT_CONFIG has carried
    /// it back since. Cleared by one that does and by a new link: until then it is not sent
    /// again (handle(), MSG_BT_CONFIG).
    bool     name_given_ = false;
};
