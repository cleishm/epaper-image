import numpy as np
import pytest

from panelgen.colour import xyz_to_lab
from panelgen.gamut import InkGamut
from panelgen.palette import Palette, builtin_palette, builtin_tables


def test_spectra6_default_is_photo_measured(spectra6):
    assert builtin_tables("spectra6") == ["photo"]
    photo = builtin_palette("spectra6", "photo")
    np.testing.assert_array_equal(photo.nominal, spectra6.nominal)
    np.testing.assert_array_equal(photo.measured, spectra6.measured)
    np.testing.assert_array_equal(
        spectra6.nominal, [(0, 0, 0), (255, 255, 255), (255, 255, 0), (255, 0, 0), (0, 0, 255), (0, 255, 0)])
    w = InkGamut(spectra6.measured_xyz).white
    lab = xyz_to_lab(spectra6.measured_xyz, w)
    k = spectra6.measured_xyz[0]
    assert abs(k[1] / w[1] - 0.084) < 0.001  # photo Y / Y_white kept
    assert np.hypot(lab[0, 1], lab[0, 2]) < 0.5  # neutralised (8-bit rounding only)
    np.testing.assert_array_equal(spectra6.measured[1], (190, 200, 200))


def test_unknown_builtin_table():
    assert builtin_tables("grayscale16") == []
    with pytest.raises(KeyError):
        builtin_palette("spectra6", "nope")
    with pytest.raises(KeyError):
        builtin_palette("grayscale16")


def test_duplicate_nominal_rejected():
    rgb = np.array([[0, 0, 0], [0, 0, 0]], np.uint8)
    with pytest.raises(ValueError, match="duplicate nominal"):
        Palette("x", rgb, rgb, "test")
