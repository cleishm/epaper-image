#!/usr/bin/env python3
"""Send a frame to a panel viewer: the reTerminal E1004 (EL133UF1 Spectra 6,
the default) or the XIAO ePaper Display Board EE05 (GDEY075T7 7.5" 800x480,
mono or 4-grey: --panel ee05 --mode mono|grey4).

Usage (see README.md for the wire protocol):

    send.py frame FRAME.bin     a packed frame written by tools/convert
    send.py test                ink (or grey level) bars, in panel-code order

FRAME.bin is convert's OUTPUT for the matching panel: --panel e1004 takes
`convert --panel el133uf1`, --panel ee05 --mode mono `gdey075t7-mono`,
--mode grey4 `gdey075t7-grey4`. Frames are sent as they are.

Common flags: --panel, --mode, --port, --baud, --out PATH (write the payload
that would be sent, instead of sending). Needs only pyserial.
"""

from __future__ import annotations

import argparse
import glob
import os
import struct
import sys
import time
import zlib
from dataclasses import dataclass
from pathlib import Path

# --- E1004: reTerminal E1004, EL133UF1 Spectra 6 ---------------------------------
# Keep in sync with firmware-e1004/main/main.cpp and README.md. The driver's native
# layout is portrait, 1200 wide x 1600 tall; 4 bpp, high nibble = left pixel.
NATIVE_W, NATIVE_H = 1200, 1600
STRIDE = (NATIVE_W + 1) // 2
FRAME_BYTES = STRIDE * NATIVE_H  # 960000
# Panel codes (idfxx_epaper color.hpp); code 4 is unused.
INK_CODES = {"black": 0, "white": 1, "yellow": 2, "red": 3, "blue": 5, "green": 6}

# --- Protocol (keep in sync with firmware-e1004/main/main.cpp and README.md) ----
MAGIC = b"EPI1"
PING = b"EPI?"
CHUNK = 4096
PREFIX = "@epi "
DEFAULT_PORT = "/dev/cu.wchusbserial110"
DEFAULT_BAUD = 921_600

