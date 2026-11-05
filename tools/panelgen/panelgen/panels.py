"""Panel definitions: what panelgen needs beyond the ink table.

The measured ink colours are NOT repeated here: they come from the ink
table named by `table` (palette.py), in its ink index
order. This file adds what the ink table does not carry: panel codes, native
size, sigma and the preset list (docs/design.md, "Panel tables").
"""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class Preset:
    name: str
    tone: float
    detail: float
    adapt: float
    vibrance: float = 1.0
    veil: float = 0.0
    lift: float = 0.0


@dataclass(frozen=True)
class Panel:
    ident: str  # C identifier suffix: epi_panel_<ident>
    name: str  # epi_panel_t.name
    palette: str  # palette name (palette.py BUILTIN_TABLES)
    table: str  # ink table name (the measured inks)
    width: int
    height: int
    ink_names: tuple[str, ...]  # ink table index order (checked against the table)
    codes: tuple[int, ...]  # panel code per ink, same order
    detail_sigma: float  # panel px
    presets: tuple[Preset, ...]  # [0] is the default
    doc: str  # one-line comment for the generated file


EL133UF1 = Panel(
    ident="el133uf1",
    name="EL133UF1",
    palette="spectra6",
    table="photo",
    width=1200,
    height=1600,
    ink_names=("black", "white", "yellow", "red", "blue", "green"),
    codes=(0, 1, 2, 3, 5, 6),  # code 4 is unused on Spectra 6
    detail_sigma=6.0,
    presets=(
        Preset("Default", tone=0.5, detail=0.75, adapt=0.5),
        Preset("Soft", tone=0.0, detail=0.75, adapt=0.5),
        Preset("Deep", tone=1.0, detail=0.75, adapt=0.5),
        Preset("Vivid", tone=0.5, detail=0.75, adapt=0.5, vibrance=1.3),
    ),
    doc='E Ink EL133UF1, Spectra 6, 13.3" 1200x1600 (reTerminal E1004)',
)

PANELS = {p.ident: p for p in (EL133UF1,)}


# -- grey glass --------------------------------------------------------------------
#
# A grey calibration's levels are not in palette.py: they come from a
# measured levels JSON (panelgen/grey.py), passed with --levels. One
# GreyPanel is one glass and one generated file, holding a calibration per
# mode (epi_panel_<ident>_<mode>, design.md "Grey panels"). Its presets are
# the library's epi_grey_presets (src/epi_grey_panel.c).


@dataclass(frozen=True)
class GreyPanel:
    ident: str  # file src/panels/<ident>.c; JSON keys and C names <ident>_<mode>
    name: str  # epi_panel_t.name
    modes: tuple[str, ...]  # emitted in this order
    width: int
    height: int
    detail_sigma: float  # panel px
    doc: str
    # Repo-relative levels JSON of the shipped calibration: the default for
    # --levels, and what tests/test_grey.py --checks the committed file
    # against. None until the calibration ships.
    levels: str | None = None


GDEY075T7 = GreyPanel(
    ident="gdey075t7",
    name="GDEY075T7",
    modes=("mono", "grey4"),
    width=800,
    height=480,
    detail_sigma=5.0,
    doc='7.5" 800x480 grey glass, UC8179 controller',
    levels="tools/panelgen/measurements/gdey075t7-levels-2026-10-31.json",
)

GREY_PANELS = {p.ident: p for p in (GDEY075T7,)}
