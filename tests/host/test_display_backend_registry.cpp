/**
 * @file test_display_backend_registry.cpp
 * @brief HOST tests for DriverRegistry MODULE-BOUND backend dispatch
 *        (type-string → factory; колишній module-local DisplayBackendRegistry).
 *
 * Started as the display registry and now covers every family that a MODULE resolves in its
 * own on_bind() instead of DriverManager creating it: display, lcd, storage and
 * link. They share one invariant worth pinning per family — a module-bound type is
 * invisible to is_known(), so DriverManager skips its binding silently rather than warning
 * about a driver it was never meant to create.
 *
 * Чиста логіка реєстру. Фабрики backend-ів самі — device-only, тож тут
 * перевіряємо реєстр через фейк-фабрику.
 */

#include "doctest.h"
#include "modesp/hal/driver_registry.h"
#include "modesp/hal/display_port.h"
#include "modesp/hal/storage_volume.h"
#include "modesp/hal/byte_link.h"
#include "modesp/hal/lcd_port.h"
#include "modesp/hal/hal.h"        // modesp::Binding, modesp::HAL (mocked на host)

#include <string>

using namespace modesp::display;
using modesp::DriverRegistry;

namespace {

/// Мінімальний IDisplayPort — лише чисті віртуали.
class FakePort : public IDisplayPort {
public:
    void present_main(const MainView&) override {}
    void present_menu(const MenuView&) override {}
    void present_edit(const EditView&) override {}
    void present_notice(const Notice&) override {}
    void clear_notice() override {}
};

FakePort g_port_a;
FakePort g_port_b;

IDisplayPort* factory_a(const modesp::Binding&, modesp::HAL&) { return &g_port_a; }
IDisplayPort* factory_b(const modesp::Binding&, modesp::HAL&) { return &g_port_b; }
IDisplayPort* factory_null(const modesp::Binding&, modesp::HAL&) { return nullptr; }

} // namespace

TEST_CASE("display registry: register + has + create dispatch") {
    DriverRegistry::reset();
    CHECK(DriverRegistry::register_display("amt630a", &factory_a));
    CHECK(DriverRegistry::register_display("ssd1306", &factory_b));

    CHECK(DriverRegistry::has_display("amt630a"));
    CHECK(DriverRegistry::has_display("ssd1306"));
    CHECK_FALSE(DriverRegistry::has_display("at7456e"));
    // display-типи НЕ видимі для sensor/actuator-шляху DriverManager-а
    CHECK_FALSE(DriverRegistry::is_known("amt630a"));
    CHECK(DriverRegistry::is_module_backend("amt630a"));

    modesp::Binding b;
    modesp::HAL hal;
    CHECK(DriverRegistry::create_display("amt630a", b, hal) == &g_port_a);
    CHECK(DriverRegistry::create_display("ssd1306", b, hal) == &g_port_b);
}

TEST_CASE("display registry: unknown type → nullptr, not known") {
    DriverRegistry::reset();
    DriverRegistry::register_display("amt630a", &factory_a);

    modesp::Binding b;
    modesp::HAL hal;
    CHECK(DriverRegistry::create_display("nope", b, hal) == nullptr);
    CHECK_FALSE(DriverRegistry::has_display("nope"));
    CHECK(DriverRegistry::create_display(nullptr, b, hal) == nullptr);
}

TEST_CASE("display registry: registration is idempotent (first factory wins)") {
    DriverRegistry::reset();
    CHECK(DriverRegistry::register_display("amt630a", &factory_a));
    // Повторна реєстрація того ж типу — no-op (true), фабрика не підмінюється.
    CHECK(DriverRegistry::register_display("amt630a", &factory_b));

    modesp::Binding b;
    modesp::HAL hal;
    CHECK(DriverRegistry::create_display("amt630a", b, hal) == &g_port_a);
}

TEST_CASE("display registry: null args rejected") {
    DriverRegistry::reset();
    CHECK_FALSE(DriverRegistry::register_display(nullptr, &factory_a));
    CHECK_FALSE(DriverRegistry::register_display("x", nullptr));
}

