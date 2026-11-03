# Panel viewer

Shows renderings on a real panel, pixel-exact, so you can compare them side by
side. It has small ESP-IDF firmwares that receive one packed frame over USB
serial and show it, and a sender that runs on the host, `send.py`. There are two
panels:

- **E1004** (the default): Seeed reTerminal E1004 (ESP32-S3, 13.3" 1200x1600
  Spectra 6 EL133UF1 panel). Most of this README is about this one.
- **EE05**: Seeed Studio XIAO ePaper Display Board EE05 (XIAO ESP32-S3, 7.5"
  800x480 monochrome UC8179 panel, GooDisplay GDEY075T7), in mono (2-level) or
  4-grey mode. See [EE05](#ee05) below.

Frames come from the library's command-line tool, `tools/convert` (built with
the library; see the top-level README). It writes the packed frame that the
panel is sent. `send.py` checks the frame and sends it. Nothing is resampled or
colour-converted on the way. Only EE05 grey4 frames change, repacked into the
firmware's bit planes.

```
firmware-e1004/        E1004 ESP-IDF project
firmware-ee05/         EE05 ESP-IDF project
components/epi_protocol/  wire protocol shared by both firmwares (UART and
                       USB-Serial-JTAG transports, chunked receive, CRC)
cmake/idfxx.cmake      IDFXX_PATH support, shared by both projects
send.py                host sender (--panel e1004|ee05); stdlib + pyserial
tests/                 pytest, no device needed
```

## Prerequisites

- A reTerminal E1004 on USB (CH34x USB-UART bridge to the ESP32-S3's UART0;
  on macOS it appears as `/dev/cu.wchusbserial*`).
