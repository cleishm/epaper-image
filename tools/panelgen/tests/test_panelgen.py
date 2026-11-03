"""panelgen self-checks: the derived tables against the model and fixtures, the
integer hull and OKLab paths, the kernel, and byte-identical regeneration."""

import re
import shutil
import subprocess
import sys
from dataclasses import replace

import numpy as np
import pytest

from panelgen import PANELGEN_DIR, REPO, PanelError
from panelgen.__main__ import main
from panelgen.gen import (CUSP_MARGIN_Q8, check, check_cusps, check_midtone_knee, check_presets, derive, kernel_q15,
                          outputs, q, render_srgb)
from panelgen.colour import M_RGB_TO_XYZ
from panelgen.model import inks, panel_partial
from panelgen.palette import BUILTIN_TABLES
from panelgen.panels import EL133UF1, PANELS, Panel, Preset

# Fixtures: values panel_partial and inks("spectra6", "photo").gamut gave
# before their code moved into panelgen/model.py, exact to the last bit
# (repr round-trips).
PHOTO_BLACK = [0.04395758468206505, 0.04722856161628528, 0.0523734864401632]
PHOTO_NEUTRAL_LSTAR = (35.139799798016554, 99.99999993147343)
PARTIAL = {  # (linear sRGB, D) -> panel_partial(gamut, M_RGB_TO_XYZ @ lin, D)
    ((0.2, 0.5, 0.9), 0.0): [0.2630386103263167, 0.28769910896280654, 0.5274773149978752],
    ((0.2, 0.5, 0.9), 0.5): [0.26110419786313466, 0.2881267697605968, 0.5326587132411023],
    ((0.2, 0.5, 0.9), 1.0): [0.25916978539995267, 0.28855443055838714, 0.5378401114843294],
    ((1.0, 1.0, 1.0), 0.0): [0.5353943837637002, 0.5642583344533554, 0.6154189090597324],
    ((1.0, 1.0, 1.0), 0.5): [0.5292712319800863, 0.5642583344533554, 0.6215973085645483],
    ((1.0, 1.0, 1.0), 1.0): [0.5231480801964723, 0.5642583344533554, 0.6277757080693642],
    ((0.0, 0.0, 0.0), 0.0): PHOTO_BLACK,
    ((0.0, 0.0, 0.0), 0.5): PHOTO_BLACK,
    ((0.0, 0.0, 0.0), 1.0): PHOTO_BLACK,
    ((0.7, 0.1, 0.05), 0.0): [0.21636891788117862, 0.16301737924314713, 0.09009339351749165],
    ((0.7, 0.1, 0.05), 0.5): [0.21399237675696306, 0.162343877138972, 0.09053450152015491],
    ((0.7, 0.1, 0.05), 1.0): [0.2116158356327475, 0.16167037503479686, 0.09097560952281815],
}


@pytest.fixture(scope="module")
def t():
    return derive(EL133UF1)


def test_q_rounds_half_away_from_zero():
    assert [q(v, 0) for v in (0.5, 1.5, 2.5, -0.5, -1.5, -2.5, 0.49, -0.49)] == [1, 2, 3, -1, -2, -3, 0, 0]
    assert q(1.0, 16) == 65536 and q(-1.0 / 65536 * 0.5, 16) == -1


@pytest.mark.parametrize("seed", [1, 2, 3])
def test_matrices_reproduce_model(t, seed):
    d = check(t, seed=seed)  # asserts inside; values recorded for the report
    assert d["err_adapt_double"] < 1e-12 and d["err_blend_double"] < 1e-12
    assert d["err_oklab_fold_double"] < 1e-12
    assert d["err_adapt_q16"] < 4 * 2.0**-16


def test_partial_adaptation_spot_values(t):
    g = inks("spectra6", "photo").gamut
    assert g.black.tolist() == PHOTO_BLACK
    for (rgb, D), want in PARTIAL.items():
        lin = np.array(rgb)
        np.testing.assert_allclose(panel_partial(g, M_RGB_TO_XYZ @ lin, D), want, rtol=0, atol=1e-14)
        m = D * t.real["lin_to_panel_adapt"] + (1 - D) * t.real["lin_to_panel_bpc"]
        np.testing.assert_allclose(m @ lin + g.black, want, rtol=0, atol=1e-14)


def test_every_ink_inside_q16_hull(t):
    I = t.ints
    assert I["hull_face_count"] == 8
    for xi in t.diag["ink_xyz_q16"]:
        for nf, df in zip(I["hull_n"], I["hull_d"]):
            assert sum(a * b for a, b in zip(nf, xi)) + df <= 0
    # each ink is a vertex: it lies on some face to within the Q16 rounding of x
    for xi in t.diag["ink_xyz_q16"]:
        slack = min(-(sum(a * b for a, b in zip(nf, xi)) + df) for nf, df in zip(I["hull_n"], I["hull_d"]))
        assert slack < 2**17


def test_source_white_lands_exactly_on_white_ink(t):
    I = t.ints
    for row, w, b in zip(I["lin_to_panel_adapt"], I["white_xyz"], I["black_xyz"]):
        assert sum(row) + b == w


