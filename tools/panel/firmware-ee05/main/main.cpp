// Panel viewer for the Seeed Studio XIAO ePaper Display Board EE05 (XIAO
// ESP32-S3 + 7.5" 800x480 UC8179 panel, GooDisplay GDEY075T7 glass).
// Receives one packed native frame over the XIAO's USB-Serial-JTAG port and
// shows it pixel-exact, in one of two formats chosen by the header magic:
//
//   EPI2  mono:  1 bpp, row-major, 100 bytes per row, bit 7 = leftmost
//         pixel, set bit = white, 48000 bytes; shown with the full (OTP)
//         waveform.
//   EPI3  grey4: 2 bpp, row-major, 200 bytes per row, bits 7-6 = leftmost
//         pixel, 0 = black ... 3 = white, 96000 bytes; shown with the
//         driver's 4-grey waveform (loaded through registers, default 20 Hz
//         at +/-15 V; ~9.0 s) after a black/white scrub.
//
// Both are idfxx write_inks frames, as tools/convert writes them, received
// into the mono_framebuffer or the gray4_framebuffer.
//
// The wire protocol (shared with the E1004 firmware, ../../components/
// epi_protocol) is specified in ../../README.md; the host side is
// ../../send.py.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <epi/protocol.hpp>
#include <esp_log.h>
#include <exception>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <idfxx/epaper/gray4_framebuffer>
#include <idfxx/epaper/ink_writer>
#include <idfxx/epaper/mono_framebuffer>
#include <idfxx/epaper/uc8179>
#include <idfxx/gpio>
#include <idfxx/panel_io>
#include <idfxx/spi/master>
#include <optional>

