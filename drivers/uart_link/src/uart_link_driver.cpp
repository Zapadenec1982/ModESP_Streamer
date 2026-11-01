/**
 * @file uart_link_driver.cpp
 * @brief The satellite's wire: bytes on and off a UART, and the one thing only the wire knows.
 *
 * Moved out of modules/bt unchanged in order and in effect. It is here because
 * none of it is about the protocol: the RX-FIFO threshold, the event queue that is the only
 * place an overflow is ever reported, and the pin probe are all answers to "is this wire
 * carrying bytes", which is the same question on any board and for any protocol. What is said
 * over the wire stays in modules/bt, and the frames it is said in stay in components/sat_link.
 *
 * The contract this serves: 460800 baud, COBS frames, an 8 KB receive ring (commit 5222100 —
 * 2048 bytes is 44 ms of wire against a drain that was a 100 Hz poll). A lost byte loses the
 * whole frame it was in, so losing one has to be REPORTED rather than merely survived; that
 * is what the overrun count is, and why HAL installs this port with an event queue at all.
 */

#include "uart_link_driver.h"

#include "modesp/hal/hal.h"
#include "modesp/hal/driver_registry.h"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include <cstdio>

static const char* TAG = "uart_link";

using modesp::link::Rx;
using modesp::link::RxPin;
using modesp::link::LinkStats;

void UartLinkDriver::configure(modesp::UartBusResource* bus) {
    bus_ = bus;
}

bool UartLinkDriver::init() {
    if (!bus_ || !bus_->initialized) {
        // Not an error here: a board without the accessory wired is a board that plays from
        // the card and the network. The module decides what to do about it.
        ESP_LOGW(TAG, "uart '%s' not up", bus_ ? bus_->id.c_str() : "?");
        return false;
    }

    const uart_port_t port = (uart_port_t)bus_->port;
    const esp_err_t err = uart_set_rx_full_threshold(port, RX_FULL_THRESHOLD);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART RX threshold: %s", esp_err_to_name(err));
        return false;
    }

    events_ = bus_->rx_events;
    if (!events_)
        ESP_LOGW(TAG, "UART installed without an event queue — overruns will not be counted");

    snprintf(desc_, sizeof desc_, "UART%d %lu 8N1 TX=%d RX=%d",
             bus_->port, (unsigned long)bus_->baud_rate, (int)bus_->tx, (int)bus_->rx);
    ready_ = true;
    return true;
}

Rx UartLinkDriver::wait_rx(uint32_t timeout_ms) {
    if (!ready_ || !events_) return Rx::Idle;

    uart_event_t ev;
    // A timeout rather than portMAX_DELAY, so the caller notices a stop without a wake-up
    // message and a silent partner still lets its loop turn over — which is also what lets
    // that loop keep a heartbeat rather than a traffic meter.
    const bool woke = (xQueueReceive(static_cast<QueueHandle_t>(events_), &ev,
                                     pdMS_TO_TICKS(timeout_ms)) == pdTRUE);
    // An overflow is counted here, the one place it is ever reported, and nothing else is
    // thrown away (IDF v5.5.5, esp_driver_uart/src/uart.c). UART_FIFO_OVF: the ISR has already
    // reset the hardware FIFO, so the loss is done and what the ring holds is good. The
    // UART_DATA event of the same interrupt was queued ahead of this one, so the reader may
    // already have taken bytes from after the loss: the flush that used to follow cut into the
    // next frame, and a framer reset now could throw away a good partial one. So this is not
    // Rx::Lost, which promises that nothing from after the loss has been handed over yet. The
    // frame the loss cut through fails its CRC or its COBS on its own, and the next delimiter
    // puts the framer back in step. UART_BUFFER_FULL has lost nothing at all: the ISR keeps
    // the bytes that did not fit and takes no more until a read makes room
    // (uart_check_buf_full), so it is a wake-up like any other — a reader too slow even then
    // lets the FIFO overflow, and that is counted.
    if (woke && ev.type == UART_FIFO_OVF) overruns_.fetch_add(1, std::memory_order_relaxed);
    return woke ? Rx::Data : Rx::Idle;
}

int UartLinkDriver::read(uint8_t* out, size_t max) {
    if (!ready_ || !out || !max) return 0;
    const int n = uart_read_bytes((uart_port_t)bus_->port, out, max, 0);
    if (n > 0) rx_bytes_.fetch_add((uint32_t)n, std::memory_order_relaxed);
    return n > 0 ? n : 0;
}