def test_white_ink_oklab_is_one_zero_zero(t):
    assert t.ints["ink_oklab"][t.real["white_index"]] == [65536, 0, 0]
    assert max(abs(v) for v in t.diag["white_oklab_lsb_err"]) <= 4


def test_neutral_interval_and_cusp(t):
    g = inks("spectra6", "photo").gamut
    np.testing.assert_allclose(g.neutral_lstar, PHOTO_NEUTRAL_LSTAR, rtol=0, atol=1e-12)
    lmin, lmax = PHOTO_NEUTRAL_LSTAR
    assert t.ints["lstar_min"] == q(lmin, 8) and t.ints["lstar_max"] == q(lmax, 8)
    assert len(t.ints["cusp_lstar"]) == 360
    lo, hi = t.ints["lstar_min"] + CUSP_MARGIN_Q8, t.ints["lstar_max"] - CUSP_MARGIN_Q8
    assert all(lo <= v <= hi for v in t.ints["cusp_lstar"])


def test_midtone_knee_check():
    check_midtone_knee("x", 66 * 256)
    for bad in (67 * 256, 100 * 256, -1):
        with pytest.raises(PanelError, match=r"x: .*monotone midtone knee"):
            check_midtone_knee("x", bad)


def test_cusp_check():
    lmin, lmax = 9000, 25600
    ok = lmin + CUSP_MARGIN_Q8
    check_cusps("x", lmin, lmax, [ok, lmax - CUSP_MARGIN_Q8] * 180)
    for bad in (lmin, lmin + CUSP_MARGIN_Q8 - 1, lmax - CUSP_MARGIN_Q8 + 1, lmax, 0, 65535):
        with pytest.raises(PanelError, match=r"x: cusp L\* .* at hue 359 "):
            check_cusps("x", lmin, lmax, [ok] * 359 + [bad])
    with pytest.raises(PanelError, match="cannot hold cusps"):
        check_cusps("x", lmin, lmin + 2 * CUSP_MARGIN_Q8 - 1, [lmin + CUSP_MARGIN_Q8] * 360)
    with pytest.raises(PanelError, match="cannot hold cusps"):
        check_cusps("x", lmin, 100 * 256 + 1, [ok] * 360)


def test_preset_check():
    check_presets(EL133UF1)
    limits = dict(tone=0.5, detail=0.75, adapt=0.5)
    check_presets(replace(EL133UF1, presets=(Preset("x", **{**limits, "tone": 1.0}, vibrance=3.0, veil=3.0),)))
    for knob, bad in (("tone", 1.5), ("detail", -0.1), ("adapt", 1.1), ("vibrance", 3.1), ("veil", float("nan"))):
        panel = replace(EL133UF1, presets=(Preset("x", **{**limits, knob: bad}),))
        with pytest.raises(PanelError, match=f"{EL133UF1.name}: preset x {knob} "):
            check_presets(panel)
    with pytest.raises(PanelError, match="no presets"):
        check_presets(replace(EL133UF1, presets=()))


def test_derived_tables_are_checked(t):
    white = t.ints["white_xyz"]
    for key, bad, message in (("lstar_min", 67 * 256, "monotone midtone knee"),
                              ("cusp_lstar", [t.ints["lstar_min"]] * 360, "cusp L"),
                              ("lstar_min", 0, "above 0"),
                              ("black_xyz", [0, -1, 0], "black ink Y"),
                              ("black_xyz", white, "black ink Y"),
                              ("white_xyz", [512, white[1], white[2]], "above 512"),
                              ("ink_count", 17, "inks 17"),
                              ("hull_face_count", 33, "hull faces 33"),
                              ("kernel_radius", 33, "kernel radius 33")):
        kept = t.ints[key]
        t.ints[key] = bad
        try:
            with pytest.raises(PanelError, match=f"{EL133UF1.name}: .*{message}"):
                check(t)
        finally:
            t.ints[key] = kept


def _panel_with_inks(monkeypatch, table, entries):
    monkeypatch.setitem(BUILTIN_TABLES, "bad", {table: (entries, "test")})
    panel = Panel(ident="bad", name="BAD", palette="bad", table=table, width=8, height=8,
                  ink_names=tuple(entries), codes=tuple(range(len(entries))), detail_sigma=1.0,
                  presets=(Preset("Default", tone=0.5, detail=0.75, adapt=0.5),), doc="bad inks")
    monkeypatch.setattr("panelgen.__main__.PANELS", {"bad": panel})
    return panel


