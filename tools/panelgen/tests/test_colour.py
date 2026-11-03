import numpy as np
import pytest

from panelgen.colour import (
    WHITE_D65,
    lab_to_xyz,
    srgb8_to_xyz,
    srgb_to_linear,
    xyz_to_lab,
)


def lab8(rgb):
    return xyz_to_lab(srgb8_to_xyz(np.array(rgb, dtype=np.uint8)))


def test_white_is_L100():
    np.testing.assert_allclose(lab8([255, 255, 255]), [100.0, 0.0, 0.0], atol=1e-9)


def test_black_is_L0():
    np.testing.assert_allclose(lab8([0, 0, 0]), [0.0, 0.0, 0.0], atol=1e-12)


def test_red_reference():
    # Common reference value (53.24, 80.09, 67.20); the IEC 4-decimal matrix
    # gives (53.233, 80.105, 67.223), within 0.05.
    np.testing.assert_allclose(lab8([255, 0, 0]), [53.24, 80.09, 67.20], atol=0.05)


def test_mid_grey_reference():
    # sRGB 128 -> linear 0.2158605 -> L* 53.585
    assert srgb_to_linear(np.uint8(128)) == pytest.approx(0.2158605, abs=1e-7)
    np.testing.assert_allclose(lab8([128, 128, 128]), [53.585, 0, 0], atol=2e-3)


def test_white_point_is_d65():
    np.testing.assert_allclose(WHITE_D65, [0.9505, 1.0, 1.0890], atol=1e-12)


def test_lab_xyz_roundtrip():
    rng = np.random.default_rng(1)
    xyz = rng.random((1000, 3)) * WHITE_D65
    xyz[:10] *= 1e-4  # exercise the linear segment
    np.testing.assert_allclose(lab_to_xyz(xyz_to_lab(xyz)), xyz, atol=1e-12)