TEST_CASE("display registry: factory returning nullptr propagates") {
    DriverRegistry::reset();
    DriverRegistry::register_display("broken", &factory_null);
    modesp::Binding b;
    modesp::HAL hal;
    CHECK(DriverRegistry::has_display("broken"));                       // тип відомий
    CHECK(DriverRegistry::create_display("broken", b, hal) == nullptr); // але фабрика не змогла
}

TEST_CASE("display registry: capacity bound + reset") {
    DriverRegistry::reset();
    // Реєстр зберігає const char* (string-літерали з MODESP_REGISTER_DISPLAY),
    // тож імена мають бути окремими стабільними рядками (не один буфер).
    static const char* names[] = {
        "b0","b1","b2","b3","b4","b5","b6","b7",
        "b8","b9","b10","b11","b12","b13","b14","b15",
    };
    REQUIRE(DriverRegistry::MAX_DRIVER_TYPES <= 16);
    for (size_t i = 0; i < DriverRegistry::MAX_DRIVER_TYPES; ++i) {
        CHECK(DriverRegistry::register_display(names[i], &factory_a));
    }
    // Понад місткість — відмова (не мовчазне переповнення).
    CHECK_FALSE(DriverRegistry::register_display("overflow", &factory_a));

    DriverRegistry::reset();
    CHECK_FALSE(DriverRegistry::has_display("b0"));
}

// ── lcd family (player-fork): panel/touch backends share one registry slot type ──

namespace {

class FakePanel : public modesp::lcd::ILcdPort, public modesp::lcd::ILcdPanel {
public:
    modesp::lcd::ILcdPanel* as_panel() override { return this; }
    bool init() override { return true; }
    modesp::lcd::LcdGeometry geometry() const override { return {480, 800, 16}; }
    esp_lcd_panel_t* panel() const override { return nullptr; }
    esp_lcd_panel_io_t* io() const override { return nullptr; }
    void set_backlight(uint8_t) override {}
    bool is_healthy() const override { return true; }
};

class FakeTouch : public modesp::lcd::ILcdPort, public modesp::lcd::ITouchPort {
public:
    modesp::lcd::ITouchPort* as_touch() override { return this; }
    bool init() override { return true; }
    esp_lcd_touch_s* handle() const override { return nullptr; }
    bool is_healthy() const override { return true; }
};

FakePanel g_panel;
FakeTouch g_touch;

modesp::lcd::ILcdPort* lcd_factory_panel(const modesp::Binding&, modesp::HAL&) { return &g_panel; }
modesp::lcd::ILcdPort* lcd_factory_touch(const modesp::Binding&, modesp::HAL&) { return &g_touch; }

} // namespace

TEST_CASE("lcd registry: register + has + create + capability cast") {
    DriverRegistry::reset();
    CHECK(DriverRegistry::register_lcd("dsi_st7701", &lcd_factory_panel));
    CHECK(DriverRegistry::register_lcd("gt911", &lcd_factory_touch));

    CHECK(DriverRegistry::has_lcd("dsi_st7701"));
    CHECK(DriverRegistry::has_lcd("gt911"));
    CHECK_FALSE(DriverRegistry::has_lcd("ssd1306"));
    // lcd backends are module-bound: invisible to the sensor/actuator path, skipped by DriverManager
    CHECK_FALSE(DriverRegistry::is_known("dsi_st7701"));
    CHECK(DriverRegistry::is_module_backend("dsi_st7701"));
    CHECK(DriverRegistry::is_module_backend("gt911"));
    CHECK_FALSE(DriverRegistry::has_display("gt911"));

    modesp::Binding b;
    modesp::HAL hal;
    auto* p = DriverRegistry::create_lcd("dsi_st7701", b, hal);
    REQUIRE(p == &g_panel);
    CHECK(p->as_panel() == &g_panel);
    CHECK(p->as_touch() == nullptr);
    CHECK(p->as_panel()->geometry().v_res == 800);

    auto* t = DriverRegistry::create_lcd("gt911", b, hal);
    REQUIRE(t == &g_touch);
    CHECK(t->as_touch() == &g_touch);
    CHECK(t->as_panel() == nullptr);
}

