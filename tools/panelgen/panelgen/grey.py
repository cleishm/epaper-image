"""Grey calibrations as const C: the tables epi_grey_panel_init builds at
runtime (src/epi_grey_panel.c), computed offline by the same rules.

The C derives everything at init (design.md "Fixed-point arithmetic and determinism"): level Y in integers from the
constant sRGB tables; thresholds and the Gaussian kernel in double with
+ - * / plus the library's epi_cbrt_d and epi_exp_d (src/epi_math.c). Python
floats are IEEE doubles and CPython does not contract a*b + c into an FMA, so
the double parts below are a transcription of the C, operation for operation
and in the same order; the integer parts are exact. The equivalence gate
(tests/test_grey.py) compiles the generated file with the library and checks
the tables against epi_grey_panel_init byte for byte.

Input: a levels JSON (the panel-photo calibration's output): one entry per mode, keyed <ident>_<mode>, each with
`nominal`, `perceived` (measured sRGB8, darkest first), `codes` and a
`photocal` metadata block.
"""

from __future__ import annotations

import hashlib
import json
import struct
from dataclasses import asdict, dataclass
from pathlib import Path

from . import REPO
from .gen import (COMMAND, MAX_INKS, MAX_KERNEL_RADIUS, RULES, _block, _digest, _flt, _header, _row, kernel_q15,
                  midtone_monotone, srgb_inputs, srgb_tables)
from .panels import GreyPanel

GREY_FORMAT_VERSION = 2  # bump when the emitted layout or a rule changes (part of the input hash)
GREY_RULES = {
    "level_y": "Y_i = sum_c srgb_to_xyz_q16[1][c] * srgb_to_lin_q16[m_c] (Q32, exact); "
               "level_y_i = (Y_i * 2^16 + Y_last / 2) / Y_last (integer)",
    "level_thr": "c = epi_cbrt_d(level_y / 65536.0); m = (c_i + c_i+1) / 2.0; "
                 "floor(m*m*m * 65536.0), clamped to [level_y_i, level_y_i+1 - 1]",
    "kernel": "src/epi_grey_panel.c gaussian_kernel (epi_exp_d, sequential sum, "
              "(int)(w/sum * 32768.0 + 0.5), residual on the centre tap); must equal panelgen kernel_q15",
    "valid": "2..16 levels, Y_last > 0, level_y strictly increasing, "
             "epi_midtone_monotone(epi_grey_lstar_q8(level_y[0])), sigma in (0, 10.5]",
    "format_version": GREY_FORMAT_VERSION,
}


class GreyError(ValueError):
    """The levels JSON cannot become a calibration (nothing is written)."""


# -- the C's double helpers, transcribed (src/epi_math.c, src/epi_intent.c) -------

def _dbits(d: float) -> int:
    return struct.unpack("<Q", struct.pack("<d", d))[0]


def _dfrom(u: int) -> float:
    return struct.unpack("<d", struct.pack("<Q", u))[0]


def _f32(x: float) -> float:
    """A float value as C's `float` holds it (then widened to double)."""
    return struct.unpack("<f", struct.pack("<f", x))[0]


def _two_prod(a: float, b: float) -> tuple[float, float]:
    split = 134217729.0  # 2^27 + 1
    prod = a * b
    ca = split * a
    ah = ca - (ca - a)
    al = a - ah
    cb = split * b
    bh = cb - (cb - b)
    bl = b - bh
    err = ((ah * bh - prod) + ah * bl + al * bh) + al * bl
    return prod, err


def cbrt_d(x: float) -> float:
    """epi_cbrt_d."""
    if x == 0.0:
        return x
    u = _dbits(x)
    mant = (1 << 52) - 1
    e = (u >> 52) - 1023
    r = e % 3  # Python's % is C's remainder plus the r < 0 fix-up
    k = (e - r) // 3  # exact
    m = _dfrom((u & mant) | ((1023 + r) << 52))  # [1, 8)
    y = 0.8125 + 0.15625 * m
    for _ in range(6):
        y = y - (y * y * y - m) / (3.0 * y * y)
    p, pe = _two_prod(y, y)
    c, ce = _two_prod(y, p)
    res = ((m - c) - ce) - y * pe
    y = y + res / (3.0 * y * y)
    return y * _dfrom((1023 + k) << 52)


