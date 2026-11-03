"""Derive, check and render the generated C tables (docs/design.md, "Panel tables", "Colour model").

Every real-valued quantity comes from model.py (model.inks -> gamut.py
InkGamut, model.cusp_lut, OKLab) and colour.py; this module only
folds matrices, quantises and formats. Quantisation is q(x, bits) =
sign(x) floor(|x| 2^bits + 1/2), half away from zero.
"""

from __future__ import annotations

import hashlib
import json
import math
from dataclasses import asdict, dataclass, field

import numpy as np

from . import PanelError, model
from .colour import M_BRADFORD, M_RGB_TO_XYZ, M_XYZ_TO_RGB, srgb_to_linear
from .gamut import BISECT_ITERS, HULL_TOL
from .model import OK_M1, OK_M2, inks
from .palette import BUILTIN_TABLES
from .panels import Panel

COMMAND = "cd tools/panelgen && python -m panelgen"
FORMAT_VERSION = 2  # bump when the emitted layout changes (part of the input hash)
RULES = {
    "round": "q(x, bits) = sign(x) * floor(|x| * 2^bits + 0.5)",
    "kernel": "w_i = exp(-i^2 / (2 sigma^2)), i = -r..r, r = ceil(3 sigma); normalised; "
              "q(w, 15); residual 32768 - sum added to the centre tap",
    "hull": "unit n, n_q = q(n, 16), d_q = q(d, 32); d_q lowered until every ink "
            "(q(xyz, 16)) has n_q . x_q + d_q <= 0",
    "format_version": FORMAT_VERSION,
}


# -- quantisation ---------------------------------------------------------------

def white_exact_rows(m, white, black):
    """Source white (lin 1, 1, 1) must land exactly on the white ink: rounding
    entry by entry can miss the row sum by an LSB, which puts white just
    outside the hull's white vertex. Put each row's residual on its
    largest-magnitude entry."""
    m = [list(row) for row in m]
    for i, row in enumerate(m):
        k = max(range(3), key=lambda c: abs(row[c]))
        row[k] += white[i] - black[i] - sum(row)
    return m


def q(x: float, bits: int) -> int:
    """Round x * 2^bits half away from zero, as a Python int."""
    x = float(x)
    if not math.isfinite(x):
        raise ValueError(f"cannot quantise {x}")
    m = math.floor(abs(x) * 2.0**bits + 0.5)
    return int(-m if x < 0 else m)


def qa(a, bits: int):
    """q over a (nested) array, as nested lists of ints."""
    a = np.asarray(a, dtype=np.float64)
    if a.ndim == 0:
        return q(a, bits)
    return [qa(v, bits) for v in a]


def deq(a, bits: int) -> np.ndarray:
    return np.asarray(a, dtype=np.float64) / 2.0**bits


def rshift_round(v: int, bits: int) -> int:
    """v / 2^bits rounded half away from zero (integer-exact)."""
    h = 1 << (bits - 1)
    return (v + h) >> bits if v >= 0 else -((-v + h) >> bits)


# -- input hash ------------------------------------------------------------------

