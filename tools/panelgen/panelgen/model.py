"""The colour model panelgen derives the colour tables from.

OKLab and the ink precomputation, the cusp table and partial adaptation,
The ink tables are palette.py's; InkGamut
(hull, Bradford/BPC) is gamut.py's; Lab and the matrices are colour.py's.
"""

from __future__ import annotations

from functools import lru_cache

import numpy as np

from . import PanelError
from .colour import lab_to_lch, lch_to_lab, xyz_to_linear_rgb
from .gamut import InkGamut
from .palette import builtin_palette, builtin_tables

# Ottosson's OKLab (linear sRGB -> LMS -> cbrt -> Lab).
OK_M1 = np.array([
    [0.4122214708, 0.5363325363, 0.0514459929],
    [0.2119034982, 0.6806995451, 0.1073969566],
    [0.0883024619, 0.2817188376, 0.6299787005],
])
OK_M2 = np.array([
    [0.2104542553, 0.7936177850, -0.0040720468],
    [1.9779984951, -2.4285922050, 0.4505937099],
    [0.0259040371, 0.7827717662, -0.8086757660],
])


# -- inks -------------------------------------------------------------------------

class Inks:
    """One built-in ink table (palette.py) and what panelgen derives from it: the
    ink hull (InkGamut), the inks in linear RGB, and ink-relative OKLab
    (white ink -> L = 1)."""

    def __init__(self, name: str, table: str):
        if table not in builtin_tables(name):
            raise PanelError(f"{name}: no built-in ink table {table!r}")
        self.name = name
        self.table = table
        self.palette = builtin_palette(name, table)
        self.gamut = InkGamut(self.palette.measured_xyz)
        self.xyz = self.palette.measured_xyz
        self.lin = xyz_to_linear_rgb(self.xyz)  # measured inks, linear RGB
        self.Y = self.xyz[:, 1]
        self.white_lin = self.lin[np.argmax(self.Y)]
        self._ok_scale = 1.0 / self.white_lin  # ink-relative: white ink -> (1,1,1)
        self.coords = {"oklab": self.to_space(self.lin, "oklab")}

    def to_space(self, v: np.ndarray, space: str) -> np.ndarray:
        if space == "oklab":
            lms = (v * self._ok_scale) @ OK_M1.T
            return np.cbrt(lms) @ OK_M2.T
        raise ValueError(space)


@lru_cache(maxsize=None)
def inks(name: str, table: str) -> Inks:
    return Inks(name, table)


# -- cusp -------------------------------------------------------------------------

CUSP_L_SAMPLES = 64
CUSP_REFINE = 17  # samples per refinement pass, between the best sample's neighbours
CUSP_REFINE_PASSES = 4  # each pass shrinks the L* step 8x (1.03 -> 2.5e-4 L*)
CUSP_C_PROBE = 200.0  # chroma certainly outside any ink hull (asserted)

_LUTS: dict[bytes, np.ndarray] = {}


def max_chroma(gamut: InkGamut, L: np.ndarray, h_deg: np.ndarray) -> np.ndarray:
    """In-hull chroma limit at ink-relative (L*, hue in degrees): InkGamut's
    constant-L* clip of a far-out probe. L must lie in the neutral interval."""
    L, h = np.broadcast_arrays(np.asarray(L, float), np.radians(np.asarray(h_deg, float)))
    probe = gamut.xyz(lch_to_lab(np.stack([L, np.full_like(L, CUSP_C_PROBE), h], axis=-1)))
    if gamut.contains(probe).any():
        raise PanelError(f"chroma probe {CUSP_C_PROBE} lies inside the hull")
    return lab_to_lch(gamut.lab(gamut.map_into_hull(probe)))[..., 1]


def cusp_lut(gamut: InkGamut) -> np.ndarray:
    """(360, 2) array: (L*, C*) of the max-chroma in-hull point at hue 0..359 deg."""
    key = gamut.points.tobytes()
    if key in _LUTS:
        return _LUTS[key]
    lmin, lmax = gamut.neutral_lstar
    hues = np.arange(360.0)
    Ls = np.linspace(lmin, lmax, CUSP_L_SAMPLES)
    C = max_chroma(gamut, Ls[None, :], hues[:, None])  # (360, n)
    rows = np.arange(360)
    best_L, best_C = Ls[np.argmax(C, axis=1)], C.max(axis=1)
    step = Ls[1] - Ls[0]
    for _ in range(CUSP_REFINE_PASSES):  # the ridge has a kink at the cusp: zoom in
        Lf = np.clip(best_L[:, None] + step * np.linspace(-1.0, 1.0, CUSP_REFINE)[None, :], lmin, lmax)
        Cf = max_chroma(gamut, Lf, hues[:, None])
        j = np.argmax(Cf, axis=1)
        best_L, best_C = Lf[rows, j], Cf[rows, j]
        step *= 2.0 / (CUSP_REFINE - 1)
    lut = np.stack([best_L, best_C], axis=-1)
    _LUTS[key] = lut
    return lut


# -- partial adaptation -----------------------------------------------------------

def panel_partial(gamut: InkGamut, src: np.ndarray, D: float) -> np.ndarray:
    """Panel XYZ with adaptation degree D: D to_panel(src) + (1 - D) (src
    (W_y - B_y) + B_ink), the second term being BPC in Y with no chromatic
    adaptation. D = 1 is exactly gamut.to_panel."""
    if np.isscalar(D) and D == 1.0:
        return gamut.to_panel(src)
    unadapted = src * (gamut.white[1] - gamut.black[1]) + gamut.black
    return D * gamut.to_panel(src) + (1.0 - D) * unadapted
