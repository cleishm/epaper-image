// Panel viewer wire protocol; see include/epi/protocol.hpp.

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <driver/uart.h>
#include <driver/uart_vfs.h>
#include <driver/usb_serial_jtag.h>
#include <driver/usb_serial_jtag_vfs.h>
#include <epi/protocol.hpp>
#include <esp_err.h>
#include <esp_rom_crc.h>
#include <freertos/task.h>

namespace epi {

namespace {

transport* g_transport = nullptr;

uint16_t le16(const uint8_t* p) {
    return uint16_t(p[0] | p[1] << 8);
}
uint32_t le32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

// Reads n bytes, giving up once timeout ticks have passed since start.
bool read_by(uint8_t* dst, size_t n, TickType_t start, TickType_t timeout) {
    size_t got = 0;
    while (got < n) {
        const TickType_t elapsed = xTaskGetTickCount() - start;
        if (elapsed >= timeout) {
            return false;
        }
        const int r = g_transport->read(dst + got, n - got, timeout - elapsed);
        if (r <= 0) {
            return false;
        }
        got += size_t(r);
    }
    return true;
}

} // namespace

// --- UART ---

uart_transport::uart_transport(uart_port_t port, int baud, int tx_gpio, int rx_gpio)
    : _port(port) {
    uart_config_t cfg{};
    cfg.baud_rate = baud;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;
    ESP_ERROR_CHECK(uart_driver_install(port, RX_BUFFER, 0, 0, nullptr, 0));
    ESP_ERROR_CHECK(uart_param_config(port, &cfg));
    // Route the console pins explicitly (defensive; not shown to be required).
    ESP_ERROR_CHECK(uart_set_pin(port, tx_gpio, rx_gpio, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    uart_vfs_dev_use_driver(port);
}

int uart_transport::read(uint8_t* dst, size_t n, TickType_t timeout) {
    const int r = uart_read_bytes(_port, dst, n, timeout);
    return r > 0 ? r : 0;
}

void uart_transport::write_line(const char* data, size_t n) {
    uart_write_bytes(_port, data, n);
    uart_wait_tx_done(_port, pdMS_TO_TICKS(100));
}

void uart_transport::flush_input() {
    uart_flush_input(_port);
}

// --- USB-Serial-JTAG ---

usj_transport::usj_transport() {
    usb_serial_jtag_driver_config_t cfg{};
    cfg.rx_buffer_size = RX_BUFFER;
    cfg.tx_buffer_size = 2048;
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
    usb_serial_jtag_vfs_use_driver();
}

int usj_transport::read(uint8_t* dst, size_t n, TickType_t timeout) {
    const int r = usb_serial_jtag_read_bytes(dst, n, timeout);
    return r > 0 ? r : 0;
}

void usj_transport::write_line(const char* data, size_t n) {
    // Bounded waits: with no host reading, the TX ring fills and these time
    // out (the line is dropped) instead of hanging the device.
    usb_serial_jtag_write_bytes(data, n, pdMS_TO_TICKS(100));
    (void)usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(100));
}

void usj_transport::flush_input() {
    uint8_t junk[64];
    while (usb_serial_jtag_read_bytes(junk, sizeof junk, 0) > 0) {
    }
}

// --- Protocol ---

void use(transport& t) {
    g_transport = &t;
}

// One write per line ("\n" only), so a line is never split.
void send_line(const char* fmt, ...) {
    char buf[160];
    int n = std::snprintf(buf, sizeof buf, "@epi ");
    va_list ap;
    va_start(ap, fmt);
    int m = std::vsnprintf(buf + n, sizeof buf - n - 1, fmt, ap);
    va_end(ap);
    if (m < 0) {
        m = 0;
    }
    n = std::min<int>(n + m, sizeof buf - 2);
    buf[n++] = '\n';
    g_transport->write_line(buf, size_t(n));
}

bool read_exact(uint8_t* dst, size_t n, TickType_t timeout) {
    return read_by(dst, n, xTaskGetTickCount(), timeout);
}

void drain_input() {
    uint8_t junk[256];
    while (g_transport->read(junk, sizeof junk, DRAIN_IDLE) > 0) {
    }
    g_transport->flush_input();
}

payload_source::payload_source(const header& h)
    : _length(h.length)
    , _want_crc(h.crc)
    , _chunk_end(std::min<uint32_t>(CHUNK, h.length)) {
    send_line("ok 0");
    _chunk_start = xTaskGetTickCount();
}

idfxx::result<size_t> payload_source::try_read(std::span<uint8_t> buf) {
    if (_failed) {
        return idfxx::error(idfxx::errc::timeout);
    }
    const size_t n = std::min<size_t>(buf.size(), _chunk_end - _offset);
    if (n == 0) {
        return 0;
    }
    const TickType_t elapsed = xTaskGetTickCount() - _chunk_start;
    const int r = elapsed < CHUNK_TIMEOUT ? g_transport->read(buf.data(), n, CHUNK_TIMEOUT - elapsed) : 0;
    if (r <= 0) {
        send_line("err timeout at offset %lu", (unsigned long)(_offset / CHUNK * CHUNK));
        _failed = true;
        return idfxx::error(idfxx::errc::timeout);
    }
    _crc = esp_rom_crc32_le(_crc, buf.data(), size_t(r));
    _offset += uint32_t(r);
    if (_offset == _chunk_end) {
        send_line("ok %lu", (unsigned long)_offset);
        _chunk_end = std::min<uint32_t>(_offset + CHUNK, _length);
        _chunk_start = xTaskGetTickCount();
    }
    return size_t(r);
}

bool payload_source::finish() {
    uint8_t junk[256];
    while (_offset < _length) {
        if (!try_read(junk)) {
            return false;
        }
    }
    if (_crc != _want_crc) {
        send_line("err crc mismatch: got %08lx want %08lx", (unsigned long)_crc, (unsigned long)_want_crc);
        return false;
    }
    send_line("crc ok");
    return true;
}

void serve(frame_handler on_frame) {
    send_line("ready");
    uint8_t win[4] = {};
    for (;;) {
        uint8_t b;
        if (g_transport->read(&b, 1, portMAX_DELAY) != 1) {
            continue;
        }
        std::memmove(win, win + 1, 3);
        win[3] = b;
        if (win[0] != 'E' || win[1] != 'P' || win[2] != 'I') {
            continue;
        }
        const uint8_t kind = win[3];
        if (kind != '?' && (kind < '1' || kind > '9')) {
            continue; // not a magic; keep the window, as a real one may overlap it
        }
        std::memset(win, 0, sizeof win);
        if (kind == '?') {
            send_line("ready");
            continue;
        }
        uint8_t rest[HEADER_REST];
        bool ok = false;
        if (!read_exact(rest, sizeof rest, HEADER_TIMEOUT)) {
            send_line("err timeout in header");
        } else {
            const header h{char(kind), le16(rest), le16(rest + 2), le32(rest + 4), le32(rest + 8)};
            ok = on_frame(h);
        }
        if (!ok) {
            drain_input();
        }
        send_line("ready");
    }
}

} // namespace epi
