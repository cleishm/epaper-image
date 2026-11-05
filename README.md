# epaper-image

A C11 library that converts images for e-paper panels. You give it rows of RGB
and get back rows of panel ink codes. From those codes you can make an RGB preview
and packed rows for the panel. Images are rendered using the colours measured on
the panel.

## Features

- **Streaming.** Push source rows in and pop rows of ink codes out. You never need
  the whole image or frame in memory.
- **No allocation.** The library never allocates memory. You provide the
  workspaces, and you can ask for their sizes up front.
- **Deterministic.** Each pixel is processed with integer maths only, so the
  output is byte-for-byte the same on a host, on ESP32 and in WASM. A preview
  on your computer uses exactly the ink codes the panel will get. Its colours
  are estimates from the panel's measured inks.
- **Rendering for measured inks.** Colours are adjusted for the paper's white
  and mapped to what the panel's inks can show. A small `epi_intent_t` controls
  tone, local detail and vibrance, and each panel has named presets.
- **Dithering.** Each pixel gets the nearest ink in OKLab, and the difference
  is spread to its neighbours with serpentine Floyd-Steinberg, damped to limit
  grain.
- **Resizing to panel size.** Box, bilinear, bicubic and Lanczos3 filters in
  fixed point, with cover and fit layouts.
- **Output formats.** The converter gives one ink code per byte. From those codes,
  `epi_pack_row` makes rows packed at 4, 2 or 1 bits per pixel, and `epi_preview_*`
  makes an RGB preview in the inks' measured or nominal colours.

## Supported panels

Each panel's calibration is an `epi_panel_t` named after the panel:

| Symbol | Panel |
|---|---|
| `epi_panel_el133uf1` | E Ink EL133UF1, Spectra 6, 13.3" 1200x1600 (for example the reTerminal E1004) |
| `epi_panel_gdey075t7_mono` | GDEY075T7, 7.5" 800x480, black and white (for example Seeed's 7.5" module on the XIAO EE05) |
| `epi_panel_gdey075t7_grey4` | GDEY075T7, 7.5" 800x480, 4 greys, measured with idfxx_epaper_uc8179's 4-grey waveform at its default 20 Hz, ±15 V |

Each calibration has named presets, and the first is the default. For other
settings, pass your own `epi_intent_t` in `epi_config_t`. A panel's grey levels
depend on how it is driven, so a different waveform, frame rate, voltage, LUT
or driver needs its own calibration.

The reTerminal E1001 is likely the same panel as the GDEY075T7, but it has not
been measured.

To add another panel, see [Adding a panel](docs/adding-panels.md).

## Installation

The library runs on Linux, macOS, Windows, ESP-IDF (ESP32 family) and
Emscripten/WASM. It needs only the C standard library.

### ESP-IDF component

```bash
idf.py add-dependency "cleishm/epaper-image"
```

or in your component's `idf_component.yml`:

```yaml
dependencies:
  cleishm/epaper-image: "*"
```

On most ESP32 chips, the workspace for a full-width colour panel won't fit in
free internal RAM, so allocate it from PSRAM (for example with
`heap_caps_malloc(size, MALLOC_CAP_SPIRAM)`). Grey panels fit in internal RAM.

### CMake

As a subdirectory:

```cmake
add_subdirectory(third_party/epaper-image)
target_link_libraries(your_target PRIVATE epaper_image::epaper_image)
```

