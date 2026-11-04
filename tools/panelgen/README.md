# panelgen

panelgen generates the C library's constant tables (docs/design.md, "Panel
tables"). The colour maths is in `panelgen/colour.py` (sRGB, XYZ, Lab,
Bradford), `panelgen/palette.py` (the ink tables), `panelgen/gamut.py`
(`InkGamut`: the ink hull, Bradford/BPC) and `panelgen/model.py` (the
ink-relative OKLab, `cusp_lut` and `panel_partial`), and in `panelgen/grey.py`
for grey calibrations. `panelgen/gen.py` only folds matrices, quantises and
writes C.

Set up this tool's venv first, from `tools/panelgen` (numpy, scipy, pytest; see
`requirements.txt`):
`python3 -m venv .venv && .venv/bin/pip install -r requirements.txt`.

```sh
cd tools/panelgen && .venv/bin/python -m panelgen          # write
cd tools/panelgen && .venv/bin/python -m panelgen --check  # regenerate and diff; exit 1 if stale
cd tools/panelgen && .venv/bin/python -m pytest -q         # self-checks
```

It writes:

- `src/epi_srgb.c`: the panel-independent sRGB EOTF table and the sRGB to
  XYZ matrix, both Q16. They are declared in `src/epi_srgb.h`, which is
  written by hand.
- `src/panels/<panel>.c`: an `epi_colour_tables_t`
  (`src/epi_panel_tables.h` gives each field's Q format) and the public
  `epi_panel_<panel>`.
- With a grey panel and its measured levels (`panelgen gdey075t7 --levels
  …`), `src/panels/<glass>.c` with one grey calibration per mode. See
  [Grey calibrations](#grey-calibrations).

Panels are defined in `panelgen/panels.py`. A definition holds the codes, the
size, σ and the presets. The measured inks come from the ink table it
names (`panelgen/palette.py`).

Each generated file starts with the command above and a SHA-256 of the
generator's inputs: the ink table, the panel definition, the model
constants and the quantisation rules. The output is deterministic on one
platform, so to test reproducibility you regenerate and diff (`--check`). The
pytest suite also regenerates the files twice into temporary trees and compares
them byte for byte.

Across platforms the floating-point results differ slightly: on Linux and
Windows, 1 to 4 of EL133UF1's 360 `cusp_lstar` entries move by 1 (Q8, about
0.004 L\*), because `cusp_lut` takes a maximum along a nearly flat ridge. On
Windows the grey panel's header also differs (path separators, and the JSON's
hash after a CRLF checkout). The shipped tables were generated on macOS arm64,
and CI runs `--check` there. Regenerate on macOS, because committing the output
from another platform changes the bits sent to the panel.

Every generation runs these self-checks (`gen.check`):

- In double, `lin_to_panel_adapt·lin + B` reproduces `gamut.to_panel`, and
  the D blend reproduces `model.panel_partial`, both for scalar D and for
  per-pixel D.
- The folded OKLab matrices reproduce `Inks.to_space(…, "oklab")`.
- Every ink is inside the Q16 hull, tested in exact integer arithmetic.
  Random ink mixtures are inside too.
- The white ink maps to OKLab (1, 0, 0) through the Q16 tables, simulated
  in integers.
- The kernel is symmetric and sums to exactly 2^15.
- Every value fits its C type.
- L\*min has a monotone midtone knee, and every cusp is at least 0.5 L\* inside
  the neutral range (`gen.CUSP_MARGIN_Q8` derives the margin).
- The tables pass every check `epi_converter_init` makes of them
  (`gen.check_tables_valid`), and the presets every check `epi_intent_init`
  makes of them (`gen.check_presets`).

The last two depend on the panel's inks, σ and presets: generation prints the
panel and the reason, and exits with status 2.

## Grey calibrations

A grey panel can also ship as const calibrations, so firmware does not have to
call `epi_grey_panel_init`. panelgen builds the tables that
`epi_grey_panel_init` would build at runtime (`src/epi_grey_panel.c`), with the
same rules, from a levels JSON (a measured calibration; the format is below):

