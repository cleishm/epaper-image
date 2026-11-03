"""Host-side tests for send.py's EE05 (GDEY075T7, mono and grey4) support."""

import random
import re
import struct
import zlib
from pathlib import Path
from types import SimpleNamespace

import pytest

import send

FIRMWARE = Path(__file__).resolve().parent.parent / "firmware-ee05" / "main" / "main.cpp"
H, W = send.EE05_H, send.EE05_W


def random_levels(mode, seed=0):
    rng = random.Random(seed)
    n = send.EE05_LEVELS[mode]
    return [[rng.randrange(n) for _ in range(W)] for _ in range(H)]


def convert_frame(levels, mode):
    """What `convert --panel gdey075t7-<mode>` writes: the level index is the
    ink code, packed MSB first (1 bpp mono, 2 bpp grey4)."""
    bpp = 1 if mode == "mono" else 2
    return b"".join(send.pack_codes(row, bpp) for row in levels)


# --- Reference framebuffers, transcribed from the idfxx headers -------------------
# idfxx_epaper/include/idfxx/epaper/mono_framebuffer.hpp: starts all 0xFF
# (white); byte y * stride + x / 8, bit 7 - x % 8; set_pixel(ink=true) clears
# the bit (a set bit is white paper).
# gray4_framebuffer.hpp + color.hpp: gray4 white=0, light=1, dark=2, black=3;
# plane 0 holds bit 0, plane 1 bit 1, same bit layout; starts all 0 (white).