_EXP_C = [float.fromhex(h) for h in (
    "0x1.6124613a86d09p-33", "0x1.1eed8eff8d898p-29", "0x1.ae64567f544e4p-26", "0x1.27e4fb7789f5cp-22",
    "0x1.71de3a556c734p-19", "0x1.a01a01a01a01ap-16", "0x1.a01a01a01a01ap-13", "0x1.6c16c16c16c17p-10",
    "0x1.1111111111111p-7", "0x1.5555555555555p-5", "0x1.5555555555555p-3")]
_INV_LN2 = float.fromhex("0x1.71547652b82fep+0")
_LN2_HI = float.fromhex("0x1.62e42fee00000p-1")
_LN2_LO = float.fromhex("0x1.a39ef35793c76p-33")


def _pow2_d(k: int) -> float:
    return _dfrom((1023 + k) << 52)


def exp_d(x: float) -> float:
    """epi_exp_d."""
    if x < -746.0:
        return 0.0
    kd = x * _INV_LN2
    k = int(kd - 0.5)  # C (int) truncates, as int() does
    r = (x - float(k) * _LN2_HI) - float(k) * _LN2_LO
    p = _EXP_C[0]
    for c in _EXP_C[1:]:
        p = p * r + c
    p = p * r + 0.5
    y = 1.0 + (r + (r * r) * p)
    if k < -1022:
        return (y * _pow2_d(k + 54)) * _pow2_d(-54)
    return y * _pow2_d(k)


_D_DELTA = 6.0 / 29.0


def _lab_f_d(t: float) -> float:
    if t > _D_DELTA * _D_DELTA * _D_DELTA:
        return cbrt_d(t)
    return t / (3.0 * _D_DELTA * _D_DELTA) + 4.0 / 29.0


def _round_d(x: float) -> int:
    return int(x + 0.5) if x >= 0.0 else -int(-x + 0.5)


def grey_lstar_q8(y_q16: int) -> int:
    """epi_grey_lstar_q8."""
    return _round_d((116.0 * _lab_f_d(y_q16 / 65536.0) - 16.0) * 256.0)


def kernel_c(sigma_f: float) -> list[int]:
    """src/epi_grey_panel.c gaussian_kernel: taps -r..r, Q15."""
    sigma = _f32(sigma_f)
    r3 = 3.0 * sigma
    r = int(r3)
    if float(r) < r3:
        r += 1
    den = 2.0 * sigma * sigma
    w, s = [], 0.0
    for i in range(-r, r + 1):
        di = float(i)
        w.append(exp_d(-(di * di) / den))
        s += w[-1]
    taps = [int((v / s) * 32768.0 + 0.5) for v in w]
    taps[r] += 32768 - sum(taps)
    return taps


# -- derivation ------------------------------------------------------------------

def measured_y_q32(rgb, lin, xyz) -> int:
    return sum(xyz[1][c] * lin[rgb[c]] for c in range(3))


@dataclass
class GreyTables:
    ident: str  # epi_panel_<ident>
    mode: str
    levels: list  # [(code, nominal(3), measured(3))]
    kernel: list[int]
    level_y: list[int]
    level_thr: list[int]
    lstar_min_q8: int


