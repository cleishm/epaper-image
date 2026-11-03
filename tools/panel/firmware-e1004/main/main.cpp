// Panel viewer for the Seeed reTerminal E1004 (ESP32-S3 + 13.3" 1200x1600
// Spectra 6 EL133UF1 panel). Receives one packed native frame (4 bpp, high
// nibble = left pixel, 960000 bytes) over UART0 and shows it pixel-exact.
// The wire protocol (shared with the EE05 firmware, ../../components/
// epi_protocol) is specified in ../../README.md; the host side is
// ../../send.py.

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <epi/protocol.hpp>
#include <esp_log.h>
#include <exception>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <idfxx/epaper/color6_framebuffer>
#include <idfxx/epaper/el133uf1>
#include <idfxx/gpio>
#include <idfxx/spi/master>
#include <optional>
#include <soc/uart_pins.h>
#include <span>

namespace {

// --- reTerminal E1004 wiring (same as the idfxx e1004_demo) ---
constexpr auto PIN_SCLK = idfxx::gpio_7;
constexpr auto PIN_MOSI = idfxx::gpio_9;
constexpr auto PIN_MISO = idfxx::gpio_8;
constexpr auto PIN_CS = idfxx::gpio_10;
constexpr auto PIN_CS1 = idfxx::gpio_2;
constexpr auto PIN_DC = idfxx::gpio_11;
constexpr auto PIN_RST = idfxx::gpio_38;
constexpr auto PIN_BUSY = idfxx::gpio_13;
constexpr auto PIN_POWER = idfxx::gpio_12; // display power enable, active high
constexpr auto PIN_SD_CS = idfxx::gpio_14; // microSD CS shares MISO: hold high

// --- Protocol constants (keep in sync with send.py and README.md) ---
constexpr uart_port_t PORT = UART_NUM_0;
// 2000000 was the target, but host->device bytes sent at 2 Mbaud through the
// E1004's CH34x bridge never arrived (device->host was fine); 921600 is
// reliable. See ../../README.md.
constexpr int BAUD = 921600;
constexpr uint16_t WIDTH = 1200;
constexpr uint16_t HEIGHT = 1600;
constexpr uint32_t FRAME_BYTES = (WIDTH + 1) / 2 * HEIGHT; // 960000

using epi::send_line;

// Byte b is valid iff both nibbles are panel codes 0,1,2,3,5,6
// (black, white, yellow, red, blue, green). 4 and 7..15 are rejected.
constexpr std::array<bool, 256> make_valid_bytes() {
    std::array<bool, 256> t{};
    auto ok = [](unsigned v) { return v <= 6 && v != 4; };
    for (unsigned b = 0; b < 256; ++b) {
        t[b] = ok(b >> 4) && ok(b & 0xF);
    }
    return t;
}
constexpr auto VALID_BYTE = make_valid_bytes();

// Returns the offset of the first byte with an invalid nibble, or -1.
long first_invalid_byte(const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (!VALID_BYTE[p[i]]) {
            return long(i);
        }
    }
    return -1;
}

struct panel_hw {
    idfxx::gpio power = PIN_POWER;
    idfxx::gpio sd_cs = PIN_SD_CS;
    std::optional<idfxx::spi::master_bus> bus;
    std::optional<idfxx::epaper::el133uf1> display;
};

// Brings up the panel (power, SPI, driver with presence check) and puts it
// to sleep until the first frame arrives. Throws std::system_error.
void panel_setup(panel_hw& hw) {
    hw.power.set_direction(idfxx::gpio::mode::output);
    hw.power.set_level(idfxx::gpio::level::high);
    vTaskDelay(pdMS_TO_TICKS(100));

    hw.sd_cs.set_level(idfxx::gpio::level::high);
    hw.sd_cs.set_direction(idfxx::gpio::mode::output);

    idfxx::spi::bus_config bus_cfg{};
    bus_cfg.mosi = PIN_MOSI;
    bus_cfg.miso = PIN_MISO;
    bus_cfg.sclk = PIN_SCLK;
    hw.bus.emplace(idfxx::spi::host_device::spi2, idfxx::spi::dma_chan::ch_auto, bus_cfg);
    hw.display.emplace(
        *hw.bus,
        idfxx::epaper::el133uf1::config{
            .master_cs_gpio = PIN_CS,
            .slave_cs_gpio = PIN_CS1,
            .dc_gpio = PIN_DC,
            .reset_gpio = PIN_RST,
            .busy_gpio = PIN_BUSY,
            .check_presence = true,
        }
    );
    hw.display->sleep();
}

// Set up in app_main before serving.
idfxx::epaper::color6_framebuffer* g_fb = nullptr;
panel_hw* g_hw = nullptr;
char g_init_error[96] = "";

// One frame: validate the header, receive the chunked payload into the
// framebuffer, check CRC and nibbles, then display.
bool handle_frame(const epi::header& h) {
    if (h.kind != '1') {
        send_line("err header EPI%c not supported: this is the E1004 viewer (EPI1 only)", h.kind);
        return false;
    }
    if (h.width != WIDTH || h.height != HEIGHT || h.length != FRAME_BYTES) {
        send_line(
            "err header %ux%u len %lu (want %ux%u len %lu)",
            h.width,
            h.height,
            (unsigned long)h.length,
            WIDTH,
            HEIGHT,
            (unsigned long)FRAME_BYTES
        );
        return false;
    }
    if (g_init_error[0] != '\0') {
        send_line("err panel init failed: %s", g_init_error);
        return false;
    }

    // Payload received straight into the framebuffer.
    const std::span<uint8_t> dst[]{g_fb->data()};
    if (!epi::receive_payload(dst, h)) {
        return false;
    }
    const uint8_t* data = g_fb->data().data();
    if (long bad = first_invalid_byte(data, h.length); bad >= 0) {
        const unsigned b = data[bad];
        const unsigned x = unsigned(bad % ((WIDTH + 1) / 2)) * 2;
        const unsigned y = unsigned(bad / ((WIDTH + 1) / 2));
        send_line("err invalid ink code at byte %ld (x=%u..%u, y=%u): 0x%02x", bad, x, x + 1, y, b);
        return false;
    }

    try {
        auto& d = *g_hw->display;
        if (d.asleep()) {
            d.wake();
        }
        d.write(*g_fb);
        const auto t0 = std::chrono::steady_clock::now();
        d.refresh();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);
        d.sleep();
        send_line("done %lld", (long long)ms.count());
        return true;
    } catch (const std::exception& e) {
        send_line("err panel: %s", e.what());
        try {
            g_hw->display->sleep();
        } catch (...) {
        }
        return false;
    }
}

} // namespace

extern "C" void app_main() {
    esp_log_level_set("*", ESP_LOG_WARN);
    // UART0 is already the console; the transport takes it over with the
    // driver (RX buffer epi::RX_BUFFER) and routes stdio through it.
    static epi::uart_transport link(PORT, BAUD, U0TXD_GPIO_NUM, U0RXD_GPIO_NUM);
    epi::use(link);

    try {
        // Allocate the frame first (PSRAM): it is reused for every transfer.
        static idfxx::epaper::color6_framebuffer fb(WIDTH, HEIGHT);
        g_fb = &fb;

        static panel_hw hw;
        g_hw = &hw;
        panel_setup(hw);
    } catch (const std::exception& e) {
        std::snprintf(g_init_error, sizeof g_init_error, "%s", e.what());
        send_line("err panel init failed: %s", g_init_error);
    }

    epi::serve(handle_frame);
}
