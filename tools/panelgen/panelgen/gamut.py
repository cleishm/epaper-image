"""Media-relative gamut mapping onto a colour panel's measured inks.

The viewer adapts to the panel's white, as in ICC media-relative
colorimetry:

1. Source sRGB -> XYZ (D65, display white Y = 1).
2. Chromatic adaptation (Bradford) from D65 to the chromaticity of the white
   ink (destination white = W_ink / Y(W_ink), so Y of white stays 1).
3. Black-point compensation (Adobe style), per XYZ component:
       XYZ' = (XYZ - B_src) * (W_ink - B_ink) / (W_src - B_src) + B_ink
   with B_src = 0 and W_src = the adapted display white. Display white lands
   exactly on the white ink, display black exactly on the black ink.
4. Pixels outside the convex hull of the measured inks' XYZ have their
   chroma reduced toward the L* axis at constant L* and hue, in INK-RELATIVE
   CIELAB (reference white Xn = W_ink, so the white ink is L* 100,
   a* = b* = 0): binary search (20 iterations) on a chroma scale s in
   [0, 1]; the kept value is always the inside end of the bracket, so the
   result is inside or on the hull.
5. Before step 4, if the neutral point (a* = b* = 0) at the pixel's L* is
   itself outside the hull, L* is clamped into the hull's neutral interval
   [L*min, L*max] (the black ink need not be exactly neutral in ink-relative
   Lab, so L*min can sit slightly above the black ink's L*). Only out-of-hull
   pixels are touched.

W_ink / B_ink are the lightest / darkest measured inks (by Y).

The hull is the right reproducible gamut because a dithered area, seen from
a distance, is a linear (area-weighted) mix of the inks' light: exactly the
convex combinations of the measured XYZ points. Only full 3-D hulls are
supported (colour panels); grey panels do not use this module.
"""

from __future__ import annotations

import numpy as np
from scipy.spatial import ConvexHull

from . import PanelError
from .colour import (
    WHITE_D65,
    bradford_matrix,
    lab_to_lch,
    lab_to_xyz,
    lch_to_lab,
    xyz_to_lab,
    y_to_lstar,
)

# Containment tolerance in XYZ units (Y of display white = 1).
HULL_TOL = 1e-7
BISECT_ITERS = 20


class InkGamut:
    def __init__(self, ink_xyz: np.ndarray):
        pts = np.asarray(ink_xyz, dtype=np.float64)
        self.points = pts
        centred = pts - pts.mean(axis=0)
        sv = np.linalg.svd(centred, compute_uv=False)
        scale = max(sv[0], 1e-12)
        self.rank = int(np.sum(sv > 1e-9 * max(1.0, scale)))
        if self.rank == 3:
            hull = ConvexHull(pts)
            self.equations = hull.equations  # unit normals: n.x + d <= 0 inside
        else:
            raise NotImplementedError(
                f"ink set of rank {self.rank} (collinear, coplanar or single point) is not supported"
            )
        self.black = pts[np.argmin(pts[:, 1])].copy()  # B_ink
        self.white = pts[np.argmax(pts[:, 1])].copy()  # W_ink, also the Lab white
        self._neutral_dir = self.white / self.white[1]
        # Step 2: D65 -> white-ink chromaticity (both whites at Y = 1).
        self.adapt_matrix = bradford_matrix(WHITE_D65, self._neutral_dir)
        self.adapted_display_white = self.adapt_matrix @ WHITE_D65
        self.neutral_y = self._neutral_interval()
        self.neutral_lstar = tuple(
            float(v) for v in y_to_lstar(np.array(self.neutral_y), self.white[1])
        )

    # -- ink-relative CIELAB --------------------------------------------
    def lab(self, xyz: np.ndarray) -> np.ndarray:
        """CIELAB relative to the white ink (white ink = (100, 0, 0))."""
        return xyz_to_lab(xyz, self.white)

    def xyz(self, lab: np.ndarray) -> np.ndarray:
        return lab_to_xyz(lab, self.white)

    # -- containment -----------------------------------------------------
    def signed_distance(self, xyz: np.ndarray) -> np.ndarray:
        """Max over faces of n.x + d (<= 0 inside)."""
        xyz = np.asarray(xyz, dtype=np.float64)
        n = self.equations[:, :3]
        d = self.equations[:, 3]
        return (xyz @ n.T + d).max(axis=-1)

    def contains(self, xyz: np.ndarray, tol: float = HULL_TOL) -> np.ndarray:
        return self.signed_distance(xyz) <= tol

    def _neutral_interval(self) -> tuple[float, float]:
        """[Ymin, Ymax] of the ink-relative neutral ray Y * W_ink / Y(W_ink)
        inside the hull."""
        n = self.equations[:, :3]
        d = self.equations[:, 3]
        k = n @ self._neutral_dir  # constraint: Y * k + d <= tol
        lo, hi = 0.0, np.inf
        for ki, di in zip(k, d):
            if ki > 0:
                hi = min(hi, -di / ki)
            elif ki < 0:
                lo = max(lo, -di / ki)
            elif di > HULL_TOL:
                lo, hi = 1.0, 0.0
        if not lo <= hi:
            raise PanelError("the ink hull does not intersect the white ink's neutral axis")
        eps = 1e-9  # shrink a hair so the end points test as inside
        return (lo + eps, hi - eps)

    # -- mapping ---------------------------------------------------------
    def adapt(self, xyz: np.ndarray) -> np.ndarray:
        """Step 2: Bradford D65 -> white-ink chromaticity."""
        return np.asarray(xyz, dtype=np.float64) @ self.adapt_matrix.T

    def black_point_compensate(self, xyz_adapted: np.ndarray) -> np.ndarray:
        """Step 3: per-component BPC, display [0, W_src] -> [B_ink, W_ink]."""
        scale = (self.white - self.black) / self.adapted_display_white
        return np.asarray(xyz_adapted, dtype=np.float64) * scale + self.black

    def to_panel(self, source_xyz: np.ndarray) -> np.ndarray:
        """Steps 2-3 (before gamut clipping)."""
        return self.black_point_compensate(self.adapt(source_xyz))

    def map_into_hull(self, xyz: np.ndarray) -> np.ndarray:
        """Steps 4-5: chroma reduction at constant ink-relative L*, hue (L*
        clamp first when the neutral point is outside). In-hull colours are
        untouched."""
        xyz = np.asarray(xyz, dtype=np.float64)
        shape = xyz.shape
        flat = xyz.reshape(-1, 3)
        out = flat.copy()
        outside = ~self.contains(flat)
        if not outside.any():
            return out.reshape(shape)
        lch = lab_to_lch(self.lab(flat[outside]))
        lmin, lmax = self.neutral_lstar
        L = np.clip(lch[:, 0], lmin, lmax)
        C = lch[:, 1]
        h = lch[:, 2]

        def at(s):
            return self.xyz(lch_to_lab(np.stack([L, s * C, h], axis=-1)))

        lo = np.zeros_like(C)
        hi = np.ones_like(C)
        full_inside = self.contains(at(hi))  # only possible when L* was clamped
        for _ in range(BISECT_ITERS):
            mid = 0.5 * (lo + hi)
            ins = self.contains(at(mid))
            lo = np.where(ins, mid, lo)
            hi = np.where(ins, hi, mid)
        lo[full_inside] = 1.0
        out[outside] = at(lo)
        return out.reshape(shape)