# --- EE05: XIAO ePaper Display Board EE05, 7.5" 800x480 UC8179 ------------------
# Keep in sync with firmware-ee05/main/main.cpp and README.md. The driver's
# native layout is landscape: 800 wide (sources) x 480 tall (gates).
EE05_W, EE05_H = 800, 480
EE05_MODES = ("mono", "grey4")
EE05_LEVELS = {"mono": 2, "grey4": 4}  # level index 0 = black .. n-1 = white
EE05_MAGIC = {"mono": b"EPI2", "grey4": b"EPI3"}
EE05_BYTES = {"mono": EE05_W // 8 * EE05_H, "grey4": EE05_W // 4 * EE05_H}  # 48000, 96000
# Espressif's USB-Serial-JTAG (the XIAO ESP32-S3's native USB port).
ESPRESSIF_USJ_VID_PID = (0x303A, 0x1001)


class SendError(RuntimeError):
    pass


class TransferError(SendError):
    """A failure before the device accepted the payload (safe to retry)."""


# --- Frames ---------------------------------------------------------------------
# convert's frames: rows top to bottom in the native layout, 8 / bpp codes per
# byte, the leftmost in the most significant bits. E1004: 4 bpp ink codes.
# EE05 mono: 1 bpp, the bit is the level (0 black, 1 white). EE05 grey4: 2 bpp,
# the level index (0 black .. 3 white).

# 1 where both nibbles of a byte are E1004 ink codes (the firmware checks too).
_E1004_BAD = bytes(0 if (b >> 4) in INK_CODES.values() and (b & 15) in INK_CODES.values() else 1
                   for b in range(256))


def check_e1004(frame: bytes, label: str = "frame") -> None:
    """Reject a byte holding a nibble that is not an ink code."""
    flags = frame.translate(_E1004_BAD)
    first = flags.find(1)
    if first >= 0:
        y, x = divmod(first, STRIDE)
        raise SendError(f"{label}: byte {first} (x={2 * x}, y={y}) is 0x{frame[first]:02x}, not two ink "
                        f"codes; {flags.count(1)} such byte(s)")


def pack_codes(codes: list[int], bpp: int) -> bytes:
    """One row of codes, packed like convert's frames (MSB = leftmost)."""
    per = 8 // bpp
    out = bytearray()
    for x in range(0, len(codes), per):
        v = 0
        for c in codes[x:x + per]:
            v = (v << bpp) | c
        out.append(v)
    return bytes(out)


def e1004_test_frame() -> bytes:
    """Six bars in panel-code order (black, white, yellow, red, blue, green),
    left to right with the panel turned a quarter turn clockwise (landscape,
    native top edge at the viewer's right), so horizontal bands from the
    native bottom up."""
    codes = sorted(INK_CODES.values())
    bar = NATIVE_H // len(codes)  # 266 landscape columns per bar; the last takes the rest
    rows = []
    for y in range(NATIVE_H):
        c = codes[min((NATIVE_H - 1 - y) // bar, len(codes) - 1)]
        rows.append(bytes([(c << 4) | c]) * STRIDE)
    return b"".join(rows)


def ee05_test_frame(mode: str) -> bytes:
    """One vertical bar per level, darkest first, on the native 800x480."""
    n = EE05_LEVELS[mode]
    bar = EE05_W // n
    row = pack_codes([min(x // bar, n - 1) for x in range(EE05_W)], 1 if mode == "mono" else 2)
    return row * EE05_H


@dataclass(frozen=True)
class Target:
    """The panel (and, for the EE05, the mode) a frame is for."""

    panel: str = "e1004"
    mode: str | None = None

    @property
    def describe(self) -> str:
        return "E1004" if self.panel == "e1004" else f"EE05 {self.mode}"

    @property
    def convert_panel(self) -> str:
        """The tools/convert --panel that makes this target's frames."""
        return "el133uf1" if self.panel == "e1004" else f"gdey075t7-{self.mode}"

    @property
    def frame_bytes(self) -> int:
        return FRAME_BYTES if self.panel == "e1004" else EE05_BYTES[self.mode]

    def to_wire(self, frame: bytes, label: str = "frame") -> bytes:
        """Check a convert frame and return the payload to send."""
        if len(frame) != self.frame_bytes:
            raise SendError(f"{label}: {len(frame)} bytes; an {self.describe} frame is {self.frame_bytes} "
                            f"bytes (convert --panel {self.convert_panel})")
        if self.panel == "e1004":
            check_e1004(frame, label)
        return frame

    def test_frame(self) -> bytes:
        return e1004_test_frame() if self.panel == "e1004" else ee05_test_frame(self.mode)

    def header(self, payload: bytes) -> bytes:
        if self.panel == "e1004":
            return encode_header(payload)
        return encode_header(payload, EE05_W, EE05_H, magic=EE05_MAGIC[self.mode])


# --- Wire protocol --------------------------------------------------------------


def encode_header(payload: bytes, width: int = NATIVE_W, height: int = NATIVE_H, magic: bytes = MAGIC) -> bytes:
    """16-byte frame header. The magic's 4th byte is the frame format:
    EPI1 E1004 colour6, EPI2 EE05 mono, EPI3 EE05 grey4."""
    return struct.pack("<4sHHII", magic, width, height, len(payload), zlib.crc32(payload) & 0xFFFFFFFF)


def parse_line(raw: bytes) -> str | None:
    """Return the protocol message in a raw line ("ready", "ok 4096", ...) or None.

    Text before "@epi " on the same line (e.g. ROM boot output received at the
    wrong baud) is discarded.
    """
    text = raw.decode("utf-8", errors="replace").rstrip("\r\n")
    pos = text.rfind(PREFIX)
    if pos < 0:
        return None
    return text[pos + len(PREFIX):].strip()


class Link:
    def __init__(self, port: str, baud: int, verbose: bool = False):
        import serial  # imported here so composition and tests need no pyserial

        self.verbose = verbose
        self.ser = serial.Serial()
        self.ser.port = port
        self.ser.baudrate = baud
        self.ser.timeout = 0.05
        # Deassert both: on the usual auto-reset circuit this leaves EN and
        # IO0 high. Some boards still reset on open; wait_ready copes.
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.open()
        self.buf = b""

    def close(self):
        self.ser.close()

    def reopen(self, timeout: float = 10.0) -> None:
        """Close and reopen the port, e.g. after a USB device re-enumerated
        (a reset of the EE05's USB-Serial-JTAG may drop the port briefly)."""
        import serial

        self.ser.close()
        self.buf = b""
        deadline = time.monotonic() + timeout
        while True:
            time.sleep(0.5)
            try:
                self.ser.open()
                return
            except serial.SerialException:
                if time.monotonic() >= deadline:
                    raise SendError(f"{self.ser.port} disappeared and did not come back within {timeout:.0f} s")

    def message(self, timeout: float) -> str | None:
        """Next protocol message within timeout seconds, ignoring other lines."""
        deadline = time.monotonic() + timeout
        while True:
            nl = self.buf.find(b"\n")
            while nl >= 0:
                raw, self.buf = self.buf[: nl + 1], self.buf[nl + 1:]
                msg = parse_line(raw)
                if self.verbose:
                    print(f"  < {raw!r}", file=sys.stderr)
                if msg is not None:
                    return msg
                nl = self.buf.find(b"\n")
            if time.monotonic() >= deadline:
                return None
            data = self.ser.read(max(1, self.ser.in_waiting))
            if data:
                self.buf += data
                if len(self.buf) > 65536 and b"\n" not in self.buf:
                    self.buf = self.buf[-4096:]

    def expect(self, want, timeout: float, what: str, skip=("ready",)) -> str:
        """Wait for a message accepted by want(msg); raise on err or timeout."""
        deadline = time.monotonic() + timeout
        while True:
            left = deadline - time.monotonic()
            msg = self.message(max(0.0, left))
            if msg is None:
                raise TransferError(f"timeout waiting for {what}")
            if msg.startswith("err"):
                raise TransferError(f"device: {msg}")
            if want(msg):
                return msg
            if msg in skip:
                continue
            raise TransferError(f"unexpected {msg!r} while waiting for {what}")

    def wait_ready(self, timeout: float = 90.0, ping_every: float = 3.0) -> None:
        """Wait for "@epi ready" (sent at boot, e.g. after a reset on open);
        if none arrives, ping. Covers a device still finishing a refresh."""
        import serial

        deadline = time.monotonic() + timeout
        first = True
        while time.monotonic() < deadline:
            try:
                msg = self.message(min(ping_every if not first else 2.0, max(0.0, deadline - time.monotonic())))
            except serial.SerialException as e:
                print(f"port error ({e}); reopening {self.ser.port}", file=sys.stderr)
                self.reopen()
                first = True
                continue
            first = False
            if msg == "ready":
                # Let any queued duplicate replies (to earlier pings) arrive.
                while (m := self.message(0.2)) is not None:
                    if m.startswith("err"):
                        print(f"device: {m}", file=sys.stderr)
                return
            if msg is not None and msg.startswith("err"):
                print(f"device: {msg}", file=sys.stderr)
                continue
            if msg is None:
                self.ser.write(PING)
                self.ser.flush()
        raise SendError(f"no '@epi ready' from {self.ser.port} within {timeout:.0f} s")


@dataclass
class SendStats:
    transfer_s: float
    chunks: int
    refresh_ms: int
    retries: int
    total_ms: int | None = None  # EE05: wake to sleep, including any scrub refreshes


def parse_done(msg: str) -> tuple[int, int | None]:
    """"done <ms>" (E1004) or "done <ms> total <ms>" (EE05)."""
    parts = msg.split()
    total = int(parts[3]) if len(parts) >= 4 and parts[2] == "total" else None
    return int(parts[1]), total


def send_frame(link: Link, frame: bytes, refresh_timeout: float = 180.0, retries: int = 1,
               header: bytes | None = None) -> SendStats:
    if header is None:
        header = encode_header(frame)
    attempt = 0
    while True:
        try:
            link.wait_ready()
            t0 = time.monotonic()
            link.ser.write(header)
            link.ser.flush()
            link.expect(lambda m: m == "ok 0", 5.0, "header ack")
            chunks = 0
            for off in range(0, len(frame), CHUNK):
                part = frame[off: off + CHUNK]
                link.ser.write(part)
                link.ser.flush()
                end = off + len(part)
                link.expect(lambda m, e=end: m == f"ok {e}", 5.0, f"ack for offset {end}", skip=())
                chunks += 1
            link.expect(lambda m: m == "crc ok", 10.0, "crc check", skip=())
            transfer_s = time.monotonic() - t0
            break
        except TransferError as e:
            if attempt >= retries:
                raise
            attempt += 1
            print(f"transfer failed ({e}); retrying ({attempt}/{retries})", file=sys.stderr)
            time.sleep(0.5)
    print(f"transfer: {transfer_s:.2f} s, {chunks} chunks of {CHUNK} bytes, crc ok; refreshing...")
    try:
        msg = link.expect(lambda m: m.startswith("done "), refresh_timeout, "refresh", skip=())
    except TransferError as e:
        raise SendError(str(e)) from None
    refresh_ms, total_ms = parse_done(msg)
    return SendStats(transfer_s, chunks, refresh_ms, attempt, total_ms)


# --- Port detection -----------------------------------------------------------------


def detect_port(panel: str, comports=None, exists=os.path.exists, globber=glob.glob) -> str:
    """The serial port for the panel when --port is not given.

    E1004 (CH34x bridge): DEFAULT_PORT if present, else the single
    /dev/cu.wchusbserial* device; with none, DEFAULT_PORT (opening it then
    fails as before). EE05 (XIAO ESP32-S3 native USB-Serial-JTAG, macOS
    /dev/cu.usbmodem*): the single port with Espressif's USB-Serial-JTAG
    VID:PID, else the single /dev/cu.usbmodem* (or Linux /dev/ttyACM*)
    device. Several candidates, or none for the EE05, is an error naming
    them. comports/exists/globber are injectable for tests.
    """
    if panel == "e1004":
        if exists(DEFAULT_PORT):
            return DEFAULT_PORT
        found = sorted(globber("/dev/cu.wchusbserial*"))
        if len(found) > 1:
            raise SendError(f"several E1004 candidate ports ({', '.join(found)}); pass --port")
        return found[0] if found else DEFAULT_PORT
    if comports is None:
        try:
            from serial.tools import list_ports
            comports = list_ports.comports()
        except ImportError:
            comports = []
    vid, pid = ESPRESSIF_USJ_VID_PID
    found = sorted({p.device for p in comports if (p.vid, p.pid) == (vid, pid)})
    if not found:
        found = sorted(set(globber("/dev/cu.usbmodem*")) | set(globber("/dev/ttyACM*")))
    if len(found) == 1:
        return found[0]
    if not found:
        raise SendError("no EE05 port found (USB-Serial-JTAG 303a:1001, /dev/cu.usbmodem*); "
                        "is the XIAO plugged in? Or pass --port")
    raise SendError(f"several EE05 candidate ports ({', '.join(found)}); pass --port")


# --- CLI ------------------------------------------------------------------------


def target_of(args) -> Target:
    panel = getattr(args, "panel", "e1004")
    mode = getattr(args, "mode", None)
    if panel == "e1004":
        if mode is not None:
            raise SendError("--mode is for --panel ee05 (the E1004 has one mode)")
        return Target("e1004")
    if mode is None:
        raise SendError("--panel ee05 needs --mode mono or --mode grey4")
    return Target("ee05", mode)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--panel", choices=("e1004", "ee05"), default="e1004",
                    help="target panel viewer (default e1004)")
    ap.add_argument("--mode", choices=EE05_MODES, help="EE05 only (required): mono or grey4")
    ap.add_argument("--port", default=None,
                    help=f"serial port (default: autodetect; E1004 {DEFAULT_PORT}, EE05 /dev/cu.usbmodem*)")
    ap.add_argument("--baud", type=int, default=DEFAULT_BAUD, help="E1004 UART baud (ignored by the EE05's USB port)")
    ap.add_argument("--out", type=Path, help="write the payload that would be sent; don't send")
    ap.add_argument("--retries", type=int, default=1, help="resend after a transfer error (default 1)")
    ap.add_argument("--refresh-timeout", type=float, default=180.0, help="seconds to wait for refresh")
    ap.add_argument("-v", "--verbose", action="store_true", help="show every line from the device")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("frame", help="send a packed frame from tools/convert")
    p.add_argument("frame")
    sub.add_parser("test", help="bars: E1004 the six inks, EE05 the mode's grey levels")
    args = ap.parse_args(argv)

    try:
        target = target_of(args)
        if args.cmd == "frame":
            try:
                data = Path(args.frame).read_bytes()
            except OSError as e:
                raise SendError(f"{args.frame}: {e.strerror}") from None
            frame = target.to_wire(data, args.frame)
        else:
            frame = target.to_wire(target.test_frame(), "test frame")
        if args.out:
            args.out.write_bytes(frame)
            print(f"wrote {args.out} ({len(frame)} bytes, {target.describe} payload)")
            return 0
        port = args.port or detect_port(target.panel)
        link = Link(port, args.baud, args.verbose)
        try:
            stats = send_frame(link, frame, args.refresh_timeout, args.retries, header=target.header(frame))
        finally:
            link.close()
        total = f" (total {stats.total_ms} ms)" if stats.total_ms is not None else ""
        print(
            f"done: transfer {stats.transfer_s:.2f} s ({stats.chunks} chunks), "
            f"refresh {stats.refresh_ms} ms{total}, retries {stats.retries}"
        )
        return 0
    except SendError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
