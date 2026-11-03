"""Host-side tests for send.py: E1004 frames, the wire protocol (no device needed)."""

import struct
import zlib

import pytest

import send

# Driver codes from idfxx_epaper's color.hpp.
DRIVER_CODES = {"black": 0, "white": 1, "yellow": 2, "red": 3, "blue": 5, "green": 6}


def test_ink_codes_match_driver():
    assert send.INK_CODES == DRIVER_CODES


# --- E1004 frames -----------------------------------------------------------------


def test_frame_sent_unchanged():
    codes = sorted(DRIVER_CODES.values())
    frame = bytes((codes[i % 6] << 4) | codes[(i // 6) % 6] for i in range(send.FRAME_BYTES))
    assert send.Target().to_wire(frame) is frame


def test_every_ink_pair_is_valid_and_others_rejected():
    codes = DRIVER_CODES.values()
    for b in range(256):
        frame = bytes([b]) + bytes([0x11]) * (send.FRAME_BYTES - 1)
        if (b >> 4) in codes and (b & 15) in codes:
            send.check_e1004(frame)
        else:
            with pytest.raises(send.SendError):
                send.check_e1004(frame)


def test_invalid_nibble_reported_with_position_and_count():
    frame = bytearray([0x11]) * send.FRAME_BYTES
    frame[3 * send.STRIDE + 7] = 0x14  # right pixel code 4 (unused)
    frame[9 * send.STRIDE] = 0x71
    with pytest.raises(send.SendError) as e:
        send.Target().to_wire(bytes(frame), "f.bin")
    msg = str(e.value)
    assert "x=14, y=3" in msg and "0x14" in msg and "2 such byte" in msg


def test_wrong_length_names_the_convert_panel():
    with pytest.raises(send.SendError, match=r"48000 bytes; an E1004 frame is 960000 bytes \(convert --panel el133uf1\)"):
        send.Target().to_wire(bytes(48000))


def test_magic_cannot_occur_in_valid_payload():
    # 'E' (0x45) has nibble 4, which is never a valid code.
    with pytest.raises(send.SendError):
        send.check_e1004(b"EPI1" + bytes([0x11]) * (send.FRAME_BYTES - 4))


def test_test_frame_is_bars_in_code_order():
    f = send.e1004_test_frame()
    assert len(f) == send.FRAME_BYTES
    send.check_e1004(f)
    # Viewed turned a quarter turn clockwise (convert --rotate): landscape
    # (x, y) is native (y, 1599 - x), so landscape column x is native row 1599 - x.
    bar = send.NATIVE_H // 6
    row_codes = [f[(send.NATIVE_H - 1 - (k * bar + bar // 2)) * send.STRIDE] & 15 for k in range(6)]
    assert row_codes == [0, 1, 2, 3, 5, 6]
    assert all(len(set(f[y * send.STRIDE:(y + 1) * send.STRIDE])) == 1 for y in range(0, send.NATIVE_H, 97))


# --- header / CRC -----------------------------------------------------------------


def test_crc32_is_standard():
    assert zlib.crc32(b"123456789") == 0xCBF43926


def test_header_encoding():
    payload = bytes(range(256)) * 3750
    h = send.encode_header(payload)
    assert len(h) == 16 and h[:4] == b"EPI1"
    magic, w, hh, n, crc = struct.unpack("<4sHHII", h)
    assert (w, hh, n) == (1200, 1600, 960000)
    assert crc == zlib.crc32(payload)
    assert h[4:6] == (1200).to_bytes(2, "little")


def test_parse_line():
    assert send.parse_line(b"@epi ready\n") == "ready"
    assert send.parse_line(b"\xff\x00garbage@epi ok 4096\r\n") == "ok 4096"
    assert send.parse_line(b"I (123) boot: something\n") is None


# --- transfer, against a fake device ----------------------------------------------


class FakeDevice:
    """Speaks the viewer protocol: "ready" at start, "ok <n>" per chunk,
    "crc ok", "done <ms>". fail_headers: reply "err ..." to that many headers
    first (then announce "ready" again, as the firmware does)."""

    def __init__(self, fail_headers=0):
        self.port = "/dev/fake"
        self.out = b"@epi ready\n"
        self.rx = b""
        self.want = None
        self.fail_headers = fail_headers
        self.headers = 0
        self.payloads = []

    @property
    def in_waiting(self):
        return len(self.out)

    def read(self, n):
        data, self.out = self.out[:n], self.out[n:]
        return data

    def flush(self):
        pass

    def write(self, data):
        self.rx += data
        if self.want is None:
            if len(self.rx) < 16:
                return
            magic, w, h, n, crc = struct.unpack("<4sHHII", self.rx[:16])
            self.rx = self.rx[16:]
            self.headers += 1
            if self.fail_headers:
                self.fail_headers -= 1
                self.out += b"@epi err busy\n@epi ready\n"
                return
            self.want, self.crc = n, crc
            self.out += b"I (5) noise\n@epi ok 0\n"
            return
        self.out += f"@epi ok {len(self.rx)}\n".encode()
        if len(self.rx) == self.want:
            assert zlib.crc32(self.rx) == self.crc
            self.payloads.append(self.rx)
            self.out += b"@epi crc ok\n@epi done 1234 total 2345\n"


def fake_link(dev):
    link = send.Link.__new__(send.Link)  # no port to open
    link.ser, link.buf, link.verbose = dev, b"", False
    return link


def test_send_frame_chunks_acks_and_crc():
    payload = bytes(range(256)) * 40 + b"\x11" * 100  # not a multiple of CHUNK
    dev = FakeDevice()
    stats = send.send_frame(fake_link(dev), payload, header=send.encode_header(payload))
    assert dev.payloads == [payload]
    assert stats.chunks == -(-len(payload) // send.CHUNK)
    assert (stats.refresh_ms, stats.total_ms, stats.retries) == (1234, 2345, 0)


def test_send_frame_retries_after_device_error():
    payload = b"\x11" * 5000
    dev = FakeDevice(fail_headers=1)
    stats = send.send_frame(fake_link(dev), payload, retries=1, header=send.encode_header(payload))
    assert stats.retries == 1 and dev.headers == 2 and dev.payloads == [payload]


def test_send_frame_gives_up_after_retries():
    dev = FakeDevice(fail_headers=2)
    with pytest.raises(send.TransferError, match="device: err busy"):
        send.send_frame(fake_link(dev), b"\x11" * 10, retries=1, header=send.encode_header(b"\x11" * 10))


# --- CLI --------------------------------------------------------------------------


def test_cli_out_writes_the_payload(tmp_path):
    src = tmp_path / "f.bin"
    src.write_bytes(send.e1004_test_frame())
    out = tmp_path / "wire.bin"
    assert send.main(["--out", str(out), "frame", str(src)]) == 0
    assert out.read_bytes() == src.read_bytes()


def test_cli_missing_file(tmp_path, capsys):
    assert send.main(["--out", str(tmp_path / "x"), "frame", str(tmp_path / "nope.bin")]) == 1
    assert "nope.bin" in capsys.readouterr().err