int UartLinkDriver::write(const uint8_t* data, size_t len) {
    if (!ready_ || !data || !len) return 0;
    const int n = uart_write_bytes((uart_port_t)bus_->port, data, len);
    if (n > 0) tx_bytes_.fetch_add((uint32_t)n, std::memory_order_relaxed);
    return n > 0 ? n : 0;
}

void UartLinkDriver::flush_rx() {
    if (ready_) uart_flush_input((uart_port_t)bus_->port);
}

LinkStats UartLinkDriver::stats() const {
    LinkStats s;
    s.rx_bytes = rx_bytes_.load(std::memory_order_relaxed);
    s.tx_bytes = tx_bytes_.load(std::memory_order_relaxed);
    s.overruns = overruns_.load(std::memory_order_relaxed);
    return s;
}

/// Asks the pin itself what it is attached to, which no meter at the other end of a wire can
/// answer: a reading taken at a connector says what the connector sees, not what the die
/// sees, and between the two sit a removed 0-ohm link and an assumption about which pad this
/// is.
///
/// Pull it up, read it; pull it down, read it. An input that follows the pull is attached to
/// nothing. An input that refuses both ways is held by something, and which way it refuses
/// says whether that is a transmitter at rest (high) or ground (low).
///
/// Takes about 4 ms and hands the pin straight back to the UART.
RxPin UartLinkDriver::probe_rx() {
    if (!ready_ || bus_->rx == GPIO_NUM_NC) return RxPin::Odd;

    const gpio_num_t pin = bus_->rx;
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << (int)pin;
    cfg.mode         = GPIO_MODE_INPUT;
    cfg.intr_type    = GPIO_INTR_DISABLE;

    // esp_rom_delay_us, not vTaskDelay: the settle below is 200 microseconds, shorter than
    // a tick at any rate this project has run (10 ms at the old 100 Hz, 1 ms now), so
    // vTaskDelay could only round it up to a whole tick — or, when it truncates to zero,
    // become a bare yield that settles nothing and hands off the shared module task for an
    // unbounded time. An internal pull of tens of kilohms against a bare pad settles in
    // microseconds.
    cfg.pull_up_en   = GPIO_PULLUP_ENABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    if (gpio_config(&cfg) != ESP_OK) return RxPin::Odd;
    esp_rom_delay_us(200);
    const int up = gpio_get_level(pin);

    cfg.pull_up_en   = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_ENABLE;
    gpio_config(&cfg);
    esp_rom_delay_us(200);
    const int down = gpio_get_level(pin);

    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&cfg);
    uart_set_pin((uart_port_t)bus_->port, bus_->tx, bus_->rx,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    // gpio_config switches the pin's IO MUX function but leaves the GPIO matrix input still
    // wired to the UART, so while the pull-down was on, the receiver saw a break and filled
    // its ring with framing-error zeros. Those zeros then land in the delimiter count and in
    // the first-bytes dump — the two instruments this probe exists to make readable. Drop
    // them: the probe must not be able to answer its own question.
    uart_flush_input((uart_port_t)bus_->port);

    if (up == 1 && down == 0) return RxPin::Floating;
    if (up == 1 && down == 1) return RxPin::High;
    if (up == 0 && down == 0) return RxPin::Low;
    return RxPin::Odd;
}

// ── registry glue ──────────────────────────────────────────────────────────────────────────

namespace {

UartLinkDriver s_pool[modesp::MAX_UART_BUSES];
size_t         s_n = 0;

modesp::link::IByteLink* uart_link_factory(const modesp::Binding& b, modesp::HAL& hal) {
    if (s_n >= modesp::MAX_UART_BUSES) {
        ESP_LOGE(TAG, "uart_link pool exhausted (max %u)", (unsigned)modesp::MAX_UART_BUSES);
        return nullptr;
    }
    auto* bus = hal.find_uart_bus(
        etl::string_view(b.hardware_id.c_str(), b.hardware_id.size()));
    if (!bus) {
        ESP_LOGE(TAG, "uart '%s' is not in board.json", b.hardware_id.c_str());
        return nullptr;
    }
    auto& drv = s_pool[s_n++];
    drv.configure(bus);
    return &drv;
}

} // namespace

MODESP_REGISTER_LINK(uart_link, &uart_link_factory)
