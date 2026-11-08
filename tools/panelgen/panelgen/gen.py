"""Render the generated C tables (docs/design.md, "Panel tables").

Colour tables come from the library's own derivation: colour_tables.c, compiled
here, runs epi_colour_panel_init and prints the integers of its tables. The sRGB
tables are computed here, and grey.py computes grey's.
Quantisation is q(x, bits) = sign(x) floor(|x| 2^bits + 1/2), half away from zero.
"""

from __future__ import annotations

import atexit
import hashlib
import json
import math
import os
import shlex
import shutil
import subprocess
import tempfile
from dataclasses import asdict, dataclass
from functools import lru_cache
from pathlib import Path

from . import PANELGEN_DIR, REPO, PanelError
from .colour import M_RGB_TO_XYZ, srgb_to_linear
from .palette import BUILTIN_TABLES
from .panels import Panel

COMMAND = "cd tools/panelgen && python -m panelgen"
FORMAT_VERSION = 2  # bump when the emitted layout changes (part of the input hash)
# Part of the hash of src/epi_srgb.c and of every grey glass.
RULES = {
    "round": "q(x, bits) = sign(x) * floor(|x| * 2^bits + 0.5)",
    "kernel": "w_i = exp(-i^2 / (2 sigma^2)), i = -r..r, r = ceil(3 sigma); normalised; "
              "q(w, 15); residual 32768 - sum added to the centre tap",
    "format_version": FORMAT_VERSION,
}
COLOUR_DERIVATION = REPO / "src" / "epi_colour_panel.c"
COLOUR_SOURCES = (PANELGEN_DIR / "colour_tables.c", COLOUR_DERIVATION, REPO / "src" / "epi_intent.c",
                  REPO / "src" / "epi_math.c", REPO / "src" / "epi_srgb.c")


def q(x: float, bits: int) -> int:
    """Round x * 2^bits half away from zero, as a Python int."""
    x = float(x)
    if not math.isfinite(x):
        raise ValueError(f"cannot quantise {x}")
    m = math.floor(abs(x) * 2.0**bits + 0.5)
    return int(-m if x < 0 else m)


# -- input hash ------------------------------------------------------------------