TEST_CASE("lcd registry: unknown/null + idempotent + reset") {
    DriverRegistry::reset();
    modesp::Binding b;
    modesp::HAL hal;
    CHECK(DriverRegistry::create_lcd("nope", b, hal) == nullptr);
    CHECK(DriverRegistry::create_lcd(nullptr, b, hal) == nullptr);
    CHECK_FALSE(DriverRegistry::register_lcd(nullptr, &lcd_factory_panel));
    CHECK_FALSE(DriverRegistry::register_lcd("x", nullptr));

    CHECK(DriverRegistry::register_lcd("dsi_st7701", &lcd_factory_panel));
    CHECK(DriverRegistry::register_lcd("dsi_st7701", &lcd_factory_touch));   // no-op, first wins
    CHECK(DriverRegistry::create_lcd("dsi_st7701", b, hal) == &g_panel);

    DriverRegistry::reset();
    CHECK_FALSE(DriverRegistry::has_lcd("dsi_st7701"));
}


// ── storage family (player-fork): a mountable volume of files ──

namespace {

class FakeVolume : public modesp::storage::IStorageVolume {
public:
    bool mount() override { mounted_ = true; return true; }
    void unmount() override { mounted_ = false; }
    bool mounted() const override { return mounted_; }
    const char* mount_point() const override { return "/sd"; }
    const char* status() const override { return mounted_ ? "ok" : "no slot"; }
    const char* label() const override { return "FAKE"; }
    uint64_t capacity_bytes() const override { return 32ull * 1024 * 1024 * 1024; }
    uint64_t free_bytes() const override { return 1ull * 1024 * 1024 * 1024; }
    const char* name() const override { return "fake_volume"; }
    bool is_healthy() const override { return mounted_; }
private:
    bool mounted_ = false;
};

FakeVolume g_volume;

modesp::storage::IStorageVolume* storage_factory(const modesp::Binding&, modesp::HAL&) {
    return &g_volume;
}
modesp::storage::IStorageVolume* storage_factory_null(const modesp::Binding&, modesp::HAL&) {
    return nullptr;
}

} // namespace

TEST_CASE("storage registry: register + has + create dispatch") {
    DriverRegistry::reset();
    CHECK(DriverRegistry::register_storage("sdmmc_card", &storage_factory));
    CHECK(DriverRegistry::has_storage("sdmmc_card"));
    CHECK_FALSE(DriverRegistry::has_storage("sdspi"));
    // A volume is module-bound: invisible to the sensor/actuator path, so DriverManager
    // skips its binding instead of creating anything or warning about it every boot.
    CHECK_FALSE(DriverRegistry::is_known("sdmmc_card"));
    CHECK(DriverRegistry::is_module_backend("sdmmc_card"));
    // ...and it does not leak into a neighbouring family's namespace.
    CHECK_FALSE(DriverRegistry::has_audio("sdmmc_card"));
    CHECK_FALSE(DriverRegistry::has_lcd("sdmmc_card"));

    modesp::Binding b;
    modesp::HAL hal;
    auto* v = DriverRegistry::create_storage("sdmmc_card", b, hal);
    REQUIRE(v == &g_volume);
    CHECK_FALSE(v->mounted());
    CHECK(std::string(v->status()) == "no slot");
    CHECK(v->mount());
    CHECK(v->mounted());
    CHECK(std::string(v->mount_point()) == "/sd");
    v->unmount();
    CHECK_FALSE(v->mounted());
}