namespace {

// --- EE05 wiring (same as the idfxx ee05_demo) ---
constexpr auto PIN_SCLK = idfxx::gpio_7;
constexpr auto PIN_MOSI = idfxx::gpio_9; // no MISO: the panel is write-only
constexpr auto PIN_CS = idfxx::gpio_44;
constexpr auto PIN_DC = idfxx::gpio_10;
constexpr auto PIN_RST = idfxx::gpio_38;
constexpr auto PIN_BUSY = idfxx::gpio_4;
constexpr auto PIN_POWER = idfxx::gpio_43; // display power enable, active high

// --- Frame formats (keep in sync with send.py and README.md) ---
constexpr uint16_t WIDTH = 800;
constexpr uint16_t HEIGHT = 480;
constexpr uint32_t MONO_BYTES = WIDTH / 8 * HEIGHT;  // 48000
constexpr uint32_t GREY4_BYTES = WIDTH / 4 * HEIGHT; // 96000
constexpr char KIND_MONO = '2';
constexpr char KIND_GREY4 = '3';

using epi::send_line;
using idfxx::epaper::color_mode;
using idfxx::epaper::refresh_mode;
using clk = std::chrono::steady_clock;

long long ms_since(clk::time_point t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(clk::now() - t0).count();
}

struct panel_hw {
    idfxx::gpio power = PIN_POWER;
    std::optional<idfxx::spi::master_bus> bus;
    std::optional<idfxx::panel_io> io;
    std::optional<idfxx::epaper::uc8179> display;
};

// Brings up the panel (power, SPI, driver) and puts it to sleep until the
// first frame arrives. Throws std::system_error.
void panel_setup(panel_hw& hw) {
    hw.power.set_direction(idfxx::gpio::mode::output);
    hw.power.set_level(idfxx::gpio::level::high);
    vTaskDelay(pdMS_TO_TICKS(100));

    idfxx::spi::bus_config bus_cfg{};
    bus_cfg.mosi = PIN_MOSI;
    bus_cfg.sclk = PIN_SCLK;
    hw.bus.emplace(idfxx::spi::host_device::spi2, idfxx::spi::dma_chan::ch_auto, bus_cfg);
    hw.io.emplace(*hw.bus, idfxx::epaper::uc8179::spi_io_config(PIN_CS, PIN_DC));
    hw.display.emplace(
        *hw.io,
        idfxx::epaper::uc8179::config{
            .reset_gpio = PIN_RST,
            .busy_gpio = PIN_BUSY,
        }
    );
    hw.display->sleep();
}

// Set up in app_main before serving.
idfxx::epaper::mono_framebuffer* g_mono = nullptr;
idfxx::epaper::gray4_framebuffer* g_grey = nullptr;
panel_hw* g_hw = nullptr;
char g_init_error[96] = "";

// Shows the received frame; returns the final refresh's duration in ms.
long long show(bool grey) {
    auto& d = *g_hw->display;
    if (d.asleep()) {
        d.wake();
    }
    d.set_color_mode(color_mode::mono);
    if (!grey) {
        d.write(*g_mono);
        const auto t0 = clk::now();
        d.refresh(refresh_mode::full);
        return ms_since(t0);
    }
    // Grey frames start from a fixed state whatever was shown before:
    // a full black refresh, then white. The black frame fills the
    // previous-image plane with black, so the white refresh (clear() blanks
    // only the new-image plane in mono mode) drives every pixel with the
    // deep black-to-white erase waveform rather than white-to-white. The
    // grey midtones are open-loop drives, so this makes each grey frame's
    // starting point repeatable (it does not remove polarisation from
    // long-held content; see the idfxx_epaper_uc8179 README). The mono
    // framebuffer is free scratch here: it holds no frame.
    g_mono->fill(true);
    d.write(*g_mono);
    d.refresh(refresh_mode::full);
    d.clear();
    d.refresh(refresh_mode::full);
    d.set_color_mode(color_mode::gray4);
    d.write(*g_grey);
    const auto t0 = clk::now();
    d.refresh(refresh_mode::full);
    return ms_since(t0);
}

// One frame: validate the header, receive the chunked payload into the
// framebuffer for its format, check CRC, then display.
bool handle_frame(const epi::header& h) {
    if (h.kind == '1') {
        send_line("err header EPI1 is an E1004 (colour6) frame: this is the EE05 viewer (EPI2 mono, EPI3 grey4)");
        return false;
    }
    if (h.kind != KIND_MONO && h.kind != KIND_GREY4) {
        send_line("err header EPI%c not supported: this is the EE05 viewer (EPI2 mono, EPI3 grey4)", h.kind);
        return false;
    }
    const bool grey = h.kind == KIND_GREY4;
    const uint32_t want = grey ? GREY4_BYTES : MONO_BYTES;
    if (h.width != WIDTH || h.height != HEIGHT || h.length != want) {
        send_line(
            "err header EPI%c %ux%u len %lu (want %ux%u len %lu)",
            h.kind,
            h.width,
            h.height,
            (unsigned long)h.length,
            WIDTH,
            HEIGHT,
            (unsigned long)want
        );
        return false;
    }
    if (g_init_error[0] != '\0') {
        send_line("err panel init failed: %s", g_init_error);
        return false;
    }

    // Payload received into the framebuffer a band at a time. Every value is
    // a valid ink (800 is a multiple of 8: no padding bits), so there is
    // nothing to check beyond the CRC.
    epi::payload_source payload(h);
    const auto written =
        grey ? idfxx::epaper::try_write_inks(*g_grey, payload) : idfxx::epaper::try_write_inks(*g_mono, payload);
    if (!payload.finish()) {
        return false;
    }
    if (!written) {
        send_line("err inks: %s", written.error().message().c_str());
        return false;
    }

    try {
        const auto t0 = clk::now();
        const long long ms = show(grey);
        g_hw->display->sleep();
        // ms: the frame's own refresh; total: everything from wake to sleep
        // (for grey4 frames, including the two scrub refreshes).
        send_line("done %lld total %lld", ms, ms_since(t0));
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
    // USB-Serial-JTAG is the console (sdkconfig.defaults); the transport
    // installs its driver (RX buffer epi::RX_BUFFER) and routes stdio
    // through it.
    static epi::usj_transport link;
    epi::use(link);

    try {
        // Allocate the frames first (internal RAM, 144 KB): reused for every
        // transfer.
        static idfxx::epaper::mono_framebuffer mono(WIDTH, HEIGHT);
        static idfxx::epaper::gray4_framebuffer grey(WIDTH, HEIGHT);
        g_mono = &mono;
        g_grey = &grey;

        static panel_hw hw;
        g_hw = &hw;
        panel_setup(hw);
    } catch (const std::exception& e) {
        std::snprintf(g_init_error, sizeof g_init_error, "%s", e.what());
        send_line("err panel init failed: %s", g_init_error);
    }

    epi::serve(handle_frame);
}
