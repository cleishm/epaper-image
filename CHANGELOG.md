# Changelog

All notable changes to **epaper-image** are documented in this file. The project
follows [semantic versioning](https://semver.org/).

## v0.4.0

Adds `epi_panel_preset`, which finds a panel's preset by name. The API is
otherwise unchanged, and so is panel output.

### Added

- `epi_panel_preset(panel, name)` returns the intent of the panel's preset with
  that name, ignoring the case of ASCII letters, or the default preset for a
  `NULL` name. It returns `NULL` for a name the panel lacks, which
  `epi_config_t` also takes as the default.
- The docs now say to start a custom intent from a copy of a preset: a
  zero-initialised intent has vibrance 0, which removes colour.

## v0.3.0

Lightens the colour presets' lower midtones, corrects the EL133UF1's green cast
on near-neutral greys, and replaces the Vivid preset with Bright. Intents and
colour panel configs gain fields, which breaks the API.

**Panel output:** EL133UF1 frames change for every preset: lower midtones are
lighter, and greys built from coloured inks no longer look green. Grey and mono
output is unchanged.

### Breaking changes

- `epi_intent_t` gains `lift`: L* added to lower midtones, fading out by L* 82
  and sparing saturated colours, 0 to 20. An intent initialised without it gets
  0, the old rendering.
- `epi_colour_panel_config_t` gains `neutral` and `neutral_count`, an optional
  neutral-cast correction. A zero-initialised config has none. Init rejects a
  `NULL` `neutral` with a non-zero count, or the reverse.
- The EL133UF1 and runtime colour panels offer Default, Soft, Deep and Bright.
  Vivid is gone; for its chroma boost, set `vibrance` to 1.3 on any intent.

### Added

- Colour presets lift by 8, because paper white is far darker than a screen's
  and unlifted lower midtones read dark. Bright (tone 0.5, lift 16) is for
  low-key images that still read dark at 8. Grey presets keep lift 0.
- `epi_neutral_knot_t`: a* and b* offsets keyed on final L*, interpolated
  between up to 16 knots and faded out between chroma 12 and 32. The built-in
  EL133UF1 carries knots measured on the glass.
- `convert` gains `--lift`, and `--compare` keys record it.

## v0.2.0

Adds linear-light previews of any size, and colour panels built at runtime from
measured inks. The converter now outputs ink codes only, which breaks the API.

**Panel output:** EL133UF1 frames change. Two cusp table entries move by one,
which reseeds the dither, so the frames differ pixel by pixel but look the same
on the panel. Grey and mono output is unchanged.

### Breaking changes

- `epi_converter_pop` writes `width` ink codes into a `uint8_t*`. Pack them for
  the panel with `epi_pack_row`, or pass them to `epi_preview_push`. Firmware
  streaming to a panel needs that call and a row buffer, and the converter's
  workspace shrinks by one row.
- `epi_output_t`, `EPI_OUT_*`, `epi_config_t.output` and `epi_output_row_size`
  are removed.
- A panel whose codes don't fit a packed format is rejected by `epi_pack_row`,
  not at converter init.
- `epi_grey_panel_init` takes a new `const char** reason`, which may be `NULL`.
  It gets a short static string on every `EPI_ERR_INVALID_ARG`.

### Added

- `epi_preview_*` reduces rows of ink codes to an RGB preview of any smaller
  size, averaging in linear light with integer weights, so the output is the
  same on every platform.
- `epi_pack_row`, `epi_pack_row_size` and `epi_pack_format_t` pack a row of ink
  codes for the panel.
- `epi_colour_panel_init` and `epi_colour_panel_size` build a colour panel from
  5 to 16 measured inks in caller storage, with no allocation or libm, and the
  same result on every target. This takes about 6 s on a 240 MHz ESP32-S3. An
  optional `epi_progress_fn_t` callback can yield to other tasks, or cancel,
  which returns the new `EPI_ERR_CANCELLED`.
- `convert` gains `--preview-scale`.

Init rejects inks whose nominal colours disagree with the measurements: a white
ink (the largest measured Y) whose nominal Y is below another ink's, or a black
ink (the smallest measured Y) whose nominal Y is above another ink's.

### Tools and packaging

- `panelgen` now runs the library's derivation, so a built-in panel equals the
  runtime panel from the same inks. It no longer needs numpy or scipy.
- The component package leaves out `.clang-format`, `.clang-format-ignore`,
  `.gitignore`, `docs/Doxyfile`, `docs/html` and `docs/xml`.

## v0.1.1

`epi_panel_t` can now be forward-declared, so a header can use it without
including `<epaper_image.h>`: `typedef struct epi_panel epi_panel_t;`. Its fields
and layout are unchanged, existing code still compiles, and panel output is
unchanged.

Also tidies the remaining American spellings in code comments and docs. The
public API was already British and doesn't change.

## v0.1.0

First release of epaper-image, a C11 library that converts images for e-paper
panels: streaming resampling, tone adjustment, palette matching and error
diffusion.

Built-in panels: EL133UF1 (Spectra 6) and GDEY075T7 (mono and 4 greys).
