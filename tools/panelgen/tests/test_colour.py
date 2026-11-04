import pytest

from panelgen.colour import srgb_to_linear


def test_srgb_to_linear():
    assert srgb_to_linear(0) == 0.0
    assert srgb_to_linear(255) == pytest.approx(1.0, abs=1e-12)
    assert srgb_to_linear(10) == pytest.approx(10 / 255 / 12.92, abs=1e-12)  # the linear segment
    assert srgb_to_linear(128) == pytest.approx(0.2158605, abs=1e-7)