- ESP-IDF 5.5 or later (built and tested with v6.1). The build gets
  [idfxx](https://github.com/cleishm/idfxx) from the component registry, so the
  first build needs network access (or a component-manager cache hit). To use a
  local checkout instead, set `IDFXX_PATH` to it and run `idf.py reconfigure`.
- Python 3 with pyserial, in this tool's venv (`tools/panel/.venv`):

  ```sh
  python3 -m venv tools/panel/.venv && tools/panel/.venv/bin/pip install -r tools/panel/requirements.txt
  ```

- The `convert` tool. Running `cmake -B build && cmake --build build` at the
  repository root builds it as `build/tools/convert/convert`.

## Build and flash

```sh
source ~/.espressif/tools/activate_idf_v6.1.sh   # or your own ESP-IDF activation
cd tools/panel/firmware-e1004
idf.py set-target esp32s3                        # first time only
idf.py build
idf.py -p /dev/cu.wchusbserial110 flash
```

`dependencies.lock` is not committed, because with `IDFXX_PATH` set it records
paths specific to your machine.

## Send

Run from the repository root:

```sh
PY=tools/panel/.venv/bin/python
CONVERT=build/tools/convert/convert
$CONVERT --preset Soft --preview preview.png photo.jpg frame.bin   # portrait
$CONVERT --rotate photo.jpg frame.bin                              # landscape
$PY tools/panel/send.py frame frame.bin
$PY tools/panel/send.py test                       # six ink bars
$PY tools/panel/send.py --out wire.bin frame frame.bin   # check only, don't send

# blind comparison: four renderings on one screen, cells A-D shuffled by the seed
$CONVERT --compare --seed 7 --key key.json --rotate photo.jpg cmp.bin \
    preset=Default preset=Soft preset=Deep tone=0.3,vibrance=1.2
$PY tools/panel/send.py frame cmp.bin
```

- `frame FRAME.bin`: sends what `convert --panel el133uf1` writes, which is
  960000 bytes, the payload below. Before sending, every byte is checked for two
  ink codes. A violation reports the count and the first bad position.
- `convert --compare`: a 2x2 grid of cells half the panel each (A top-left, B
  top-right, C bottom-left, D bottom-right). Each cell is converted on its own
  from the image cover-cropped to the cell. The cell's letter is in a small
  white box at its outer corner. `key.json` says which config is in which cell.
- `test`: six bars in panel-code order (black, white, yellow, red, blue,
  green), left to right when viewed in landscape (below), unlabelled.
- Flags go before the command: `--port` (default: `/dev/cu.wchusbserial110`
  if present, otherwise the only `/dev/cu.wchusbserial*` device),
  `--baud` (default 921600), `--out PATH` (write the payload that would be
  sent and do not send), `--retries N` (resend after a transfer error,
  default 1), `--refresh-timeout S` (default 180), `-v` (show every line
  received).

`send.py` prints the transfer time, the chunk count and the refresh time
measured by the device. A send takes about 45 s: ~14 s transfer, ~30 s refresh
(longer when the panel is cold).

**Orientation.** `convert --rotate` renders in landscape (1600x1200), then
rotates the frame to the native portrait layout. It rotates the same way as
`idfxx::gfx::rotated` with `rotation::cw90` (the e1004_demo's landscape mode):
landscape pixel (x, y) goes to native (y, 1599 - x). View the panel turned a
quarter turn clockwise from native, that is, with its native top edge on your
right. Without `--rotate`, view it with the native top edge up.

## Wire protocol

The E1004 uses UART0 of the ESP32-S3, through the board's CH34x bridge, at
**921600 baud, 8N1, no flow control**. (The EE05 uses its native USB port
instead. See [EE05](#ee05).) At 2 Mbaud, bytes sent from host to device never
arrived, although device to host worked. 921600 is reliable. The baud is `BAUD`
in `firmware-e1004/main/main.cpp` and `DEFAULT_BAUD` in `send.py`. The console
shares the port: the ROM and bootloader print at 115200, the app switches to
921600 in `app_main`, and logging is limited to warnings and errors.

**Device to host.** Every protocol message is one text line that ends in `\n`
and starts with `@epi `. The host ignores every other line (boot ROM output,
logs, panics). Within a line, it also ignores anything before the last `@epi `,
because junk received at the wrong baud can come before it.

| Message | Meaning |
| --- | --- |
| `@epi ready` | Idle, waiting for a header. Sent at boot, on every return to idle, and in reply to a ping. |
| `@epi ok <n>` | Header accepted (`n` = 0) or payload received up to byte offset `n`; send the next chunk. |
| `@epi crc ok` | All bytes received and the CRC32 matches. |
| `@epi done <ms>` | Frame shown; `ms` is the panel refresh time. |
| `@epi err <text>` | Failure; the device drains input until the line is quiet for 250 ms, then sends `@epi ready`. |

**Host to device.** The device scans the byte stream for a 4-byte magic and
never assumes alignment:

- `EPI?`: ping, answered with `@epi ready`.
- `EPI1`: frame header, followed by 12 more bytes, little-endian:
  `u16 width` (1200), `u16 height` (1600), `u32 length` (960000),
  `u32 CRC32` of the payload (the zlib/IEEE CRC: `zlib.crc32`).
  The header is 16 bytes in total:
  `struct.pack("<4sHHII", b"EPI1", w, h, len, crc)`.
- The magic's 4th byte is the frame format: `EPI1` E1004 colour6 (above),
  `EPI2` EE05 mono, `EPI3` EE05 grey4 (same 12 header bytes; see
  [EE05](#ee05)). A firmware replies `@epi err header ...` to a format it
  does not show (for example an E1004 frame sent to the EE05), then drains.

**Payload.** This is the panel's native frame: 1200 wide x 1600 tall, row-major,
4 bits per pixel, two pixels per byte with the **high nibble the left (even-x)
pixel**, 600 bytes per row, 960000 bytes. Nibbles are panel codes: black 0,
white 1, yellow 2, red 3, blue 5, green 6 (4 and 7-15 are invalid). Because
`E` (0x45) contains nibble 4, a magic can never appear inside a valid payload.

**Sequence.**

1. The host opens the port with DTR and RTS deasserted. Opening may still reset
   the board. The host waits up to 2 s for `@epi ready`, then sends `EPI?`
   every 3 s until `@epi ready` arrives. The limit is 90 s, which also covers a
   device still finishing an earlier refresh. Duplicate `ready` replies to
   queued pings are skipped.
2. The host sends the header. The device checks width, height and length and
   replies `@epi ok 0` (or `@epi err header ...`).
3. The host sends the payload in chunks of `min(4096, remaining)` bytes (235
   chunks, the last 1536 bytes), waiting after each for `@epi ok <offset of
   next byte>` (5 s host timeout). The device receives each chunk directly into
   its frame buffer and prints nothing else in the meantime. If a chunk does not
   complete within 3 s, the device replies `@epi err timeout at offset <n>`.
4. After the last chunk, the device compares the CRC32 (`@epi crc ok` or
   `@epi err crc mismatch ...`). It then rejects the frame if any nibble is 4 or
   greater than 6 (`@epi err invalid ink code at byte ...`).
5. The device wakes the panel if it is asleep, writes the frame and refreshes
   (25-30 s, longer cold). It then puts the panel into deep sleep (the image
   stays when unpowered) and replies `@epi done <ms>` (host timeout 180 s by
   default), then `@epi ready`.

On any error the host may resend from step 1 (`--retries`). Errors after
`@epi crc ok` (invalid ink code, panel failure) are not retried. The device RX
buffer is 16 KiB (one chunk plus margin).

If the port gives an error while the host waits for `@epi ready` (a USB device
that re-enumerated after a reset), the host closes and reopens it.

## EE05

This is the Seeed Studio XIAO ePaper Display Board EE05: an XIAO ESP32-S3
driving a 7.5" 800x480 monochrome panel (GooDisplay GDEY075T7, UltraChip UC8179
controller) through the idfxx `idfxx_epaper_uc8179` driver. One firmware shows
both mono (2-level) and 4-grey frames, and the frame header says which.

### Prerequisites

- The EE05 with the panel's FPC connected, on USB-C. The XIAO talks over the
  ESP32-S3's native USB-Serial-JTAG port (no bridge, no driver). On macOS it
  appears as `/dev/cu.usbmodem*` (USB ID `303a:1001`).
- ESP-IDF, idfxx and the Python venv, as for the E1004 above. The firmware was
  built with ESP-IDF v6.1, like the E1004's.
- If flashing cannot connect, put the ESP32-S3 into its ROM bootloader: hold
  the XIAO's BOOT button while plugging in the USB-C cable (or hold BOOT and
  press RESET), flash, then press RESET (or replug) to run the firmware.

### Build and flash

```sh
source ~/.espressif/tools/activate_idf_v6.1.sh   # or your own ESP-IDF activation
cd tools/panel/firmware-ee05
idf.py set-target esp32s3                        # first time only
idf.py build
ls /dev/cu.usbmodem*                             # the XIAO's port
idf.py -p /dev/cu.usbmodem1101 flash             # use the port listed above
```

**USB-Serial-JTAG hazard.** The link can die when the port is opened, most often
on back-to-back sends in 4-grey mode, so space sends apart. To recover: replug,
do an esptool hard reset, wait ~55 s quiet, then send a passive ping with
DTR/RTS false. Never open the port right after a replug.

As for the E1004, `sdkconfig` and `dependencies.lock` are not committed. The
image is about 256 KiB (83% of the app partition free). The firmware assumes
only 4 MB of flash and no PSRAM (like the idfxx `ee05_demo`). Its two
framebuffers (48 KB mono, 96 KB grey4) live in internal RAM.

### Send

```sh
PY=tools/panel/.venv/bin/python
CONVERT=build/tools/convert/convert
$CONVERT --panel gdey075t7-grey4 photo.jpg grey4.bin
$PY tools/panel/send.py --panel ee05 --mode grey4 frame grey4.bin
$CONVERT --panel gdey075t7-mono --rotate photo.jpg mono.bin        # portrait
$PY tools/panel/send.py --panel ee05 --mode mono frame mono.bin
$PY tools/panel/send.py --panel ee05 --mode grey4 test          # four level bars
$PY tools/panel/send.py --panel ee05 --mode mono test           # black and white bars
$CONVERT --panel gdey075t7-grey4 --compare --seed 1 --key key.json photo.jpg cmp.bin \
    preset=Default preset=Soft preset=Deep
$PY tools/panel/send.py --panel ee05 --mode grey4 frame cmp.bin
```

- `--mode` is required with `--panel ee05` (and rejected without it). It must
  match the frame: `convert --panel gdey075t7-mono` (48000 bytes, 1 bpp) for
  `mono`, `gdey075t7-grey4` (96000 bytes, 2 bpp level indices) for `grey4`.
  A frame of the wrong size is rejected, and the error names the `convert` panel
  to use.
- `frame FRAME.bin`: mono frames are sent as they are. Grey4 frames are
  repacked from convert's 2 bpp rows into the firmware's two bit planes
  (below).
- `convert --rotate` renders in portrait (480x800) and rotates the frame the
  same way as the E1004's landscape frames: portrait (x, y) goes to native
  (y, 479 - x). View the panel a quarter turn clockwise from native, with its
  native top edge on your right.
- `convert --compare`: as on the E1004, four 400x240 cells (240x400 with
  `--rotate`).
- `test`: one vertical bar per level, darkest first, unlabelled.
- `--port` defaults to the one serial port with the USB-Serial-JTAG ID
  `303a:1001`, otherwise the one `/dev/cu.usbmodem*` (or Linux `/dev/ttyACM*`)
  device. If there are none or several, the error asks for `--port`. The USB
  port ignores `--baud`.
- `send.py` prints the transfer time and both device times: `refresh` (the
  frame's own refresh) and `total` (wake to sleep, including the grey scrub).

**Native orientation.** The UC8179 driver's framebuffer is 800 wide x 480
tall with (0, 0) at the top left, as the idfxx `ee05_demo` draws it. This is the
panel's natural landscape orientation.

### Frames and the firmware

The wire protocol is as for the E1004 (same messages, chunking, CRC32, retries),
with these differences:

- **Transport**: the XIAO's USB-Serial-JTAG CDC port, which is also the
  console (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG`, no secondary console, logging
  at WARN). The firmware installs the USB-Serial-JTAG driver with a 16 KiB RX
  ring and routes stdio through it, so a stray log line cannot be written into
  the middle of a protocol line. A protocol line inside a log line is still
  found, because the host takes the text after the last `@epi `. The driver
  drops received bytes if its RX ring is full. The per-chunk acks keep at most
  one 4096-byte chunk in flight. Lines to a host that is not reading time out
  rather than block. Opening the port may reset the XIAO: the USB-Serial-JTAG
  resets the chip when RTS is asserted with DTR low, which can happen
  transiently while the port's control lines are being deasserted. The host
  waits for `@epi ready` as for the E1004. It reopens the port if the port gives
  an error during that wait (not mid-transfer).
- **Header**: the same 16 bytes, magic `EPI2` (mono) or `EPI3` (grey4), width
  800, height 480, length 48000 (mono) or 96000 (grey4).
- **Payload** (the idfxx framebuffer layouts, received straight into them):
  - mono: 48000 bytes, `idfxx::epaper::mono_framebuffer`: row-major, 100
    bytes per row, bit 7 the leftmost pixel. A **set bit is white** and a
    cleared bit is black, so the bit is the level index.
  - grey4: 96000 bytes, `idfxx::epaper::gray4_framebuffer`: plane 0 (bit 0 of
    each pixel's `gray4` level) then plane 1 (bit 1), each in the mono layout.
    `gray4` runs **white = 0**, light = 1, dark = 2, black = 3, that is,
    `3 - level index`. (The driver inverts the planes on their way to the
    controller.) `send.py` makes this from convert's 2 bpp frame
    (`grey4_to_planes`).
  - 800 is a multiple of 8, so there are no padding bits, and every byte
    value is a valid pixel pattern. The device checks only the CRC. (Unlike the
    E1004's, these payloads can contain the magic bytes. This is harmless,
    because the device never scans for a magic inside a payload.)
- **Display**: mono frames: wake, mono mode, write, full refresh (the
  controller's OTP full waveform), deep sleep. Grey4 frames: wake, then a
  fixed scrub in mono mode, then gray4 mode, write, refresh with the driver's
  4-grey waveform, deep sleep. The scrub is a full refresh to black, then
  `clear()` and a full refresh to white. The black frame fills the controller's
  previous-image plane, so the white refresh gives every pixel the deep
  black-to-white erase rather than white-to-white. The scrub makes each grey
  frame start from the same state, whatever was shown before. It does not
  remove ghosting from content held on the panel for a long time (see the
  driver's README), so do grey calibration shots on a panel that has not held
  one image for long. `@epi done <ms> total <ms>` gives the frame's own refresh,
  and wake to sleep.

### Waveforms

Grey and mono calibrations depend on the drive waveform.

- The driver loads the 4-grey waveform tables through registers (default
  20 Hz at +/-15 V: `idfxx_epaper_uc8179` `config::gray4_frame_rate` /
  `gray4_source_voltage`). The grey refresh takes about 9.0 s. Grey levels
  therefore depend on the frame rate, the voltage **and** the driver version, so
  re-calibrate after changing any of them.
- Mono full refreshes use the OTP waveform (`_init_full`: power setting
  `07 07 3F 3F`, booster `17 17 28 17`, panel setting `0x1F`, TCON `0x22`),
  with no forced temperature.
- The driver's only other register LUTs are for the `fast` mono refresh, which
  the viewer does not use.