def derive_grey(panel: GreyPanel, mode: str, entry: dict) -> GreyTables:
    """The tables epi_grey_panel_init derives for this entry and panel.sigma,
    or GreyError where it would return EPI_ERR_INVALID_ARG."""
    ident = f"{panel.ident}_{mode}"
    try:
        nominal, perceived, codes = entry["nominal"], entry["perceived"], entry["codes"]
    except (KeyError, TypeError) as e:
        raise GreyError(f"{ident}: entry lacks {e}") from None
    n = len(perceived)
    if not (len(nominal) == len(codes) == n):
        raise GreyError(f"{ident}: {len(nominal)} nominal, {n} perceived, {len(codes)} codes")
    if not 2 <= n <= MAX_INKS:
        raise GreyError(f"{ident}: {n} levels (2..{MAX_INKS})")

    def u8(v, what):
        if isinstance(v, bool) or not isinstance(v, int) or not 0 <= v <= 255:
            raise GreyError(f"{ident}: {what} {v!r} is not an integer 0..255")
        return v

    levels = []
    for j in range(n):
        if len(nominal[j]) != 3 or len(perceived[j]) != 3:
            raise GreyError(f"{ident}: level {j} colour is not an RGB triple")
        levels.append((u8(codes[j], "code"), [u8(v, "nominal") for v in nominal[j]],
                       [u8(v, "perceived") for v in perceived[j]]))
    sigma = _f32(panel.detail_sigma)
    if not (0.0 < sigma <= 10.5):
        raise GreyError(f"{ident}: sigma {sigma} outside (0, 10.5]")

    lin, xyz = srgb_tables()
    y = [measured_y_q32(m, lin, xyz) for _, _, m in levels]
    y_last = y[-1]
    if y_last <= 0:
        raise GreyError(f"{ident}: lightest level has Y = 0")
    level_y = [(v * 65536 + y_last // 2) // y_last for v in y]  # all non-negative: C / is floor
    for i in range(1, n):
        if level_y[i] <= level_y[i - 1]:
            raise GreyError(f"{ident}: level Y not strictly increasing in Q16 at level {i} "
                            f"({level_y[i - 1]} then {level_y[i]})")
    lmin = grey_lstar_q8(level_y[0])
    if not midtone_monotone(lmin):
        raise GreyError(f"{ident}: darkest level L* {lmin / 256:.2f} is too light for the midtone "
                        "curve (epi_grey_panel_init rejects L* >= 66.29)")

    thr = []
    for i in range(n - 1):
        c0 = cbrt_d(level_y[i] / 65536.0)
        c1 = cbrt_d(level_y[i + 1] / 65536.0)
        m = (c0 + c1) / 2.0
        q = int(m * m * m * 65536.0)
        q = level_y[i] if q < level_y[i] else (level_y[i + 1] - 1 if q >= level_y[i + 1] else q)
        thr.append(q)

    kern = kernel_c(sigma)
    if kern != kernel_q15(sigma)[0]:
        raise AssertionError(f"sigma {sigma}: C kernel rule differs from panelgen kernel_q15")
    r = (len(kern) - 1) // 2
    assert r <= MAX_KERNEL_RADIUS and sum(kern) == 32768 and kern == kern[::-1]
    assert level_y[-1] == 65536 and all(a <= t < b for a, t, b in zip(level_y, thr, level_y[1:]))
    return GreyTables(ident=ident, mode=mode, levels=levels, kernel=kern, level_y=level_y,
                      level_thr=thr, lstar_min_q8=lmin)


# -- input -----------------------------------------------------------------------

@dataclass
class Levels:
    path: Path
    raw: bytes
    data: dict

    @property
    def sha256(self) -> str:
        return hashlib.sha256(self.raw).hexdigest()

    def placeholder_reasons(self, panel: GreyPanel) -> list[str]:
        why = []
        if "_placeholder" in self.data or self.data.get("placeholder"):
            why.append("top level")
        for mode in panel.modes:
            e = self.data.get(f"{panel.ident}_{mode}")
            if isinstance(e, dict) and (e.get("placeholder") or (e.get("photocal") or {}).get("placeholder")):
                why.append(f"{panel.ident}_{mode}")
        return why


def load_levels(path: Path) -> Levels:
    raw = Path(path).read_bytes()
    try:
        data = json.loads(raw)
    except json.JSONDecodeError as e:
        raise GreyError(f"{path}: not JSON ({e})") from None
    if not isinstance(data, dict):
        raise GreyError(f"{path}: expected a JSON object of modes")
    return Levels(path=Path(path), raw=raw, data=data)


def _photocal_meta(ident: str, entry: dict, placeholder: bool) -> dict:
    meta = entry.get("photocal")
    if meta is None:
        if placeholder:
            return {}
        raise GreyError(f"{ident}: no photocal block; a measured calibration records its date and "
                        "waveform (design.md \"Grey panels\"). Use a measured levels JSON.")
    for k in ("date", "waveform"):
        if not meta.get(k) or (k == "waveform" and meta[k] == "UNRECORDED" and not placeholder):
            raise GreyError(f"{ident}: photocal.{k} is {meta.get(k)!r}; grey levels depend on the waveform "
                            "(design.md \"Grey panels\"): record it in the levels JSON")
    return meta


# -- C rendering -----------------------------------------------------------------

def _levels_arg(path: Path) -> str:
    """How the header names the JSON: repo-relative if inside the repo, else
    the bare file name (so a temp path never lands in the output)."""
    p = path.resolve()
    try:
        return str(p.relative_to(REPO))
    except ValueError:
        return p.name


def grey_inputs(panel: GreyPanel, lv: Levels) -> dict:
    return {"panel": asdict(panel), "levels_sha256": lv.sha256, "srgb": srgb_inputs(),
            "rules": RULES, "grey_rules": GREY_RULES}


def render_grey(panel: GreyPanel, lv: Levels, allow_placeholder: bool = False) -> tuple[str, list[GreyTables]]:
    """(src/panels/<ident>.c text, derived tables per mode); GreyError if the
    JSON is a placeholder (without allow_placeholder) or invalid."""
    why = lv.placeholder_reasons(panel)
    if why and not allow_placeholder:
        raise GreyError(f"{lv.path}: marked placeholder ({', '.join(why)}); refusing to generate a "
                        "calibration from unmeasured levels (pass --allow-placeholder for tests)")
    pid = panel.ident
    tabs, metas = [], []
    for mode in panel.modes:
        key = f"{pid}_{mode}"
        if key not in lv.data:
            raise GreyError(f"{lv.path}: no {key!r} entry (have {', '.join(sorted(lv.data))})")
        entry = lv.data[key]
        if not isinstance(entry, dict):
            raise GreyError(f"{key}: expected an object")
        tabs.append(derive_grey(panel, mode, entry))
        metas.append(_photocal_meta(key, entry, bool(why)))

    shipped = panel.levels is not None and lv.path.resolve() == (REPO / panel.levels).resolve()
    cmd = f"{COMMAND} {pid}" + ("" if shipped else f" --levels {_levels_arg(lv.path)}")
    cmd += " --allow-placeholder" if why else ""
    what = [f"Grey calibrations {panel.name}: {panel.doc}."]
    if why:
        what += ["", "PLACEHOLDER LEVELS (NOT MEASURED): built with --allow-placeholder. Do not ship."]
    what += ["", f"Levels JSON: {_levels_arg(lv.path)}", f"Levels JSON SHA-256: {lv.sha256}"]
    for t, meta in zip(tabs, metas):
        if meta:
            what.append(f"epi_panel_{t.ident}: photocal {meta.get('format', '?')}, {meta['date']}, "
                        f"waveform {meta['waveform']}, chart {meta.get('chart_version', '?')}, "
                        f"{len(meta.get('photos', []))} photo(s)")
            for w in meta.get("warnings", []):
                what.append(f"  photocal warning: {w}")
        else:
            what.append(f"epi_panel_{t.ident}: no photocal block")
    what += ["",
             "Tables: the same rules as epi_grey_panel_init (src/epi_grey_panel.c) at",
             f"sigma {panel.detail_sigma}; tools/panelgen/tests/test_grey.py checks them against it",
             "byte for byte. See src/epi_panel_tables.h for the Q formats."]
    for t in tabs:
        what.append(f"epi_panel_{t.ident}: {len(t.levels)} levels, darkest L* "
                    f"{t.lstar_min_q8 / 256:.2f}, kernel r {(len(t.kernel) - 1) // 2}.")
    sha = _digest(grey_inputs(panel, lv))
    head = _header(sha, what).replace(f" * Command: {COMMAND}\n", f" * Command: {cmd}\n", 1)

    out = [head, '#include "epaper_image.h"', '#include "../epi_panel_tables.h"', ""]
    for t in tabs:
        sid = t.ident
        out.append(f"static const epi_ink_t {sid}_levels[{len(t.levels)}] = {{")
        for code, nom, mea in t.levels:
            out.append(f"    {{{code}, {_row(nom)}, {_row(mea)}}},")
        out += ["};", ""]
        out.append(f"static const epi_grey_tables_t {sid}_tables = {{")
        out.append("    .base.kind = EPI_PANEL_GREY,")
        out.append(f"    .base.ink_count = {len(t.levels)},")
        out.append(f"    .base.kernel_radius = {(len(t.kernel) - 1) // 2},")
        out.append("    .base.kernel =\n        {\n" + _block(t.kernel, 10, "            ") + "\n        },")
        out.append(f"    .level_y = {_row(t.level_y)},")
        out.append(f"    .level_thr = {_row(t.level_thr)},")
        out += ["};", ""]
        out.append(f"const epi_panel_t epi_panel_{sid} = {{")
        out.append(f'    .name = "{panel.name}",')
        out.append(f"    .width = {panel.width},")
        out.append(f"    .height = {panel.height},")
        out.append(f"    .inks = {sid}_levels,")
        out.append(f"    .ink_count = {len(t.levels)},")
        out.append(f"    .detail_sigma = {_flt(panel.detail_sigma)},")
        out.append("    .presets = epi_grey_presets,")
        out.append("    .preset_count = EPI_GREY_PRESET_COUNT,")
        out.append(f"    .tables = &{sid}_tables.base,")
        out += ["};", ""]
    return "\n".join(out), tabs


def grey_output(panel: GreyPanel, levels_path: Path, allow_placeholder: bool = False):
    """{repo-relative path: (text, tables)} for one grey glass."""
    text, tabs = render_grey(panel, load_levels(levels_path), allow_placeholder)
    return {f"src/panels/{panel.ident}.c": (text, tabs)}
