"""Ink tables: each ink's nominal colour and its MEASURED ("perceived")
colour, i.e. what the panel shows. panels.py names a table per colour panel.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np

from . import PanelError
from .colour import srgb8_to_xyz

# Photo-measured spectra6 panel (reTerminal E1004, 13.3"). Provenance:
# iPhone 16 Pro ProRAW close-ups IMG_3544-3547 of the chart-1 screen,
# 2026-10-28, registered to panel pixels and flat-fielded; each ink is the
# mean over solid single-ink pixels (5x5-solid in the ink map) of the four
# close-ups, converted with the DNG's own camera matrix, its Lab relative to
# the white ink kept and re-expressed against a white of (190, 200, 200).
# A camera is not a colorimeter: the luminance ratios (Y / Y_white: black
# 0.084, yellow 0.877, red 0.138, blue 0.263, green 0.316) are more
# trustworthy than the chroma. Black is NEUTRALISED: the photo's Y / Y_white
# of 0.084 is kept, its chroma set to 0 relative to white (the photo's
# (63, 59, 80) blue tint is judged a camera artefact). Black is
# surround-dependent on the panel (~0.06 of white beside white patches,
# ~0.11 in a dark ramp); 0.084 is the pooled mean.
SPECTRA6_PHOTO = {
    "black": ((0, 0, 0), (59, 62, 62)),
    "white": ((255, 255, 255), (190, 200, 200)),
    "yellow": ((255, 255, 0), (203, 189, 66)),
    "red": ((255, 0, 0), (137, 50, 41)),
    "blue": ((0, 0, 255), (60, 108, 177)),
    "green": ((0, 255, 0), (87, 125, 107)),
}
# palette -> {table name: (entries, source label)}; the first is the default.
BUILTIN_TABLES = {
    "spectra6": {
        "photo": (SPECTRA6_PHOTO, "built-in photo-measured (ProRAW 2026-10-28, neutral black)"),
    },
}


@dataclass(frozen=True, eq=False)
class Palette:
    name: str
    nominal: np.ndarray  # (n, 3) uint8
    measured: np.ndarray  # (n, 3) uint8
    source: str

    def __post_init__(self):
        keys = _keys(self.nominal)
        if len(np.unique(keys)) != len(keys):
            raise PanelError(f"palette {self.name}: duplicate nominal colours ({self.source})")

    @property
    def measured_xyz(self) -> np.ndarray:
        return srgb8_to_xyz(self.measured)


def _keys(rgb) -> np.ndarray:
    rgb = np.asarray(rgb).astype(np.int64)
    return (rgb[..., 0] << 16) | (rgb[..., 1] << 8) | rgb[..., 2]


def builtin_tables(name: str) -> list[str]:
    """Names of the built-in tables for palette `name` (default first)."""
    return list(BUILTIN_TABLES.get(name, {}))


def builtin_palette(name: str, table: str | None = None) -> Palette:
    """A built-in table for `name`; `table` None = its default."""
    tables = BUILTIN_TABLES.get(name)
    if not tables:
        raise KeyError(f"palette {name!r} has no built-in tables")
    table = table or next(iter(tables))
    if table not in tables:
        raise KeyError(f"palette {name!r} has no built-in table {table!r}; known: {', '.join(tables)}")
    entries, label = tables[table]
    nom = np.array([v[0] for v in entries.values()], dtype=np.uint8)
    per = np.array([v[1] for v in entries.values()], dtype=np.uint8)
    return Palette(name, nom, per, label)