TEST_CASE("storage registry: a binding naming a driver this build does not have") {
    DriverRegistry::reset();
    DriverRegistry::register_storage("sdmmc_card", &storage_factory);

    modesp::Binding b;
    modesp::HAL hal;
    // This is the shape of `{"hardware":"sd_0","driver":"sdspi",...}` on a build where
    // drivers/sdspi does not exist, or where CONFIG_MODESP_DRIVER_SDSPI=n so its
    // registration hook was never compiled. The module must get a null and disable local
    // files, NOT a driver of the wrong kind and not a crash.
    CHECK_FALSE(DriverRegistry::has_storage("sdspi"));
    CHECK(DriverRegistry::create_storage("sdspi", b, hal) == nullptr);
    CHECK(DriverRegistry::create_storage(nullptr, b, hal) == nullptr);
    // An unknown type is not a module backend either, so DriverManager DOES warn about it —
    // which is the right outcome: the binding names something this firmware cannot provide.
    CHECK_FALSE(DriverRegistry::is_module_backend("sdspi"));
    CHECK_FALSE(DriverRegistry::is_known("sdspi"));

    // A registered type whose factory fails (pool exhausted, hardware id not in board.json)
    // is a different fault with the same outcome for the module.
    DriverRegistry::register_storage("broken", &storage_factory_null);
    CHECK(DriverRegistry::has_storage("broken"));
    CHECK(DriverRegistry::create_storage("broken", b, hal) == nullptr);
}

TEST_CASE("storage registry: null args, idempotent, reset") {
    DriverRegistry::reset();
    CHECK_FALSE(DriverRegistry::register_storage(nullptr, &storage_factory));
    CHECK_FALSE(DriverRegistry::register_storage("x", nullptr));

    CHECK(DriverRegistry::register_storage("sdmmc_card", &storage_factory));
    CHECK(DriverRegistry::register_storage("sdmmc_card", &storage_factory_null));  // no-op, first wins
    modesp::Binding b;
    modesp::HAL hal;
    CHECK(DriverRegistry::create_storage("sdmmc_card", b, hal) == &g_volume);

    DriverRegistry::reset();
    CHECK_FALSE(DriverRegistry::has_storage("sdmmc_card"));
}


// ── link family (player-fork): the wire to a co-processor ──

namespace {

/// A link that can be told what to hand over next, so the Rx contract can be exercised
/// without a UART: Lost means "the rest is gone, reset your framer", and Idle does NOT mean
/// there is nothing to read.
class FakeLink : public modesp::link::IByteLink {
public:
    bool init() override { return up_; }
    bool can_wait() const override { return can_wait_; }
    modesp::link::Rx wait_rx(uint32_t) override { return next_; }
    int read(uint8_t* out, size_t max) override {
        size_t n = 0;
        while (n < max && pos_ < pending_.size()) out[n++] = (uint8_t)pending_[pos_++];
        stats_.rx_bytes += (uint32_t)n;
        return (int)n;
    }
    int write(const uint8_t*, size_t len) override {
        stats_.tx_bytes += (uint32_t)len;
        return (int)len;
    }
    void flush_rx() override { pending_.clear(); pos_ = 0; }
    modesp::link::LinkStats stats() const override { return stats_; }
    modesp::link::RxPin probe_rx() override { return pin_; }
    const char* describe() const override { return "FAKE 460800 8N1 TX=26 RX=27"; }
    const char* name() const override { return "fake_link"; }
    bool is_healthy() const override { return up_; }

    void offer(const std::string& bytes, modesp::link::Rx how) {
        pending_ = bytes; pos_ = 0; next_ = how;
    }
    void lose() { next_ = modesp::link::Rx::Lost; stats_.overruns++; pending_.clear(); pos_ = 0; }

    bool up_ = true;
    bool can_wait_ = true;
    modesp::link::RxPin pin_ = modesp::link::RxPin::High;

private:
    std::string pending_;
    size_t      pos_  = 0;
    modesp::link::Rx next_ = modesp::link::Rx::Idle;
    modesp::link::LinkStats stats_{};
};

FakeLink g_link;

modesp::link::IByteLink* link_factory(const modesp::Binding&, modesp::HAL&) { return &g_link; }
modesp::link::IByteLink* link_factory_null(const modesp::Binding&, modesp::HAL&) { return nullptr; }

} // namespace

