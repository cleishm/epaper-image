"""Colour maths: sRGB -> linear -> XYZ (D65) <-> CIELAB, and Bradford.

Everything is float64 and vectorised over a trailing axis of length 3.

References
----------
* sRGB: IEC 61966-2-1:1999. Transfer function and the 4-decimal primary
  matrix from the standard. The D65 reference white is taken as the matrix
  row sums (0.9505, 1.0000, 1.0890), so sRGB white maps to a* = b* = 0
  exactly.
* CIELAB: CIE 15:2004, with the D65 white above.
* Bradford chromatic adaptation: Lam (1985); the linearised form ICC v4
  recommends for its chromatic-adaptation tag.
"""

from __future__ import annotations

import numpy as np

# IEC 61966-2-1 linear sRGB -> CIE XYZ (D65), 4-decimal form from the standard.
M_RGB_TO_XYZ = np.array(
    [
        [0.4124, 0.3576, 0.1805],
        [0.2126, 0.7152, 0.0722],
        [0.0193, 0.1192, 0.9505],
    ]
)
M_XYZ_TO_RGB = np.linalg.inv(M_RGB_TO_XYZ)

# D65 reference white (Y = 1), consistent with the matrix above.
WHITE_D65 = M_RGB_TO_XYZ.sum(axis=1)

_DELTA = 6.0 / 29.0


def srgb_to_linear(c):
    """Decode sRGB values. uint8 input is scaled from [0, 255]; float input is
    taken as already in [0, 1]."""
    c = np.asarray(c)
    if c.dtype == np.uint8:
        c = c.astype(np.float64) / 255.0
    else:
        c = c.astype(np.float64)
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def srgb8_to_xyz(rgb8):
    """uint8 sRGB (..., 3) -> XYZ (..., 3), Y of sRGB white = 1."""
    return srgb_to_linear(rgb8) @ M_RGB_TO_XYZ.T


def xyz_to_linear_rgb(xyz):
    return np.asarray(xyz, dtype=np.float64) @ M_XYZ_TO_RGB.T


def _f(t):
    return np.where(t > _DELTA**3, np.cbrt(t), t / (3 * _DELTA**2) + 4.0 / 29.0)


def _finv(f):
    return np.where(f > _DELTA, f**3, 3 * _DELTA**2 * (f - 4.0 / 29.0))


def xyz_to_lab(xyz, white=WHITE_D65):
    xyz = np.asarray(xyz, dtype=np.float64)
    fx = _f(xyz[..., 0] / white[0])
    fy = _f(xyz[..., 1] / white[1])
    fz = _f(xyz[..., 2] / white[2])
    return np.stack([116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz)], axis=-1)


def lab_to_xyz(lab, white=WHITE_D65):
    lab = np.asarray(lab, dtype=np.float64)
    fy = (lab[..., 0] + 16.0) / 116.0
    fx = fy + lab[..., 1] / 500.0
    fz = fy - lab[..., 2] / 200.0
    return np.stack(
        [white[0] * _finv(fx), white[1] * _finv(fy), white[2] * _finv(fz)], axis=-1
    )


def y_to_lstar(y, white_y=1.0):
    return 116.0 * _f(np.asarray(y, dtype=np.float64) / white_y) - 16.0


def lab_to_lch(lab):
    lab = np.asarray(lab, dtype=np.float64)
    c = np.hypot(lab[..., 1], lab[..., 2])
    h = np.arctan2(lab[..., 2], lab[..., 1])  # radians
    return np.stack([lab[..., 0], c, h], axis=-1)


def lch_to_lab(lch):
    lch = np.asarray(lch, dtype=np.float64)
    return np.stack(
        [lch[..., 0], lch[..., 1] * np.cos(lch[..., 2]), lch[..., 1] * np.sin(lch[..., 2])],
        axis=-1,
    )


# Bradford cone-response matrix (Lam 1985), linearised Bradford.
M_BRADFORD = np.array(
    [
        [0.8951, 0.2664, -0.1614],
        [-0.7502, 1.7135, 0.0367],
        [0.0389, -0.0685, 1.0296],
    ]
)


def bradford_matrix(src_white, dst_white):
    """3x3 matrix adapting XYZ from src_white to dst_white (Bradford, linear
    von Kries in the Bradford cone space). Maps src_white exactly onto
    dst_white, so pass both whites at the same Y to change chromaticity only."""
    rs = M_BRADFORD @ np.asarray(src_white, dtype=np.float64)
    rd = M_BRADFORD @ np.asarray(dst_white, dtype=np.float64)
    return np.linalg.inv(M_BRADFORD) @ np.diag(rd / rs) @ M_BRADFORD