or download it from
[GitHub](https://github.com/cleishm/epaper-image) when configuring:

```cmake
include(FetchContent)
FetchContent_Declare(
    epaper_image
    GIT_REPOSITORY https://github.com/cleishm/epaper-image.git
    GIT_TAG v0.3.0
)
FetchContent_MakeAvailable(epaper_image)

target_link_libraries(your_target PRIVATE epaper_image::epaper_image)
```

Once installed, the library can also be found with
`find_package(epaper_image)`.

## Usage

You decode the source image yourself. This example converts a decoded RGB image
to a packed 4-bit frame for the EL133UF1, scaling it to cover the panel and
cropping it around the centre:

```c
#include <stdint.h>
#include <stdlib.h>

#include <epaper_image.h>

/* `src` is src_w x src_h pixels, row-major. `frame` receives 1600 rows of
 * 600 bytes. Returns 0 on success. */
int convert(const epi_rgb_t* src, uint16_t src_w, uint16_t src_h, uint8_t* frame) {
    const epi_panel_t* panel = &epi_panel_el133uf1;
    int rc = -1;

    /* Layout and resampler: scale the source to cover the panel. */
    epi_resampler_config_t rcfg = {
        .src_width = src_w,
        .src_height = src_h,
        .dst_width = panel->width,
        .dst_height = panel->height,
        .filter = EPI_FILTER_LANCZOS3,
    };
    epi_layout_cover(&rcfg); /* sets the source crop and the placement */

    /* Converter: panel-sized rows in, ink codes out. */
    epi_config_t ccfg = {
        .panel = panel,
        .intent = NULL, /* the panel's default preset */
        .width = panel->width,
        .height = panel->height,
    };

    /* Workspaces: the library never allocates. */
    size_t rsize = epi_resampler_workspace_size(&rcfg);
    size_t csize = epi_converter_workspace_size(&ccfg);
    void* rmem = malloc(rsize);
    void* cmem = malloc(csize);
    epi_rgb_t* line = malloc(panel->width * sizeof *line);
    uint8_t* codes = malloc(panel->width);
    if (rsize == 0 || csize == 0 || !rmem || !cmem || !line || !codes) goto done;

    epi_resampler_t* r;
    epi_converter_t* c;
    if (epi_resampler_init(&rcfg, rmem, rsize, &r) != EPI_OK) goto done;
    if (epi_converter_init(&ccfg, cmem, csize, &c) != EPI_OK) goto done;

    size_t row_bytes = epi_pack_row_size(EPI_PACK_4BPP, panel->width);
    uint8_t* out = frame;

    /* Push source rows; after every push, drain both stages. Each row of ink
     * codes is packed into the frame. */
    for (uint16_t y = 0; y < src_h; y++) {
        epi_resampler_push(r, src + (size_t)y * src_w);
        while (epi_resampler_pop(r, line) == EPI_OK) {
            epi_converter_push(c, line);
            while (epi_converter_pop(c, codes) == EPI_OK) {
                if (epi_pack_row(panel, EPI_PACK_4BPP, codes, panel->width, out) != EPI_OK) goto done;
                out += row_bytes;
            }
        }
    }
    rc = 0;
done:
    free(codes);
    free(line);
    free(cmem);
    free(rmem);
    return rc;
}
```

After every push, keep popping until `pop` stops returning `EPI_OK`. Output rows
come out later than the input rows that produce them, and the last push releases
the rest. If an image is already panel-sized, skip the resampler and push its
rows straight to the converter.

### One conversion, a preview and a frame

A preview is a separate step from conversion. The converter gives ink codes, and
both the preview and the frame come from those codes, so an app that shows a
preview on an LCD and sends the frame to the device needs to convert only once.
As each row of codes pops, push it to `epi_preview_*` and pack it with
`epi_pack_row`, as in this outline:

```c
epi_config_t ccfg = {
    .panel = panel,
    .intent = panel->presets[preset].intent,
    .width = panel->width,
    .height = panel->height,
};
/* ...set up the resampler and converter as above; `codes` holds one row of
 * panel->width bytes... */

/* The preview, in screen pixels. */
epi_preview_config_t pcfg = {
    .panel = panel,
    .src_width = panel->width,
    .src_height = panel->height,
    .dst_width = preview_w,
    .dst_height = preview_h,
};
size_t size = epi_preview_workspace_size(&pcfg);
void* mem = malloc(size);
epi_preview_t* p;
if (size == 0 || !mem || epi_preview_init(&pcfg, mem, size, &p) != EPI_OK) return -1;
epi_rgb_t* pixels = preview;

/* The frame for the panel. */
size_t row_bytes = epi_pack_row_size(EPI_PACK_4BPP, panel->width);
uint8_t* packed = frame;

/* Where the converter is drained: */
while (epi_converter_pop(c, codes) == EPI_OK) {
    epi_preview_push(p, codes);
    while (epi_preview_pop(p, pixels) == EPI_OK) pixels += preview_w;
    if (epi_pack_row(panel, EPI_PACK_4BPP, codes, panel->width, packed) != EPI_OK) return -1;
    packed += row_bytes;
}
free(mem); /* after the last row */
```

An app can also keep the codes, `panel->height` rows of `panel->width` bytes, to
make a preview at another size without converting again.

Size the preview in screen pixels, not layout points: a preview 200 points wide on
a 2x screen should be 400 pixels wide, because a smaller image visibly loses
detail. A preview needn't be larger than the panel, and the reducer doesn't
enlarge. Sides from 1 up to the panel's are accepted, on each axis, at any ratio.

Reduce with `epi_preview_*` rather than an ordinary image scaler. A scaler averages
sRGB values, which makes dithered areas too dark: equal black and white dots give
128 rather than the correct 188. The reducer averages in linear light, as the eye
does at a distance.

The library doesn't rotate, because a quarter turn needs the whole frame in memory.
To display in an orientation other than the panel's native one, do these steps in
order:

1. Convert in the display orientation. A quarter turn swaps width and height.
2. Make the preview from those codes, so that it comes out upright.
3. Rotate the codes to the panel's native layout.
4. Pack them into the frame.

## Memory and performance

The library never allocates memory. Converters, resamplers and preview reducers
run in workspaces you provide and work row by row, so no workspace has to hold a whole image. A
converter's workspace size depends only on the panel and width. A resampler's
also grows with the source size, because shrinking an image reads more source
pixels for each output pixel. `epi_converter_workspace_size`,
`epi_resampler_workspace_size` and `epi_grey_panel_size` return exact sizes.
Typical figures:

| Configuration | Workspace |
|---|---|
| Colour (EL133UF1), 1200 wide | 195 KB |
| Colour (EL133UF1), 1600 wide | 258 KB |
| Grey (GDEY075T7), 800 wide | 90 KB |
| Resampler, 4000x3000 to 1200x1600 | 21 KB (box) to 102 KB (Lanczos3) |

Converting a 1600x1200 image to a 1200x1600 colour frame with Lanczos3 takes
about 0.6 s natively on an Apple silicon laptop and about 1 s as WebAssembly
under Node. On a 240 MHz ESP32-S3 it takes about 51 s, and an 800x480 grey frame
takes 3.7 s. Colour times vary with the image: colours the panel can't show need
a search, so a very saturated photo takes longer (83 s in the worst case
measured on the ESP32-S3).

### Untrusted images

The library reads decoded rows, not image files, and every pixel value is valid.
A bug in parsing a hostile file is up to your decoder, so choose one that is
hardened for untrusted input. `stb_image`, which `tools/convert` uses, is not.

A small file can also declare a huge width and height. The library rejects a
source over 100 megapixels, or with a side over 65535 pixels. Raise or lower the
area limit with `max_src_pixels`. You can assign a 32-bit declared size directly
to `src_width` and `src_height`: they are 32-bit too, so a large or negative value
can't wrap to a valid size.

Check the size before decoding, so a hostile file is rejected before your decoder
allocates memory:

```c
cfg.src_width = declared_width;
cfg.src_height = declared_height;
epi_layout_cover(&cfg);
size_t size = epi_resampler_workspace_size(&cfg);
if (size == 0 || size > budget) {
    /* reject the image */
}
```

The size is 0 if the image is over the limit. Check your memory budget as well,
because a long, thin image needs a large workspace for its area:

| Source, scaled to 1200x1600 | Box | Lanczos3 |
|---|---|---|
| 4000x3000, cover | 21 KB | 102 KB |
| 10000x10000, cover | 60 KB | 323 KB |
| 65441x409, fit | 480 KB | 2.8 MB |

An image at the limit can still take a long time on a microcontroller, so consider
a lower limit there. You push the rows yourself, so you can stop between any two
rows with nothing to clean up. Check a deadline or feed a watchdog there.

## Tools

- `tools/convert` is a command-line converter.
  `convert [options] INPUT OUTPUT` reads an image (PNG, JPEG, BMP and others,
  using [stb_image](https://github.com/nothings/stb), downloaded when
  configuring), applies a JPEG's EXIF orientation, and writes the packed frame
  for the panel. Options set the panel (`--panel`, `--list-panels`), a preset
  or individual intent values (`--preset`, `--tone`, `--detail`, `--adapt`,
  `--vibrance`, `--veil`, `--lift`), the layout (`--cover` or `--fit` with
  `--background`), the resampling filter and rotation, and can save a preview
  PNG of what the panel will show (`--preview`, `--nominal`), at a fraction of
  the shown size with `--preview-scale`. `--compare` puts
  two to four versions into labelled cells of one frame for blind comparison.
  Run `convert --help` for the full list. It is built when
  `EPAPER_IMAGE_BUILD_TOOLS` is on. It is a development tool and shouldn't be
  given untrusted files.
- `tools/panel` shows converted frames on a real panel during development.
  It has ESP-IDF firmware that receives a frame over USB serial, and `send.py`,
  which sends it. See [tools/panel/README.md](https://github.com/cleishm/epaper-image/blob/main/tools/panel/README.md) for the
  supported boards.
- `tools/panelgen` generates the tables for colour and grey panels shipped as
  const calibrations. See
  [tools/panelgen/README.md](https://github.com/cleishm/epaper-image/blob/main/tools/panelgen/README.md).

## Building and testing

CMake options:

| Option | Default | Effect |
|---|---|---|
| `EPAPER_IMAGE_BUILD_TESTS` | on when top level | Build the test suite |
| `EPAPER_IMAGE_BUILD_TOOLS` | on when top level, off under Emscripten | Build `tools/convert` |
| `EPAPER_IMAGE_WARNINGS_AS_ERRORS` | off | Treat compiler warnings in the library as errors |
| `EPAPER_IMAGE_BUILD_FUZZ` | off | Build the randomised API test, for sanitizer runs (needs `EPAPER_IMAGE_BUILD_TESTS`) |
| `EPAPER_IMAGE_BUILD_DOCS` | off | Add a `docs` target that runs Doxygen |

```bash
cmake -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

The tests use [Unity](https://github.com/ThrowTheSwitch/Unity), downloaded when
configuring. The same tests run on ESP32 targets, normally under QEMU, using the
ESP-IDF test app in [tests/esp_idf](https://github.com/cleishm/epaper-image/blob/main/tests/esp_idf/README.md).

For WASM, configure with the Emscripten toolchain and run the tests under Node:

```bash
emcmake cmake -B build-wasm -DEPAPER_IMAGE_BUILD_TOOLS=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build-wasm
ctest --test-dir build-wasm --output-on-failure
```

CI also checks the formatting of the C and C++ sources, using clang-format
23.1.3 (`pip install clang-format==23.1.3`) and the repository's
`.clang-format`. Generated tables are exempt (`.clang-format-ignore`).

## Documentation

- [docs/design.md](docs/design.md) describes how the library and its algorithms
  work.
- [docs/adding-panels.md](docs/adding-panels.md) explains how to add a panel.
- To generate the API reference with Doxygen, configure with
  `-DEPAPER_IMAGE_BUILD_DOCS=ON` and build the `docs` target. You can also read
  the comments in the header,
  [include/epaper_image.h](https://github.com/cleishm/epaper-image/blob/main/include/epaper_image.h).

## License

MIT; see [LICENSE](https://github.com/cleishm/epaper-image/blob/main/LICENSE). The resampler's coefficient computation is derived
from [Pillow](https://python-pillow.org)'s resampling code, which is under the
MIT-CMU License (reproduced in LICENSE).
