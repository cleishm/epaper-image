// Panel viewer wire protocol, shared by the E1004 and EE05 firmwares
// (../../../../firmware-e1004, ../../../../firmware-ee05). Specified in
// ../../../../README.md; the host side is ../../../../send.py.
//
// Every device->host protocol message is a single line starting "@epi ".
// Anything else on the line (ROM boot output, panics, stray logs) is noise
// the host ignores.
//
// The firmware picks a transport (UART through a USB bridge, or the
// ESP32-S3's native USB-Serial-JTAG), installs it with epi::use(), and hands
// epi::serve() a per-panel frame handler. serve() scans the input for a
// 4-byte magic, answers pings, reads the 12 header bytes that follow a
// frame magic "EPI<digit>" and calls the handler; the handler validates the
// header, receives the payload with epi::receive_payload() and shows it.

#pragma once

#include <cstddef>
#include <cstdint>
#include <driver/uart.h>
#include <freertos/FreeRTOS.h>
#include <span>

namespace epi {

// --- Protocol constants (keep in sync with send.py and README.md) ---
inline constexpr size_t CHUNK = 4096;
inline constexpr size_t RX_BUFFER = 16384;                        // >= CHUNK + margin
inline constexpr TickType_t HEADER_TIMEOUT = pdMS_TO_TICKS(1000); // rest of header after magic
inline constexpr TickType_t CHUNK_TIMEOUT = pdMS_TO_TICKS(3000);  // one chunk
inline constexpr TickType_t DRAIN_IDLE = pdMS_TO_TICKS(250);      // line quiet before resync

// Header after the 4-byte magic "EPI<kind>": u16 width, u16 height,
// u32 length, u32 CRC32 of the payload, all little-endian.
inline constexpr size_t HEADER_REST = 12;

struct header {
    char kind; // the magic's 4th byte: '1' (E1004 colour6), '2' (mono), '3' (grey4), ...
    uint16_t width;
    uint16_t height;
    uint32_t length;
    uint32_t crc;
};

// A byte stream to the host. read() returns the number of bytes read (up to
// n, at least 1) or 0 if none arrived within timeout.
class transport {
public:
    virtual ~transport() = default;
    virtual int read(uint8_t* dst, size_t n, TickType_t timeout) = 0;
    // Writes one complete line in a single driver call, so it is never split
    // by other output; waits (bounded) for it to leave.
    virtual void write_line(const char* data, size_t n) = 0;
    virtual void flush_input() = 0;
};

// UART (the console UART, through a USB-UART bridge). Takes the UART over
// with the driver and routes stdio through it, so stray log lines are queued
// rather than interleaved mid-line.
class uart_transport final : public transport {
public:
    uart_transport(uart_port_t port, int baud, int tx_gpio, int rx_gpio);
    int read(uint8_t* dst, size_t n, TickType_t timeout) override;
    void write_line(const char* data, size_t n) override;
    void flush_input() override;

private:
    uart_port_t _port;
};

// The ESP32-S3's native USB-Serial-JTAG CDC port. Requires
// CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG (the console on this port, no
// secondary console): installs the driver and routes stdio through it, so
// stray log lines go through the same TX ring buffer as protocol lines.
// The driver's RX ISR drops bytes when its ring buffer is full; the
// protocol's per-chunk acks keep at most one CHUNK in flight, well under
// RX_BUFFER. Writes time out (rather than block) when no host is reading.
class usj_transport final : public transport {
public:
    usj_transport();
    int read(uint8_t* dst, size_t n, TickType_t timeout) override;
    void write_line(const char* data, size_t n) override;
    void flush_input() override;
};

// Selects the transport for everything below. Call once, before serve().
void use(transport& t);

void send_line(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// Reads exactly n bytes, all within timeout of the call. False on timeout.
bool read_exact(uint8_t* dst, size_t n, TickType_t timeout);

// Discards input until the line has been quiet for DRAIN_IDLE, so the rest
// of an aborted transfer is not mistaken for a new header.
void drain_input();

// Receives a payload of h.length bytes in CHUNK-byte chunks (the last one is
// the remainder) straight into dst (consecutive segments, total size >=
// h.length), replying "@epi ok 0" first and "@epi ok <offset of next byte>"
// after each chunk, nothing else meanwhile. Then checks the CRC32 and
// replies "@epi crc ok". Returns false after replying "@epi err ..." on a
// chunk timeout or CRC mismatch.
bool receive_payload(std::span<const std::span<uint8_t>> dst, const header& h);

// The per-panel frame handler: validate the header, receive the payload,
// show the frame. Returns after replying "@epi done ..." (true) or
// "@epi err ..." (false); serve() then drains input on false.
using frame_handler = bool (*)(const header& h);

// Idle loop, never returns: sends "@epi ready", then scans the byte stream
// for a 4-byte magic, never assuming alignment. "EPI?" is a ping answered
// with "@epi ready"; "EPI<digit>" starts a frame header, handed to
// on_frame. "@epi ready" is also sent on every return to idle.
[[noreturn]] void serve(frame_handler on_frame);

} // namespace epi