def _digest(obj) -> str:
    blob = json.dumps(obj, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(blob).hexdigest()


def srgb_inputs() -> dict:
    return {
        "M_RGB_TO_XYZ": M_RGB_TO_XYZ.tolist(),
        "eotf": "u/255; c <= 0.04045 ? c/12.92 : ((c+0.055)/1.055)^2.4 (panelgen.colour.srgb_to_linear)",
        "formats": {"epi_srgb_to_lin_q16": 16, "epi_srgb_to_xyz_q16": 16},
        "rules": RULES,
    }


def panel_inputs(panel: Panel) -> dict:
    entries = BUILTIN_TABLES[panel.palette][panel.table][0]
    return {
        "panel": asdict(panel),
        "inks": {k: [list(v[0]), list(v[1])] for k, v in entries.items()},
        "constants": {
            "M_RGB_TO_XYZ": M_RGB_TO_XYZ.tolist(),
            "M_BRADFORD": M_BRADFORD.tolist(),
            "OK_M1": OK_M1.tolist(),
            "OK_M2": OK_M2.tolist(),
            "HULL_TOL": HULL_TOL,
            "BISECT_ITERS": BISECT_ITERS,
            "CUSP_L_SAMPLES": model.CUSP_L_SAMPLES,
            "CUSP_REFINE": model.CUSP_REFINE,
            "CUSP_REFINE_PASSES": model.CUSP_REFINE_PASSES,
            "CUSP_C_PROBE": model.CUSP_C_PROBE,
        },
        "rules": RULES,
    }


# -- derivation ------------------------------------------------------------------

@dataclass
class Derived:
    """Quantised tables (ints) plus the double-precision values they came from."""
    panel: Panel
    ints: dict
    real: dict
    diag: dict = field(default_factory=dict)


def kernel_q15(sigma: float) -> tuple[list[int], int]:
    r = math.ceil(3.0 * sigma)
    i = np.arange(-r, r + 1, dtype=np.float64)
    w = np.exp(-(i * i) / (2.0 * sigma * sigma))
    w = w / w.sum()
    taps = [q(v, 15) for v in w]
    residual = 32768 - sum(taps)
    taps[r] += residual
    return taps, residual


def derive(panel: Panel) -> Derived:
    entries = BUILTIN_TABLES[panel.palette][panel.table][0]
    if tuple(entries) != panel.ink_names:
        raise PanelError(f"{panel.name}: ink order {tuple(entries)} != {panel.ink_names}")
    if len(panel.codes) != len(panel.ink_names):
        raise PanelError(f"{panel.name}: {len(panel.codes)} codes for {len(panel.ink_names)} inks")
    ink = inks(panel.palette, panel.table)
    g = ink.gamut
    if g.rank != 3:
        raise PanelError(f"{panel.name}: colour panels need a 3-D ink hull (rank {g.rank})")
    iw, ib = int(np.argmax(ink.Y)), int(np.argmin(ink.Y))
    if not (np.array_equal(ink.xyz[iw], g.white) and np.array_equal(ink.xyz[ib], g.black)):
        raise AssertionError("white/black ink mismatch between Inks and InkGamut")

    # Panel model: panel = M(D) lin + B, M(D) = D M_adapt + (1 - D) M_bpc (both on linear sRGB).
    scale = (g.white - g.black) / g.adapted_display_white  # k (InkGamut.black_point_compensate)
    lin_to_panel_adapt = np.diag(scale) @ g.adapt_matrix @ M_RGB_TO_XYZ
    lin_to_panel_bpc = (g.white[1] - g.black[1]) * M_RGB_TO_XYZ
    # OKLab_ink(v) = M2 cbrt(M1 diag(1/w) v).
    lin_to_lms = OK_M1 @ np.diag(1.0 / ink.white_lin)
    nhat = ink.white_lin / np.linalg.norm(ink.white_lin)

    # Hull faces (Qhull, n.x + d <= 0 inside), unit n.
    eq = np.asarray(g.equations, dtype=np.float64)
    norm = np.linalg.norm(eq[:, :3], axis=1)
    n = eq[:, :3] / norm[:, None]
    d = eq[:, 3] / norm
    n_q = [qa(v, 16) for v in n]
    d_q0 = [q(v, 32) for v in d]
    x_q = [qa(v, 16) for v in ink.xyz]
    d_q, nudges = [], []
    for nf, df in zip(n_q, d_q0):
        worst = max(sum(a * b for a, b in zip(nf, xi)) for xi in x_q)
        dn = min(df, -worst)
        d_q.append(dn)
        nudges.append(df - dn)

    lmin, lmax = g.neutral_lstar
    cusp = model.cusp_lut(g)[:, 0]
    taps, residual = kernel_q15(panel.detail_sigma)

    real = {
        "white_xyz": g.white, "black_xyz": g.black,
        "lin_to_panel_adapt": lin_to_panel_adapt, "lin_to_panel_bpc": lin_to_panel_bpc,
        "xyz_to_lin": M_XYZ_TO_RGB, "lin_to_lms": lin_to_lms, "lms_to_oklab": OK_M2,
        "hull_n": n, "hull_d": d, "lstar_min": lmin, "lstar_max": lmax, "cusp_lstar": cusp,
        "ink_lin": ink.lin, "ink_oklab": ink.coords["oklab"], "nhat": nhat,
        "ink_xyz": ink.xyz, "white_index": iw,
    }
    ints = {
        "kind": "EPI_PANEL_COLOUR",
        "ink_count": len(panel.ink_names),
        "kernel_radius": (len(taps) - 1) // 2,
        "kernel": taps,
        "white_xyz": qa(g.white, 16),
        "black_xyz": qa(g.black, 16),
        "lin_to_panel_adapt": white_exact_rows(qa(lin_to_panel_adapt, 16), qa(g.white, 16),
                                               qa(g.black, 16)),
        "lin_to_panel_bpc": qa(lin_to_panel_bpc, 16),
        "xyz_to_lin": qa(M_XYZ_TO_RGB, 16),
        "lin_to_lms": qa(lin_to_lms, 16),
        "lms_to_oklab": qa(OK_M2, 16),
        "hull_face_count": len(n_q),
        "hull_n": n_q,
        "hull_d": d_q,
        "lstar_min": q(lmin, 8),
        "lstar_max": q(lmax, 8),
        "cusp_lstar": [q(v, 8) for v in cusp],
        "ink_lin": qa(ink.lin, 16),
        "ink_oklab": qa(ink.coords["oklab"], 16),
        "nhat": qa(nhat, 16),
    }
    diag = {"hull_d_unnudged": d_q0, "hull_nudges": nudges, "kernel_residual": residual,
            "ink_xyz_q16": x_q}
    out = Derived(panel=panel, ints=ints, real=real, diag=diag)
    check(out)
    return out


# -- limits of the C ---------------------------------------------------------------

MAX_INKS = 16
MAX_HULL_FACES = 32
MAX_KERNEL_RADIUS = 32
_MID_DELTA_MIN = 0.3371

# How far inside the neutral interval every cusp must be, in Q8 L* (0.5 L*), so
# that cusp_move (src/epi_intent.c) needs no run-time guard. There, in Q16 L*:
#
#   span = lmax - lmin <= 100 * 2^16.
#   lc interpolates two neighbouring cusps, so it is at least margin * 2^8 from
#   each end, and k = 2 * (ld > 0 ? lmax - lc : lc - lmin) lies in [2^16, 2 * span].
#   |ld| <= k / 2, since l is in [lmin, lmax].
#   c = hypot(a, b) * 2^8, with a and b as they reach cusp_move, not clamped:
#   lab_to_xyz's +-(2^17 - 1) applies only to its own copies. panel_lab gives
#   f in [-2^18, 2^21] (lab_f_q19 of an int32 ratio), so |a| <= 500 * (2^21 + 2^18)
#   / 2^11 = 576000 and |b| <= 230400 in Q8; vibrance multiplies both by at most
#   3, so c < 2^29.
#   e1 = k/2 + |ld| + round(c * span / (20 * k))
#      <= 2 * span + 2^29 * 100 * 2^16 / (20 * 2^16) + 1 < 2.7e9 < 2^31.5,
#   so e1 * e1 fits int64, and 2 * k * |ld| <= k^2 <= (2 * span)^2 < 2^48.
CUSP_MARGIN_Q8 = 128


def midtone_monotone(lstar_min_q8: int) -> bool:
    """epi_midtone_monotone."""
    lmin = lstar_min_q8 / 256.0
    k = lmin / (1.0 - _MID_DELTA_MIN)
    k = k if k > 60.0 else 60.0
    delta = (k - lmin) / k
    fa, fb = 0.15 / delta, 1.0 / delta
    return lmin >= 0.0 and k < 100.0 and fa * fa + fb * fb <= 9.0


def check_tables_valid(name: str, I: dict) -> None:
    """What epi_converter_init asks of colour tables (src/epi_converter.c, tables_valid and
    valid_config), so that it cannot refuse a generated panel. check_midtone_knee and
    check_cusps cover the rest: L*min's upper bound, L*max and L*min < L*max."""
    for what, n, limit in (("inks", I["ink_count"], MAX_INKS), ("hull faces", I["hull_face_count"], MAX_HULL_FACES),
                           ("kernel radius", I["kernel_radius"], MAX_KERNEL_RADIUS)):
        if n > limit:
            raise PanelError(f"{name}: {what} {n} (at most {limit})")
    if I["lstar_min"] <= 0:
        raise PanelError(f"{name}: neutral range's darkest L* {I['lstar_min'] / 256:.2f} must be above 0")
    black_y, white_y = I["black_xyz"][1], I["white_xyz"][1]
    if not 0 <= black_y < white_y:
        raise PanelError(f"{name}: black ink Y {black_y} must be at least 0 and below white ink Y {white_y} (Q16)")
    if min(I["white_xyz"]) <= 512:
        raise PanelError(f"{name}: white ink XYZ {I['white_xyz']} must be above 512 in Q16, to keep its reciprocal "
                         "in int32")


def check_presets(panel: Panel) -> None:
    """What epi_intent_init asks of a preset; epi_converter_init needs at least one."""
    if not panel.presets:
        raise PanelError(f"{panel.name}: no presets")
    for pr in panel.presets:
        for knob, hi in (("tone", 1.0), ("detail", 1.0), ("adapt", 1.0), ("vibrance", 3.0), ("veil", 3.0)):
            if not 0.0 <= getattr(pr, knob) <= hi:
                raise PanelError(f"{panel.name}: preset {pr.name} {knob} {getattr(pr, knob)} outside 0..{hi}")


def check_midtone_knee(name: str, lstar_min_q8: int) -> None:
    if not midtone_monotone(lstar_min_q8):
        raise PanelError(f"{name}: neutral range's darkest L* {lstar_min_q8 / 256:.2f} has no monotone "
                         "midtone knee (epi_midtone_monotone needs L* in [0, 66.29))")


def check_cusps(name: str, lstar_min_q8: int, lstar_max_q8: int, cusp_q8: list[int]) -> None:
    if lstar_max_q8 > 100 * 256 or lstar_max_q8 - lstar_min_q8 < 2 * CUSP_MARGIN_Q8:
        raise PanelError(f"{name}: neutral range L* {lstar_min_q8 / 256:.2f}..{lstar_max_q8 / 256:.2f} "
                         f"cannot hold cusps {CUSP_MARGIN_Q8 / 256} L* inside it")
    lo, hi = lstar_min_q8 + CUSP_MARGIN_Q8, lstar_max_q8 - CUSP_MARGIN_Q8
    for hue, c in enumerate(cusp_q8):
        if not lo <= c <= hi:
            raise PanelError(f"{name}: cusp L* {c / 256:.2f} at hue {hue} is not {CUSP_MARGIN_Q8 / 256} L* inside "
                             f"the neutral range {lstar_min_q8 / 256:.2f}..{lstar_max_q8 / 256:.2f} (cusp_move "
                             "would overflow or divide by zero)")


# -- self-checks -----------------------------------------------------------------

def check(t: Derived, seed: int = 1) -> dict:
    """Assert the derived tables against the double-precision model and the
    limits of the C; record results in t.diag."""
    g = inks(t.panel.palette, t.panel.table).gamut
    r, I, dg = t.real, t.ints, t.diag
    check_presets(t.panel)
    check_tables_valid(t.panel.name, I)
    check_midtone_knee(t.panel.name, I["lstar_min"])
    check_cusps(t.panel.name, I["lstar_min"], I["lstar_max"], I["cusp_lstar"])
    rng = np.random.default_rng(seed)
    lin = np.concatenate([rng.uniform(0.0, 1.0, (256, 3)), np.eye(3), np.ones((1, 3)), np.zeros((1, 3))])
    src = lin @ M_RGB_TO_XYZ.T

    # 1. D = 1 part reproduces gamut.to_panel; the D blend reproduces model.panel_partial.
    ma, mb, B = r["lin_to_panel_adapt"], r["lin_to_panel_bpc"], g.black
    err_adapt = float(np.abs(lin @ ma.T + B - g.to_panel(src)).max())
    err_blend = 0.0
    for D in (0.0, 0.25, 0.5, 0.8, 1.0):
        mine = lin @ (D * ma + (1.0 - D) * mb).T + B
        err_blend = max(err_blend, float(np.abs(mine - model.panel_partial(g, src, D)).max()))
    Dpix = rng.uniform(0.0, 1.0, (len(lin), 1))  # per-pixel D, as the _h ramp gives
    mine = (Dpix * (lin @ ma.T) + (1.0 - Dpix) * (lin @ mb.T)) + B
    err_blend = max(err_blend, float(np.abs(mine - model.panel_partial(g, src, Dpix)).max()))
    assert err_adapt < 1e-12 and err_blend < 1e-12, (err_adapt, err_blend)
    dg["err_adapt_double"], dg["err_blend_double"] = err_adapt, err_blend

    # Quantisation error of every Q16 matrix: 2^-17, except that white_exact_rows
    # may move one entry per row of lin_to_panel_adapt by a further LSB.
    qerr = {k: float(np.abs(deq(I[k], 16) - r[k]).max())
            for k in ("lin_to_panel_adapt", "lin_to_panel_bpc", "xyz_to_lin", "lin_to_lms", "lms_to_oklab")}
    for k, e in qerr.items():
        assert e <= (3 if k == "lin_to_panel_adapt" else 1) * 2.0**-17 + 1e-15, (k, e)
    dg["matrix_q16_err"] = qerr
    # The same check with the Q16 matrices (what C computes, before its own rounding).
    mq = deq(I["lin_to_panel_adapt"], 16)
    dg["err_adapt_q16"] = float(np.abs(lin @ mq.T + deq(I["black_xyz"], 16) - g.to_panel(src)).max())

    # OKLab fold: OK_M2 cbrt(lin_to_lms lin) == Inks.to_space(lin, "oklab").
    ink = inks(t.panel.palette, t.panel.table)
    ok_fold = np.cbrt(lin @ r["lin_to_lms"].T) @ OK_M2.T
    err_ok = float(np.abs(ok_fold - ink.to_space(lin, "oklab")).max())
    assert err_ok < 1e-12, err_ok
    dg["err_oklab_fold_double"] = err_ok

    # 2. Every ink inside the Q16 hull (exact integer test, as C does it).
    worst = max(sum(a * b for a, b in zip(nf, xi)) + df
                for nf, df in zip(I["hull_n"], I["hull_d"]) for xi in dg["ink_xyz_q16"])
    assert worst <= 0, worst
    dg["hull_worst_ink"] = worst
    assert np.allclose(deq(I["hull_n"], 16), r["hull_n"], atol=2.0**-17)
    # Random mixtures of the inks (inside the double-precision hull) test inside the Q16 hull.
    mix = rng.dirichlet(np.ones(len(r["ink_xyz"])), 4096) @ r["ink_xyz"]
    xq = np.array([[q(v, 16) for v in p] for p in mix], dtype=np.int64)
    sd = (xq @ np.array(I["hull_n"], dtype=np.int64).T + np.array(I["hull_d"], dtype=np.int64)).max(1)
    assert g.contains(mix).all() and (sd <= 0).all()

    # 3. White ink -> OKLab (1, 0, 0) through the Q16 tables, integer-simulated.
    wl = I["ink_lin"][r["white_index"]]
    lms = [rshift_round(sum(a * b for a, b in zip(row, wl)), 16) for row in I["lin_to_lms"]]
    c = [q(np.cbrt(v / 65536.0), 16) for v in lms]
    ok = [rshift_round(sum(a * b for a, b in zip(row, c)), 16) for row in I["lms_to_oklab"]]
    lsb = [ok[0] - 65536, ok[1], ok[2]]
    assert max(abs(v) for v in lsb) <= 4, (lms, ok)
    assert I["ink_oklab"][r["white_index"]] == [65536, 0, 0], I["ink_oklab"][r["white_index"]]
    dg["white_lms_q16"], dg["white_oklab_q16_path"], dg["white_oklab_lsb_err"] = lms, ok, lsb

    # 4. Kernel symmetric and summing to 2^15.
    k = I["kernel"]
    assert sum(k) == 32768 and k == k[::-1] and len(k) == 2 * I["kernel_radius"] + 1
    assert all(v > 0 for v in k)

    # Ranges of the C types.
    assert 0 <= min(I["cusp_lstar"]) and max(I["cusp_lstar"]) <= 0xFFFF
    for key in ("white_xyz", "black_xyz", "lin_to_panel_adapt", "lin_to_panel_bpc", "xyz_to_lin",
                "lin_to_lms", "lms_to_oklab", "hull_n", "ink_lin", "ink_oklab", "nhat"):
        flat = np.asarray(I[key], dtype=np.int64).ravel()
        assert (np.abs(flat) < 2**31).all(), key
    return dg


# -- C rendering -----------------------------------------------------------------

def _header(inputs_sha: str, what: list[str]) -> str:
    lines = ["/*", " * Generated by tools/panelgen — do not edit.", " *",
             f" * Command: {COMMAND}", f" * Inputs SHA-256: {inputs_sha}", " *"]
    lines += [f" * {w}" if w else " *" for w in what]
    return "\n".join(lines + [" */", "", "/* clang-format off */", ""])


def _row(vals, fmt=str) -> str:
    return "{" + ", ".join(fmt(v) for v in vals) + "}"


def _block(vals, per_line: int, indent: str, fmt=str) -> str:
    out = []
    for i in range(0, len(vals), per_line):
        out.append(indent + ", ".join(fmt(v) for v in vals[i:i + per_line]) + ",")
    return "\n".join(out)


def _mat(name: str, m, indent="    ", fmt=str) -> str:
    rows = "\n".join(f"{indent}        {_row(r, fmt)}," for r in m)
    return f"{indent}.{name} =\n{indent}    {{\n{rows}\n{indent}    }},"


def _flt(v: float) -> str:
    s = repr(float(v))
    return s + "f"


def srgb_tables() -> tuple[list[int], list[list[int]]]:
    """(epi_srgb_to_lin_q16, epi_srgb_to_xyz_q16) as ints: what src/epi_srgb.c holds."""
    return qa(srgb_to_linear(np.arange(256, dtype=np.uint8)), 16), qa(M_RGB_TO_XYZ, 16)


def render_srgb() -> tuple[str, str]:
    """(header comment hash, src/epi_srgb.c text)."""
    sha = _digest(srgb_inputs())
    lin, mat = srgb_tables()
    head = _header(sha, [
        'Panel-independent sRGB constants (docs/design.md, "Colour model").',
        "epi_srgb_to_lin_q16: sRGB EOTF of u = 0..255, Q16 (255 -> 65536).",
        "epi_srgb_to_xyz_q16: the 4-decimal IEC 61966-2-1 matrix, Q16 (XYZ = M lin).",
    ])
    body = [head, '#include "epi_srgb.h"', "",
            "const uint32_t epi_srgb_to_lin_q16[256] = {",
            _block(lin, 8, "    "), "};", "",
            "const int32_t epi_srgb_to_xyz_q16[3][3] = {",
            "\n".join(f"    {_row(r)}," for r in mat), "};", ""]
    return sha, "\n".join(body)


def render_panel(t: Derived) -> tuple[str, str]:
    p, I = t.panel, t.ints
    sha = _digest(panel_inputs(p))
    measured = inks(p.palette, p.table).palette.measured
    nominal = inks(p.palette, p.table).palette.nominal
    pid = p.ident
    head = _header(sha, [
        f"Panel calibration {p.name}: {p.doc}.",
        f"Inks: ink table {p.palette}/{p.table} (tools/panelgen/panelgen/palette.py).",
        "Tables: see src/epi_panel_tables.h for every field's Q format. The kernel",
        "holds tap -r at index 0 through tap +r at index 2r; entries beyond are 0.",
        f"Hull: {I['hull_face_count']} faces; d lowered by {sum(1 for v in t.diag['hull_nudges'] if v)} "
        f"face(s), max {max(t.diag['hull_nudges'])} Q32 LSB, so every ink tests inside in Q16.",
    ])
    out = [head, '#include "epaper_image.h"', '#include "../epi_panel_tables.h"', ""]
    out.append(f"static const epi_ink_t {pid}_inks[{len(p.ink_names)}] = {{")
    for name, code, nom, mea in zip(p.ink_names, p.codes, nominal, measured):
        out.append(f"    {{{code}, {_row(nom.tolist())}, {_row(mea.tolist())}}}, /* {name} */")
    out += ["};", ""]
    for pr in p.presets:
        out.append(f"static const epi_intent_t {pid}_intent_{pr.name.lower()} = {{")
        out.append(f"    .tone = {_flt(pr.tone)},")
        out.append(f"    .detail = {_flt(pr.detail)},")
        out.append(f"    .adapt = {_flt(pr.adapt)},")
        out.append(f"    .vibrance = {_flt(pr.vibrance)},")
        out.append(f"    .veil = {_flt(pr.veil)},")
        out += ["};", ""]
    out.append(f"static const epi_preset_t {pid}_presets[{len(p.presets)}] = {{")
    for pr in p.presets:
        out.append(f'    {{"{pr.name}", &{pid}_intent_{pr.name.lower()}}},')
    out += ["};", ""]

    i64 = lambda v: f"INT64_C({v})"  # noqa: E731
    out.append(f"static const epi_colour_tables_t {pid}_tables = {{")
    out.append(f"    .base.kind = {I['kind']},")
    out.append(f"    .base.ink_count = {I['ink_count']},")
    out.append(f"    .base.kernel_radius = {I['kernel_radius']},")
    out.append("    .base.kernel =\n        {\n" + _block(I["kernel"], 10, "            ") + "\n        },")
    out.append(f"    .white_xyz = {_row(I['white_xyz'])},")
    out.append(f"    .black_xyz = {_row(I['black_xyz'])},")
    for k in ("lin_to_panel_adapt", "lin_to_panel_bpc", "xyz_to_lin", "lin_to_lms", "lms_to_oklab"):
        out.append(_mat(k, I[k]))
    out.append(f"    .hull_face_count = {I['hull_face_count']},")
    out.append(_mat("hull_n", I["hull_n"]))
    out.append("    .hull_d =\n        {\n" + _block(I["hull_d"], 2, "            ", i64) + "\n        },")
    out.append(f"    .lstar_min = {I['lstar_min']},")
    out.append(f"    .lstar_max = {I['lstar_max']},")
    out.append("    .cusp_lstar =\n        {\n" + _block(I["cusp_lstar"], 12, "            ") + "\n        },")
    out.append(_mat("ink_lin", I["ink_lin"]))
    out.append(_mat("ink_oklab", I["ink_oklab"]))
    out.append(f"    .nhat = {_row(I['nhat'])},")
    out += ["};", ""]

    out.append(f"const epi_panel_t epi_panel_{pid} = {{")
    out.append(f'    .name = "{p.name}",')
    out.append(f"    .width = {p.width},")
    out.append(f"    .height = {p.height},")
    out.append(f"    .inks = {pid}_inks,")
    out.append(f"    .ink_count = {len(p.ink_names)},")
    out.append(f"    .detail_sigma = {_flt(p.detail_sigma)},")
    out.append(f"    .presets = {pid}_presets,")
    out.append(f"    .preset_count = {len(p.presets)},")
    out.append(f"    .tables = &{pid}_tables.base,")
    out += ["};", ""]
    return sha, "\n".join(out)


def outputs(panels) -> dict[str, tuple[str, Derived | None]]:
    """{repo-relative path: (text, derived tables or None)} for every generated file."""
    res = {"src/epi_srgb.c": (render_srgb()[1], None)}
    for p in panels:
        t = derive(p)
        res[f"src/panels/{p.ident}.c"] = (render_panel(t)[1], t)
    return res