@pytest.mark.parametrize("table, entries, message", [
    # Black so light that the midtone curve has no knee.
    ("light", {"black": ((0, 0, 0), (170, 170, 170)), "white": ((255, 255, 255), (240, 240, 240)),
               "red": ((255, 0, 0), (235, 170, 170)), "blue": ((0, 0, 255), (170, 180, 240))},
     "monotone midtone knee"),
    # A strongly tinted black: the neutral axis enters the hull above it, so hues
    # towards it peak in chroma at the very bottom of the neutral range.
    ("tinted", {"black": ((0, 0, 0), (0, 0, 120)), "white": ((255, 255, 255), (240, 240, 240)),
                "yellow": ((255, 255, 0), (230, 220, 40)), "red": ((255, 0, 0), (200, 30, 30)),
                "green": ((0, 255, 0), (30, 170, 40))},
     "not 0.5 L\\* inside"),
])
def test_bad_inks_fail_generation(monkeypatch, capsys, tmp_path, table, entries, message):
    panel = _panel_with_inks(monkeypatch, table, entries)
    with pytest.raises(PanelError, match=message):
        derive(panel)
    assert main(["--root", str(tmp_path)]) == 2
    assert re.search(message, capsys.readouterr().err)
    assert not list(tmp_path.iterdir())


def test_generator_bugs_are_not_reported_as_bad_inks(monkeypatch):
    def bug(_panels):
        raise ValueError("a bug")
    monkeypatch.setattr("panelgen.__main__.outputs", bug)
    with pytest.raises(ValueError, match="a bug"):
        main([])


def _cusp_move_arithmetic(lmin, lmax, lc, l, c):
    """cusp_move's anchor arithmetic in exact integers: (k, e1 * e1, 2 * k * |ld|)."""
    span = lmax - lmin
    ld = l - lc
    k = 2 * (lmax - lc if ld > 0 else lc - lmin)
    ald = abs(ld)
    num, den = c * span, 20 * k
    e1 = k // 2 + ald + (num + den // 2) // den
    return k, e1 * e1, 2 * k * ald


def test_cusp_margin_keeps_cusp_move_in_int64():
    # The comment's bound (gen.py): widest span, the margin, the largest c.
    lmin, lmax = 0, 100 << 16
    c = (1 << 29) - 1
    margin = CUSP_MARGIN_Q8 << 8
    for lc in (margin, lmax // 2, lmax - margin):
        for l in (lmin, lc - 1, lc, lc + 1, lmax):
            k, e2, kld = _cusp_move_arithmetic(lmin, lmax, lc, l, c)
            assert k >= 1 << 16 and e2 < 2**63 and kld < 2**63
    # Half the margin is not enough: the check is not vacuous.
    _, e2, _ = _cusp_move_arithmetic(lmin, lmax, margin // 2, lmin, c)
    assert e2 >= 2**63


@pytest.mark.parametrize("sigma", [1.0, 2.5, 6.0, 10.0])
def test_kernel_symmetric_sums_to_2_15(sigma):
    taps, _ = kernel_q15(sigma)
    assert sum(taps) == 32768 and taps == taps[::-1] and len(taps) == 2 * int(np.ceil(3 * sigma)) + 1


def test_panel_kernel(t):
    k = t.ints["kernel"]
    assert t.ints["kernel_radius"] == 18 and sum(k) == 32768 and k == k[::-1]


def test_srgb_table():
    text = render_srgb()[1]
    lin = [int(v) for v in re.search(r"lin_q16\[256\] = \{(.*?)\};", text, re.S).group(1).replace(",", " ").split()]
    assert len(lin) == 256 and lin[0] == 0 and lin[255] == 65536
    assert all(b > a for a, b in zip(lin, lin[1:]))


def _ints_after(text, field):
    m = re.search(r"\." + field + r" =\n        \{\n(.*?)\n        \},\n", text, re.S) or \
        re.search(r"\." + field + r" = \{(.*?)\},\n", text, re.S)
    return [int(v) for v in re.findall(r"-?\d+", re.sub(r"INT64_C", "", m.group(1)))]


def test_rendered_values_match_derived(t):
    text = outputs([EL133UF1])["src/panels/el133uf1.c"][0]
    for f in ("kernel", "hull_d", "cusp_lstar", "lin_to_panel_adapt", "ink_oklab", "hull_n"):
        assert _ints_after(text, f) == list(np.asarray(t.ints[f]).ravel()), f


def test_committed_files_up_to_date():
    r = subprocess.run([sys.executable, "-m", "panelgen", "--check"], cwd=PANELGEN_DIR,
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr


def test_rerun_is_byte_identical(tmp_path):
    roots = [tmp_path / "a", tmp_path / "b"]
    for root in roots:
        subprocess.run([sys.executable, "-m", "panelgen", "--root", str(root)], cwd=PANELGEN_DIR,
                       check=True, capture_output=True)
    rels = ["src/epi_srgb.c"] + [f"src/panels/{p}.c" for p in PANELS]
    for rel in rels:
        a, b = ((root / rel).read_bytes() for root in roots)
        assert a == b == (REPO / rel).read_bytes(), rel


@pytest.mark.skipif(shutil.which("cc") is None, reason="no C compiler")
def test_generated_c_compiles_cleanly(tmp_path):
    for rel in ["src/epi_srgb.c"] + [f"src/panels/{p}.c" for p in PANELS]:
        r = subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-Iinclude", "-Isrc",
                            "-c", rel, "-o", str(tmp_path / "x.o")], cwd=REPO, capture_output=True, text=True)
        assert r.returncode == 0, rel + "\n" + r.stderr