TEST_CASE("link registry: register + has + create dispatch") {
    DriverRegistry::reset();
    CHECK(DriverRegistry::register_link("uart_link", &link_factory));
    CHECK(DriverRegistry::has_link("uart_link"));
    CHECK_FALSE(DriverRegistry::has_link("spi_link"));
    CHECK_FALSE(DriverRegistry::is_known("uart_link"));
    CHECK(DriverRegistry::is_module_backend("uart_link"));
    CHECK_FALSE(DriverRegistry::has_storage("uart_link"));

    modesp::Binding b;
    modesp::HAL hal;
    CHECK(DriverRegistry::create_link("uart_link", b, hal) == &g_link);
}

TEST_CASE("link seam: Idle still has bytes, Lost has none and says so") {
    modesp::link::LinkStats before = g_link.stats();

    // A wait that timed out is not a statement about the buffer: the caller drains anyway.
    g_link.offer("hello", modesp::link::Rx::Idle);
    CHECK(g_link.wait_rx(10) == modesp::link::Rx::Idle);
    uint8_t buf[8] = {};
    CHECK(g_link.read(buf, sizeof buf) == 5);
    CHECK(std::string((const char*)buf, 5) == "hello");
    CHECK(g_link.read(buf, sizeof buf) == 0);          // drained

    // A loss is counted, the remainder is already gone, and the caller is told to resync.
    g_link.offer("abc", modesp::link::Rx::Data);
    g_link.lose();
    CHECK(g_link.wait_rx(10) == modesp::link::Rx::Lost);
    CHECK(g_link.read(buf, sizeof buf) == 0);
    CHECK(g_link.stats().overruns == before.overruns + 1);
    CHECK(g_link.stats().rx_bytes == before.rx_bytes + 5);

    CHECK(g_link.write((const uint8_t*)"xy", 2) == 2);
    CHECK(g_link.stats().tx_bytes == before.tx_bytes + 2);

    // The wording of a pin reading is shared, so every link says it the same way.
    CHECK(std::string(modesp::link::rx_pin_words(modesp::link::RxPin::High))
              .find("transmitter at rest") != std::string::npos);
    CHECK(std::string(modesp::link::rx_pin_words(modesp::link::RxPin::Floating))
              .find("attached to nothing") != std::string::npos);
    CHECK(std::string(modesp::link::rx_pin_words(modesp::link::RxPin::Unknown))
              .find("not probeable") != std::string::npos);
}

TEST_CASE("link registry: a binding naming a driver this build does not have") {
    DriverRegistry::reset();
    DriverRegistry::register_link("uart_link", &link_factory);

    modesp::Binding b;
    modesp::HAL hal;
    // `{"hardware":"uart_sat","driver":"spi_link",...}` on a build without drivers/spi_link,
    // or with CONFIG_MODESP_DRIVER_SPI_LINK=n. modules/bt must get a null and disable
    // Bluetooth — a player without a satellite still plays from the card and the network.
    CHECK_FALSE(DriverRegistry::has_link("spi_link"));
    CHECK(DriverRegistry::create_link("spi_link", b, hal) == nullptr);
    CHECK(DriverRegistry::create_link(nullptr, b, hal) == nullptr);
    CHECK_FALSE(DriverRegistry::is_module_backend("spi_link"));

    DriverRegistry::register_link("broken", &link_factory_null);
    CHECK(DriverRegistry::has_link("broken"));
    CHECK(DriverRegistry::create_link("broken", b, hal) == nullptr);
}

TEST_CASE("link registry: null args, idempotent, reset") {
    DriverRegistry::reset();
    CHECK_FALSE(DriverRegistry::register_link(nullptr, &link_factory));
    CHECK_FALSE(DriverRegistry::register_link("x", nullptr));

    CHECK(DriverRegistry::register_link("uart_link", &link_factory));
    CHECK(DriverRegistry::register_link("uart_link", &link_factory_null));   // no-op, first wins
    modesp::Binding b;
    modesp::HAL hal;
    CHECK(DriverRegistry::create_link("uart_link", b, hal) == &g_link);

    DriverRegistry::reset();
    CHECK_FALSE(DriverRegistry::has_link("uart_link"));
}
