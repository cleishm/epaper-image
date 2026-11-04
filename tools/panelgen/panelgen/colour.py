"""The sRGB constants behind src/epi_srgb.c (IEC 61966-2-1:1999): the transfer
function, and the 4-decimal primary matrix from the standard."""

from __future__ import annotations

# Linear sRGB -> CIE XYZ (D65).
M_RGB_TO_XYZ = [
    [0.4124, 0.3576, 0.1805],
    [0.2126, 0.7152, 0.0722],
    [0.0193, 0.1192, 0.9505],
]


def srgb_to_linear(u: int) -> float:
    """Decode an sRGB8 value."""
    c = u / 255.0
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4
