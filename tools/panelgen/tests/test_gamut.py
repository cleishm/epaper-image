import numpy as np
import pytest

from panelgen.colour import WHITE_D65, lab_to_lch, lch_to_lab, srgb8_to_xyz
from panelgen.gamut import HULL_TOL, InkGamut


def _sources(n=4000, seed=3):
    rng = np.random.default_rng(seed)
    rgb = rng.integers(0, 256, (n, 3), dtype=np.uint8)
    corners = np.array(
        [[r, g, b] for r in (0, 255) for g in (0, 255) for b in (0, 255)], dtype=np.uint8
    )
    return np.concatenate([corners, rgb])


def target(g, xyz):
    """The full media-relative target: BPC'd, then into the hull."""
    return g.map_into_hull(g.to_panel(xyz))


def test_inks_map_to_themselves(spectra6):
    g = InkGamut(spectra6.measured_xyz)
    np.testing.assert_allclose(g.map_into_hull(spectra6.measured_xyz), spectra6.measured_xyz, atol=1e-12)


def test_display_white_and_black_map_to_inks(spectra6):
    g = InkGamut(spectra6.measured_xyz)
    t = target(g, srgb8_to_xyz(np.array([[255, 255, 255], [0, 0, 0]], np.uint8)))
    np.testing.assert_allclose(t[0], g.white, atol=1e-6)
    np.testing.assert_allclose(t[1], g.black, atol=1e-6)
    np.testing.assert_allclose(g.to_panel(np.array([WHITE_D65, [0.0, 0, 0]])), [g.white, g.black], atol=1e-12)


def test_white_ink_is_lab_100_in_ink_relative_lab(spectra6):
    g = InkGamut(spectra6.measured_xyz)
    np.testing.assert_allclose(g.lab(g.white), [100.0, 0.0, 0.0], atol=1e-9)


def test_bradford_adapts_d65_to_ink_white_chromaticity(spectra6):
    g = InkGamut(spectra6.measured_xyz)
    np.testing.assert_allclose(g.adapt(WHITE_D65), g.white / g.white[1], atol=1e-12)
    # neutral sources become ink-relative neutral (a* = b* = 0)
    grey = srgb8_to_xyz(np.array([[64] * 3, [128] * 3, [200] * 3], np.uint8))
    np.testing.assert_allclose(g.lab(g.to_panel(grey))[:, 1:], 0.0, atol=0.5)


def test_source_black_maps_to_ink_black(spectra6):
    g = InkGamut(spectra6.measured_xyz)
    t = target(g, srgb8_to_xyz(np.array([0, 0, 0], np.uint8))[None])[0]
    np.testing.assert_allclose(t, spectra6.measured_xyz[0], atol=1e-12)


def test_in_gamut_colours_unchanged(spectra6):
    g = InkGamut(spectra6.measured_xyz)
    rng = np.random.default_rng(7)
    w = rng.dirichlet(np.ones(len(spectra6.measured)), size=2000)
    xyz = w @ spectra6.measured_xyz
    assert g.contains(xyz).all()
    out = g.map_into_hull(xyz)
    np.testing.assert_array_equal(out, xyz)
    np.testing.assert_array_equal(lab_to_lch(g.lab(out))[:, 1], lab_to_lch(g.lab(xyz))[:, 1])


def test_out_of_gamut_ends_inside_or_on_hull(spectra6):
    g = InkGamut(spectra6.measured_xyz)
    comp = g.to_panel(srgb8_to_xyz(_sources()))
    outside = ~g.contains(comp)
    assert outside.mean() > 0.3  # the test really exercises mapping
    out = g.map_into_hull(comp)
    assert (g.signed_distance(out) <= HULL_TOL).all()


def test_mapping_preserves_hue_and_is_maximal(spectra6):
    g = InkGamut(spectra6.measured_xyz)
    comp = g.to_panel(srgb8_to_xyz(_sources()))
    outside = ~g.contains(comp)
    lch_in = lab_to_lch(g.lab(comp[outside]))
    lch_out = lab_to_lch(g.lab(g.map_into_hull(comp[outside])))
    lmin, lmax = g.neutral_lstar
    # chroma only reduced
    assert (lch_out[:, 1] <= lch_in[:, 1] + 1e-9).all()
    # ink-relative L* kept unless clamped into the hull's neutral interval
    np.testing.assert_allclose(lch_out[:, 0], np.clip(lch_in[:, 0], lmin, lmax), atol=1e-9)
    # hue kept wherever hue is meaningful
    ok = lch_out[:, 1] > 1.0
    assert ok.mean() > 0.5
    dh = np.degrees(np.angle(np.exp(1j * (lch_out[ok, 2] - lch_in[ok, 2]))))
    assert np.abs(dh).max() < 1.0
    # maximal: a little more chroma would leave the hull (binary search found
    # the boundary instead of over-reducing)
    reduced = lch_out[:, 1] < lch_in[:, 1] - 1e-6
    bump = lch_out[reduced].copy()
    bump[:, 1] += 1e-3 * lch_in[reduced, 1] + 1e-4
    frac = (~g.contains(g.xyz(lch_to_lab(bump)))).mean()
    assert frac > 0.99


def test_spectra6_neutral_interval(spectra6):
    """In ink-relative Lab the white ink is the top of the neutral axis; the
    black ink is not exactly neutral there, so L*min may sit a little above it."""
    g = InkGamut(spectra6.measured_xyz)
    lab = g.lab(spectra6.measured_xyz)
    lmin, lmax = g.neutral_lstar
    assert lmax == pytest.approx(100.0, abs=1e-5)
    assert lmin >= lab[0, 0] - 1e-6 and lmin < lab[0, 0] + 2.0


def test_flat_ink_sets_rejected():
    grey = srgb8_to_xyz(np.array([[20 + 12 * i] * 3 for i in range(16)], np.uint8))
    with pytest.raises(NotImplementedError, match="rank 1"):
        InkGamut(grey)