def ref_mono(ink) -> bytes:
    stride = (W + 7) // 8
    data = bytearray([0xFF]) * (stride * H)
    for y in range(H):
        for x in range(W):
            if ink[y][x]:
                data[y * stride + x // 8] &= ~(1 << (7 - x % 8)) & 0xFF
    return bytes(data)


def ref_grey4(grey) -> bytes:
    stride = (W + 7) // 8
    size = stride * H
    data = bytearray(2 * size)
    for y in range(H):
        for x in range(W):
            g = grey[y][x]
            mask = 1 << (7 - x % 8)
            if g & 1:
                data[y * stride + x // 8] |= mask
            if g & 2:
                data[size + y * stride + x // 8] |= mask
    return bytes(data)


# --- Frames -> payload -----------------------------------------------------------------


def test_mono_frame_is_the_payload():
    lv = random_levels("mono", seed=2)
    frame = convert_frame(lv, "mono")
    assert send.Target("ee05", "mono").to_wire(frame) == frame
    assert frame == ref_mono([[v == 0 for v in row] for row in lv])


def test_grey4_plane_bit_layout():
    white = bytes([0xFF]) * 96000  # level 3 everywhere: gray4 0 in both planes
    assert send.grey4_to_planes(white) == b"\x00" * 96000
    frame = bytearray(white)
    frame[0], frame[1] = 0x1B, 0xFF  # row 0 levels 0, 1, 2, 3, 3, 3, 3, 3 -> gray4 3, 2, 1, 0, 0, ...
    p = send.grey4_to_planes(bytes(frame))
    assert len(p) == 96000
    assert p[0] == 0xA0  # plane 0, bit 0: 1, 0, 1, 0, ...
    assert p[48000] == 0xC0  # plane 1, bit 1: 1, 1, 0, 0, ...
    assert send.grey4_to_planes(bytes(96000)) == b"\xff" * 96000  # all black


def test_grey4_payload_matches_idfxx_reference():
    lv = random_levels("grey4", seed=3)
    got = send.Target("ee05", "grey4").to_wire(convert_frame(lv, "grey4"))
    assert got == ref_grey4([[3 - v for v in row] for row in lv])


@pytest.mark.parametrize("mode,size", [("mono", 48000), ("grey4", 96000)])
def test_wrong_length_names_the_convert_panel(mode, size):
    with pytest.raises(send.SendError, match=rf"an EE05 {mode} frame is {size} bytes \(convert --panel gdey075t7-{mode}\)"):
        send.Target("ee05", mode).to_wire(bytes(960000))


# --- Header ------------------------------------------------------------------------------


@pytest.mark.parametrize("mode,magic,length", [("mono", b"EPI2", 48000), ("grey4", b"EPI3", 96000)])
def test_header_carries_mode(mode, magic, length):
    t = send.Target("ee05", mode)
    payload = t.to_wire(convert_frame(random_levels(mode, seed=5), mode))
    h = t.header(payload)
    assert len(h) == 16
    m, w, hh, n, crc = struct.unpack("<4sHHII", h)
    assert (m, w, hh, n) == (magic, 800, 480, length)
    assert crc == zlib.crc32(payload)


def test_e1004_header_unchanged_by_target():
    payload = bytes(960000)
    assert send.Target().header(payload) == send.encode_header(payload)
    assert send.Target().header(payload)[:4] == b"EPI1"


def test_firmware_constants_match():
    src = FIRMWARE.read_text()
    assert re.search(r"WIDTH = 800;", src) and re.search(r"HEIGHT = 480;", src)
    assert "KIND_MONO = '2'" in src and "KIND_GREY4 = '3'" in src
    assert send.EE05_MAGIC == {"mono": b"EPI2", "grey4": b"EPI3"}


def test_parse_done():
    assert send.parse_done("done 31234") == (31234, None)
    assert send.parse_done("done 4100 total 12900") == (4100, 12900)


# --- Test pattern ------------------------------------------------------------------------


@pytest.mark.parametrize("mode", send.EE05_MODES)
def test_level_test_pattern(mode):
    f = send.ee05_test_frame(mode)
    assert len(f) == send.EE05_BYTES[mode]
    n = send.EE05_LEVELS[mode]
    bpp = 1 if mode == "mono" else 2
    stride = W * bpp // 8

    def level(y, x):
        b = f[y * stride + x * bpp // 8]
        return (b >> (8 - bpp - (x * bpp) % 8)) & (n - 1)

    bar = W // n
    assert [level(H - 1, k * bar + bar // 2) for k in range(n)] == list(range(n))
    assert f[:stride] * H == f  # every row the same


# --- CLI ------------------------------------------------------------------------------------


def test_cli_out_grey4_round_trip(tmp_path):
    lv = random_levels("grey4", seed=30)
    src = tmp_path / "f.bin"
    src.write_bytes(convert_frame(lv, "grey4"))
    out = tmp_path / "wire.bin"
    assert send.main(["--panel", "ee05", "--mode", "grey4", "--out", str(out), "frame", str(src)]) == 0
    assert out.read_bytes() == send.grey4_to_planes(src.read_bytes())


def test_cli_mode_rules(tmp_path, capsys):
    out = str(tmp_path / "x.bin")
    assert send.main(["--panel", "ee05", "--out", out, "test"]) == 1
    assert "needs --mode" in capsys.readouterr().err
    assert send.main(["--mode", "mono", "--out", out, "test"]) == 1
    assert "--mode is for --panel ee05" in capsys.readouterr().err


def test_cli_rejects_grey4_frame_in_mono_mode(tmp_path, capsys):
    src = tmp_path / "g.bin"
    src.write_bytes(convert_frame(random_levels("grey4", seed=31), "grey4"))
    assert send.main(["--panel", "ee05", "--mode", "mono", "--out", str(tmp_path / "x"), "frame", str(src)]) == 1
    assert "convert --panel gdey075t7-mono" in capsys.readouterr().err


# --- Port detection ---------------------------------------------------------------------------


def port(device, vid=None, pid=None):
    return SimpleNamespace(device=device, vid=vid, pid=pid)


def globber_for(paths):
    import fnmatch

    return lambda pattern: [p for p in paths if fnmatch.fnmatch(p, pattern)]


def test_detect_ee05_by_vid_pid():
    ports = [port("/dev/cu.wchusbserial110", 0x1A86, 0x55D4), port("/dev/cu.usbmodem1101", 0x303A, 0x1001)]
    assert send.detect_port("ee05", comports=ports, globber=globber_for([])) == "/dev/cu.usbmodem1101"


def test_detect_ee05_glob_fallback_and_errors():
    g = globber_for(["/dev/cu.usbmodem2101", "/dev/cu.wchusbserial110"])
    assert send.detect_port("ee05", comports=[], globber=g) == "/dev/cu.usbmodem2101"
    with pytest.raises(send.SendError, match="no EE05 port"):
        send.detect_port("ee05", comports=[], globber=globber_for([]))
    two = [port("/dev/cu.usbmodem1101", 0x303A, 0x1001), port("/dev/cu.usbmodem2101", 0x303A, 0x1001)]
    with pytest.raises(send.SendError, match="several"):
        send.detect_port("ee05", comports=two, globber=globber_for([]))


def test_detect_e1004_keeps_default():
    g = globber_for(["/dev/cu.wchusbserial110", "/dev/cu.usbmodem1101"])
    assert send.detect_port("e1004", exists=lambda p: True, globber=g) == send.DEFAULT_PORT
    other = globber_for(["/dev/cu.wchusbserial2420", "/dev/cu.usbmodem1101"])
    assert send.detect_port("e1004", exists=lambda p: False, globber=other) == "/dev/cu.wchusbserial2420"
    assert send.detect_port("e1004", exists=lambda p: False, globber=globber_for([])) == send.DEFAULT_PORT
