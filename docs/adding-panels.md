# Adding a panel

The library models a panel as inks mixed by area, so other ink-palette panels
work as data. Kaleido and other colour-filter panels need a different output
model and aren't supported.

To support a new panel, you measure the colours it really shows and give them
to the library as a calibration. How you do that depends on the panel:

| Panel | Route | Library changes |
|---|---|---|
| Grey, 2 to 16 levels | [Build it at runtime](#grey-panel-at-runtime) with `epi_grey_panel_init` | None |
| Grey, shipped with the library | [Generate it](#built-in-grey-panel) with `tools/panelgen` | New source file |
| Colour, 5 to 16 inks | [Build it at runtime](#colour-panel-at-runtime) with `epi_colour_panel_init` | None |
| Colour, shipped with the library | [Generate it](#built-in-colour-panel) with `tools/panelgen` | New source file |

Every route starts with the same measurements.

## Measure the panel

For each ink or grey level you need three values, which make up an `epi_ink_t`:

- `code`: the value sent to the panel.
- `nominal`: the ink's ideal sRGB8 colour, used for nominal previews. A colour
  panel's init also compares the nominal colours to check which ink is white.
- `measured`: the sRGB8 colour the panel really shows. Matching, dithering
  and measured previews all use this value, so the quality of the calibration
  depends on it.

Grey levels also depend on how the panel is driven. A different waveform, frame
rate, voltage, LUT or driver changes them, so write down what you measured with
and measure again if it changes.

You also need the **detail sigma**, the size of the blur used to preserve local
detail, in panel pixels. A good value is the number of pixels in about 1 mm of
the panel: 6.0 for the 13.3" EL133UF1 and 5.0 for the 7.5" GDEY075T7.

### Measuring with an instrument

The most accurate way to measure a panel is with a spectrophotometer that can
read reflective surfaces. Many colorimeters are made for screens that emit light
and can't do this, so check yours first. Set the instrument to D65 and measure a
solid patch of each ink or level, larger than its aperture, then convert the
readings as in step 6 below.

**Contributions welcome.** If you have a suitable instrument and one of the
supported panels, please share your readings by opening an issue on
[GitHub](https://github.com/cleishm/epaper-image). They would help to improve
the shipped calibrations. New panels are welcome too.

### Measuring with a camera

The shipped calibrations were measured with a phone's raw photos and no
instrument or colour reference, so their colours are estimates. The repo has no
measuring tool, but this is the method they used:

1. **Show a chart.** Use solid patches of each ink or level, with the lightest
   level around the chart as a brightness reference. Show each patch inside a
   ring of the darkest level and again inside a ring of the lightest, because
   neighbouring areas change the reading: the EL133UF1's black read 0.06 of
   white beside white, and 0.11 in a dark ramp. For grey levels, use a panel that
   has not held one image for a long time, because ghosting shifts the levels.
2. **Take the photos.** Take at least 3 raw photos in daylight, with a dark or
   neutral background behind the panel. Bright surroundings raise the black
   reading.
3. **Correct for uneven light.** Fit a smooth brightness field to the surrounding
   area and divide it out, then convert camera RGB to XYZ with the raw file's
   colour matrix.
4. **Average the patches.** Average the pixels of each patch, staying a few
   pixels away from any other colour, then average the patches for each ink.
5. **Drop photos with glare.** If black and the dark greys are all raised
   together in one photo, drop that photo.
6. **Convert to sRGB8.** Keep each ink's colour relative to white, and express
   the results against the shipped white: (190, 200, 200) for colour, or a neutral
   198 for grey.

A camera is not a colorimeter. Its brightness ratios are more reliable than its
colours, so make grey levels neutral, keeping only the ratio to white. The
EL133UF1's black was also made neutral, because its blue tint looked like a
camera error. If you use a camera, photographing a reference chart, such as a
ColorChecker, beside the panel in the same light should give more reliable colours
than the raw file's colour matrix.

## Grey panel at runtime

This needs no generated code. Pass the levels, darkest first, to
`epi_grey_panel_init`:

```c
#include <stdio.h>
#include <stdlib.h>

#include <epaper_image.h>

static const epi_ink_t levels[] = {
    /* code, nominal, measured */
    {0, {0, 0, 0}, {58, 58, 58}},
    {1, {85, 85, 85}, {102, 102, 102}},
    {2, {170, 170, 170}, {175, 175, 175}},
    {3, {255, 255, 255}, {198, 198, 198}},
};

static const epi_grey_panel_config_t grey = {
    .name = "My panel", /* not copied: must outlive the panel */
    .width = 800,
    .height = 480,
    .levels = levels, /* copied: can be freed after init */
    .level_count = 4,
    .detail_sigma = 5.0f,
};

const epi_panel_t* make_panel(void) {
    size_t size = epi_grey_panel_size(&grey); /* 0 if the config is invalid */
    void* storage = size ? malloc(size) : NULL;
    const epi_panel_t* panel = NULL;
    const char* reason = NULL; /* why the config or levels were rejected, for logs */
    epi_err_t err = epi_grey_panel_init(&grey, storage, size, &panel, &reason);
    if (err != EPI_OK) {
        fprintf(stderr, "grey panel: %s%s%s\n", epi_strerror(err), reason ? ": " : "", reason ? reason : "");
        free(storage);
    }
    return panel; /* NULL on failure */
}
```

Use the result like a built-in panel, in `epi_config_t.panel`. Its storage must
outlive the panel and every converter using it. It includes Default, Soft and
Deep presets.

The config is rejected unless:

- There are 2 to 16 levels.
- Luminance increases strictly from each level to the next.
- The darkest level has an L\* below 66.29, which is a contrast of about 2.8:1.
- The detail sigma is above 0 and at most 10.5.

## Built-in grey panel

This generates the same tables as `epi_grey_panel_init`, as constants in the
library. Set up panelgen's Python environment first, as described in
[tools/panelgen/README.md](https://github.com/cleishm/epaper-image/blob/main/tools/panelgen/README.md). The commands below run
from `tools/panelgen`, using its `.venv/bin/python`.

1. **Add the measurements.** Commit a JSON file as
   `tools/panelgen/measurements/<panel>-levels-<date>.json`, with one entry per
   mode, named `<ident>_<mode>`:

   ```json
   {
     "mypanel_grey4": {
       "nominal": [[0, 0, 0], [85, 85, 85], [170, 170, 170], [255, 255, 255]],
       "perceived": [[58, 58, 58], [102, 102, 102], [175, 175, 175], [198, 198, 198]],
       "codes": [0, 1, 2, 3],
       "photocal": {
         "date": "2026-01-31",
         "waveform": "my_driver abc1234, 4-grey waveform, 20 Hz, ±15 V"
       }
     }
   }
   ```

   `perceived` holds the measured colours, darkest first. `date` and `waveform`
   are required, and are recorded in the generated file.
2. **Define the panel.** Add a `GreyPanel` to `GREY_PANELS` in
   `panelgen/panels.py`, with `levels` set to the JSON's repo-relative path. See
   `GDEY075T7` in that file for an example.
3. **Generate the tables.** Run `python -m panelgen <ident>`. Check the printed
   levels, thresholds and darkest L\* against your measurements. This writes
   `src/panels/<ident>.c`, with one `epi_panel_<ident>_<mode>` per mode.
4. **Finish the library changes**, as described in
   [Add it to the library](#add-it-to-the-library). Declare each mode separately.

To recalibrate a shipped panel, commit the new JSON, point `levels` at it, and
regenerate.

## Colour panel at runtime

This needs no generated code. Pass the inks, in the order the panel should match
them because ties go to the earlier ink, to `epi_colour_panel_init`:

```c
#include <stdio.h>
#include <stdlib.h>

#include <epaper_image.h>

static const epi_ink_t inks[] = {
    /* code, nominal, measured */
    {0, {0, 0, 0}, {45, 45, 50}},
    {1, {255, 255, 255}, {200, 200, 195}},
    {2, {255, 0, 0}, {170, 50, 50}},
    {3, {0, 255, 0}, {60, 140, 90}},
    {4, {0, 0, 255}, {60, 80, 170}},
};

static const epi_colour_panel_config_t colour = {
    .name = "My panel", /* not copied: must outlive the panel */
    .width = 800,
    .height = 480,
    .inks = inks, /* copied: can be freed after init */
    .ink_count = 5,
    .detail_sigma = 5.0f,
};

const epi_panel_t* make_panel(void) {
    size_t size = epi_colour_panel_size(&colour); /* 0 if the config's structure is invalid */
    void* storage = size ? malloc(size) : NULL;
    const epi_panel_t* panel = NULL;
    const char* reason = NULL; /* why the config or inks were rejected, for logs */
    epi_err_t err = epi_colour_panel_init(&colour, storage, size, &panel, &reason);
    if (err != EPI_OK) {
        fprintf(stderr, "colour panel: %s%s%s\n", epi_strerror(err), reason ? ": " : "", reason ? reason : "");
        free(storage);
    }
    return panel; /* NULL on failure */
}
```

Use the result like a built-in panel, in `epi_config_t.panel`. Its storage must
outlive the panel and every converter using it. It includes Default, Soft, Deep and
Vivid presets.

Init runs about 400,000 in-gamut tests in double, each asking whether a colour
lies inside the convex hull of the inks. On a chip without a double-precision FPU,
that blocks for seconds (about 6 s on a 240 MHz ESP32-S3), so call it once at
startup, or generate a built-in panel instead.

For a target where the block matters, set `progress` in the config. Init calls it
regularly, with how much is done and the total, and returns `EPI_ERR_CANCELLED` if
it returns false. It must not touch the storage or change the config. On ESP-IDF,
`vTaskDelay(1)` in it lets the idle task run and so feeds the idle task watchdog,
while `taskYIELD()` won't let a lower-priority idle task run. A delay on each call
costs a tick each time, so the callback may delay only every Nth call.
`esp_task_wdt_reset()` is the alternative for a task subscribed to the watchdog.

```c
static bool yield_to_idle(void* ctx, unsigned done, unsigned total) {
    (void)ctx;
    (void)total;
    if (done % 10 == 0) {
        vTaskDelay(1);
    }
    return true; /* false cancels */
}
/* ... .progress = yield_to_idle, in the config */
```

`epi_colour_panel_size` looks only at the config's structure; the inks are
checked by `epi_colour_panel_init`, against the [limits](#limits-on-colour-inks).
When it rejects the config or the inks it also gives a short English reason, for
logs and people; don't compare or parse it.

## Built-in colour panel

This runs the derivation of `epi_colour_panel_init` when you generate the panel, and
stores the tables as constants in the library. Set up panelgen's Python environment
first, as described in
[tools/panelgen/README.md](https://github.com/cleishm/epaper-image/blob/main/tools/panelgen/README.md).
You also need a C compiler: `$CC`, or else `cc`. The commands below run from
`tools/panelgen`, using its `.venv/bin/python`.

1. **Add the inks.** In `panelgen/palette.py`, add each ink's nominal and
   measured sRGB8 to a table in `BUILTIN_TABLES`. See `SPECTRA6_PHOTO` for an
   example. List the inks in the order the panel should match them, because ties
   go to the earlier ink.
2. **Define the panel.** Add a `Panel` to `PANELS` in `panelgen/panels.py`, with:
   - the C identifier and display name
   - the palette and table names
   - the size
   - the ink names, in table order, and each ink's code
   - the detail sigma
   - the presets, default first
3. **Generate the tables.** Run `python -m panelgen`. This writes
   `src/panels/<ident>.c`, with `epi_panel_<ident>`, and prints a summary of the
   tables. Inks outside the [limits](#limits-on-colour-inks) stop it with the
   reason.
4. **Finish the library changes**, as described in
   [Add it to the library](#add-it-to-the-library).

## Limits on colour inks

The inks must meet these limits, or `epi_colour_panel_init` and panelgen, which runs
the same checks, reject them, with a reason:

- 5 to 16 inks: white, black and at least three colours around the neutral axis.
  Four inks can't surround it, which rules out a black, white, red and yellow panel.
- The inks must span a 3-D colour volume of at most 32 faces, and the neutral
  axis from the darkest to the lightest ink must run through it.
- The neutral range's darkest L\* must be at least 0 and below 66.29.
- The ink with the largest measured Y is the white ink, and its nominal Y must
  not be below any other ink's. All nominal colours zero, or all equal, skip this.
- The black ink's Y must be at least 0 and below the white ink's, and the white
  ink's X, Y and Z must each be above 0.008 (Y = 1 is diffuse white). None of the
  white ink's red, green and blue may be 0.
- No ink may have twice as much of a cone response as the white ink.
- The L\* of each hue's most saturated colour must be at least 0.5 inside the
  neutral range. A strongly tinted black can break this, because the hues
  towards it then peak at the very bottom of the range.
- The detail sigma must be above 0 and give a blur radius (3 times sigma, rounded
  up) of at most 32 pixels: at most 10.5.

## Add it to the library

1. Add `src/panels/<ident>.c` to both source lists in the root `CMakeLists.txt`:
   `idf_component_register` for ESP-IDF, and `add_library`.
2. Declare the panel in `include/epaper_image.h`, for example
   `extern const epi_panel_t epi_panel_<ident>;`. For grey, say which waveform
   the levels were measured with.
3. Add it to the `PANELS` table in `tools/convert/convert.c`, with the packed
   format the panel is sent.
4. Add it to the table in the project README.

## Check the result

1. Run panelgen's tests and check that the generated files are current:

   ```sh
   cd tools/panelgen
   .venv/bin/python -m pytest -q
   .venv/bin/python -m panelgen --check          # colour panels
   .venv/bin/python -m panelgen <ident> --check  # a grey panel
   ```

2. Build and run the library's tests:

   ```sh
   cmake -B build
   cmake --build build
   ctest --test-dir build --output-on-failure
   ```

3. For ESP32 targets, run the QEMU tests as described in
   [tests/esp_idf/README.md](https://github.com/cleishm/epaper-image/blob/main/tests/esp_idf/README.md).
4. Convert a few photos with `tools/convert` and show them on the panel. For a
   panel that `tools/panel` doesn't support, send the frame with your own
   firmware.