def _digest(obj) -> str:
    blob = json.dumps(obj, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(blob).hexdigest()


def srgb_inputs() -> dict:
    return {
        "M_RGB_TO_XYZ": M_RGB_TO_XYZ,
        "eotf": "u/255; c <= 0.04045 ? c/12.92 : ((c+0.055)/1.055)^2.4 (panelgen.colour.srgb_to_linear)",
        "formats": {"epi_srgb_to_lin_q16": 16, "epi_srgb_to_xyz_q16": 16},
        "rules": RULES,
    }


def neutral_json(panel: Panel) -> dict | None:
    """The panel's neutral-cast JSON, parsed, or None: its knots are [L*, a*, b*] numbers."""
    if panel.neutral is None:
        return None
    path = REPO / panel.neutral
    try:
        data = json.loads(path.read_bytes())
    except (OSError, json.JSONDecodeError) as e:
        raise PanelError(f"{panel.name}: neutral JSON {panel.neutral}: {e}") from None
    if not isinstance(data, dict) or data.get("panel") != panel.ident:
        raise PanelError(f"{panel.name}: neutral JSON {panel.neutral} is not for panel {panel.ident}")
    knots = data.get("knots")
    if not isinstance(knots, list) or not all(
            isinstance(k, list) and len(k) == 3 and all(isinstance(v, (int, float)) and not isinstance(v, bool)
                                                        for v in k) for k in knots):
        raise PanelError(f"{panel.name}: neutral JSON {panel.neutral}: knots must be [L*, a*, b*] numbers")
    return data


def panel_inputs(panel: Panel) -> dict:
    """The panel definition, its ink table, its neutral-cast JSON (parsed, so line endings don't count) and the
    format version."""
    entries = BUILTIN_TABLES[panel.palette][panel.table]
    return {
        "panel": asdict(panel),
        "inks": {k: [list(v[0]), list(v[1])] for k, v in entries.items()},
        "neutral": neutral_json(panel),
        "format_version": FORMAT_VERSION,
    }


# -- derivation ------------------------------------------------------------------

@dataclass
class Derived:
    """A colour panel and its tables, as colour_tables.c prints them."""
    panel: Panel
    ints: dict


@lru_cache(maxsize=None)
def _colour_tables_program() -> Path:
    """Compile colour_tables.c with the library sources it needs, as the repo's CMake compiles them."""
    cc = shlex.split(os.environ.get("CC") or "cc")
    if shutil.which(cc[0]) is None:
        raise PanelError(f"no C compiler ({cc[0]} not found; set CC): colour tables come from "
                         "src/epi_colour_panel.c, compiled and run here")
    build = Path(tempfile.mkdtemp(prefix="panelgen-"))
    atexit.register(shutil.rmtree, build, ignore_errors=True)
    program = build / "colour_tables"
    command = [*cc, "-std=c11", "-O2", "-ffp-contract=off", "-DEPI_FP_CONTRACT_OFF",
               f"-I{REPO / 'include'}", f"-I{REPO / 'src'}", *map(str, COLOUR_SOURCES), "-o", str(program)]
    try:
        subprocess.run(command, check=True, capture_output=True, text=True)
    except subprocess.CalledProcessError as e:
        raise PanelError(f"{cc[0]} failed to compile colour_tables.c (exit status {e.returncode})\n"
                         f"{e.stderr.strip()}") from e
    except OSError as e:
        raise PanelError(f"cannot run the C compiler {cc[0]}: {e.strerror or e}") from e
    return program


def derive(panel: Panel) -> Derived:
    entries = BUILTIN_TABLES[panel.palette][panel.table]
    if tuple(entries) != panel.ink_names:
        raise PanelError(f"{panel.name}: ink order {tuple(entries)} != {panel.ink_names}")
    if len(panel.codes) != len(panel.ink_names):
        raise PanelError(f"{panel.name}: {len(panel.codes)} codes for {len(panel.ink_names)} inks")
    check_presets(panel)
    # Each ink as measured R G B then nominal R G B; entries hold (nominal, measured).
    inks = [str(v) for nominal, measured in entries.values() for v in (*measured, *nominal)]
    neutral = neutral_json(panel)
    knots = neutral["knots"] if neutral else []
    args = [repr(panel.detail_sigma), str(len(knots)), *(repr(float(v)) for k in knots for v in k), *inks]
    r = subprocess.run([_colour_tables_program(), *args], capture_output=True, text=True)
    if r.returncode == 1:  # the inks, sigma or knots; anything else is a bug
        raise PanelError(f"{panel.name}: {r.stderr.strip()}")
    r.check_returncode()
    return Derived(panel=panel, ints=json.loads(r.stdout))


def kernel_q15(sigma: float) -> list[int]:
    """The kernel rule (RULES), which grey.py's transcription of the C must reproduce."""
    radius = math.ceil(3.0 * sigma)
    w = [math.exp(-(i * i) / (2.0 * sigma * sigma)) for i in range(-radius, radius + 1)]
    total = 0.0
    for v in w:
        total += v
    taps = [q(v / total, 15) for v in w]
    taps[radius] += 32768 - sum(taps)
    return taps


# -- limits of the C ---------------------------------------------------------------

MAX_INKS = 16
MAX_KERNEL_RADIUS = 32
_MID_DELTA_MIN = 0.3371


def midtone_monotone(lstar_min_q8: int) -> bool:
    """epi_midtone_monotone."""
    lmin = lstar_min_q8 / 256.0
    k = lmin / (1.0 - _MID_DELTA_MIN)
    k = k if k > 60.0 else 60.0
    delta = (k - lmin) / k
    fa, fb = 0.15 / delta, 1.0 / delta
    return lmin >= 0.0 and k < 100.0 and fa * fa + fb * fb <= 9.0


def check_presets(panel: Panel) -> None:
    """What epi_intent_init asks of a preset; epi_converter_init needs at least one."""
    if not panel.presets:
        raise PanelError(f"{panel.name}: no presets")
    for pr in panel.presets:
        for knob, hi in (("tone", 1.0), ("detail", 1.0), ("adapt", 1.0), ("vibrance", 3.0), ("veil", 3.0), ("lift", 20.0)):
            if not 0.0 <= getattr(pr, knob) <= hi:
                raise PanelError(f"{panel.name}: preset {pr.name} {knob} {getattr(pr, knob)} outside 0..{hi}")


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
    return [q(srgb_to_linear(u), 16) for u in range(256)], [[q(v, 16) for v in row] for row in M_RGB_TO_XYZ]


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
    entries = BUILTIN_TABLES[p.palette][p.table]
    pid = p.ident
    head = _header(sha, [
        f"Panel calibration {p.name}: {p.doc}.",
        f"Inks: ink table {p.palette}/{p.table} (tools/panelgen/panelgen/palette.py).",
        *([f"Neutral cast: {p.neutral}."] if p.neutral else []),
        "Tables: epi_colour_tables_derive (src/epi_colour_panel.c) and",
        "epi_gaussian_kernel_q15; see src/epi_panel_tables.h for every field's Q format.",
        "The kernel holds tap -r at index 0 through tap +r at index 2r; entries beyond are 0.",
    ])
    out = [head, '#include "epaper_image.h"', '#include "../epi_panel_tables.h"', ""]
    out.append(f"static const epi_ink_t {pid}_inks[{len(p.ink_names)}] = {{")
    for name, code in zip(p.ink_names, p.codes):
        nominal, measured = entries[name]
        out.append(f"    {{{code}, {_row(nominal)}, {_row(measured)}}}, /* {name} */")
    out += ["};", ""]
    for pr in p.presets:
        out.append(f"static const epi_intent_t {pid}_intent_{pr.name.lower()} = {{")
        out.append(f"    .tone = {_flt(pr.tone)},")
        out.append(f"    .detail = {_flt(pr.detail)},")
        out.append(f"    .adapt = {_flt(pr.adapt)},")
        out.append(f"    .vibrance = {_flt(pr.vibrance)},")
        out.append(f"    .veil = {_flt(pr.veil)},")
        out.append(f"    .lift = {_flt(pr.lift)},")
        out += ["};", ""]
    out.append(f"static const epi_preset_t {pid}_presets[{len(p.presets)}] = {{")
    for pr in p.presets:
        out.append(f'    {{"{pr.name}", &{pid}_intent_{pr.name.lower()}}},')
    out += ["};", ""]

    i64 = lambda v: f"INT64_C({v})"  # noqa: E731
    out.append(f"static const epi_colour_tables_t {pid}_tables = {{")
    out.append("    .base.kind = EPI_PANEL_COLOUR,")
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
    if I["has_neutral"]:
        out.append(f"    .has_neutral = {I['has_neutral']},")
        out.append("    .neutral_q8 =\n        {\n" + _block(I["neutral_q8"], 8, "            ", _row) + "\n        },")
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