```sh
cd tools/panelgen && .venv/bin/python -m panelgen gdey075t7 --levels LEVELS.json          # write
cd tools/panelgen && .venv/bin/python -m panelgen gdey075t7 --levels LEVELS.json --check  # diff; exit 1 if stale
```

`--levels` defaults to the panel's shipped JSON (`levels` in
`panelgen/panels.py`), so regenerating the shipped file needs only
`panelgen gdey075t7`.

This writes `src/panels/gdey075t7.c`, which defines one `epi_panel_t` per
mode, named `epi_panel_<glass>_<mode>` (design.md "Grey panels"):
`epi_panel_gdey075t7_mono` and `epi_panel_gdey075t7_grey4`. The levels come
from the JSON entries `gdey075t7_mono` and `gdey075t7_grey4`: `perceived` is
the measured sRGB8 (darkest first), plus `nominal` and `codes`. The panel
definition in `panelgen/panels.py` (`GDEY075T7`) gives the rest: name
"GDEY075T7", 800x480 and σ 5.0. Its presets are the library's
`epi_grey_presets` (`src/epi_grey_panel.c`), which the generated file references
rather than defining its own.

The tables are computed in `panelgen/grey.py`, which mirrors the C rules
exactly:

- `level_y` is pure integer arithmetic from the Q16 sRGB tables (the same
  ints as `src/epi_srgb.c`).
- The thresholds use a line-by-line port of `epi_cbrt_d`, in the C's order
  of double operations. Python floats are IEEE doubles, and CPython does not
  fuse operations into FMAs.
- The kernel follows `gaussian_kernel`'s rule (`epi_exp_d`, sequential sum).
  Generation asserts that it equals panelgen's `kernel_q15`.
- The validity rules are `derive_levels`'s: 2..16 levels, σ in (0, 10.5], Y
  strictly increasing in Q16, and `epi_midtone_monotone` of the darkest level's
  L\*. A JSON the runtime would reject is an error, and nothing is written.

The generated header records:

- the command;
- an inputs hash;
- the levels JSON's path and SHA-256 (of its bytes);
- each mode's calibration metadata (format, date, waveform, chart, photo count,
  warnings).

The generator refuses a JSON that is marked as a placeholder (top-level
`_placeholder`/`placeholder`, or `placeholder: true` in a mode entry or its
`photocal` block) unless
`--allow-placeholder` is given. Output built that way says PLACEHOLDER in its
header and must not ship. A JSON that is not a placeholder must have a
`photocal` block per mode, with a `date` and a recorded `waveform` (not
`UNRECORDED`), because grey levels depend on the waveform (design.md "Grey
panels").

`tests/test_grey.py` is the gate. It generates into temporary trees from
`tests/grey-levels-placeholder.json` (guessed, not measured; with
`--allow-placeholder`), from random 2–16-level ramps and from edge cases (pure
black, codes out of order, levels 1 sRGB8 step apart). Each generated file is
compiled with `-Wall -Wextra -Wpedantic -Werror`, then linked with the library
sources (compiled with `-std=c11 -O2 -ffp-contract=off`, as CMake does; the
repo's CMake is not used) and with `tests/grey_equiv.c`. That program builds
each mode at runtime with `epi_grey_panel_init` from the same levels, name, size
and σ, and requires all of the following to be identical:

- every field of `epi_grey_tables_t`;
- the levels, name, size and σ bits;
- the presets;
- the converter's ink codes for every preset, and their measured preview.

Negative controls check that a 1-LSB change in a threshold, `level_y` or the
kernel, or a wrong preset count, is caught. The suite also checks the refusals,
that a rerun is byte-identical, and the `--check` exit codes.

### Adding a built-in grey calibration

`src/panels/gdey075t7.c` is generated from
`tools/panelgen/measurements/gdey075t7-levels-2026-10-31.json`. To recalibrate it,
for example after a waveform change, or to add another grey panel, see
[Adding a panel](../../docs/adding-panels.md).
