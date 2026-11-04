"""Grey calibrations (panelgen/grey.py): the equivalence gate against
epi_grey_panel_init, the placeholder refusal, determinism and --check."""

import hashlib
import json
import math
import random
import shutil
import subprocess
import sys
from fractions import Fraction

import pytest

from panelgen import PANELGEN_DIR, REPO
from panelgen.gen import kernel_q15
from panelgen.grey import GreyError, _f32, cbrt_d, derive_grey, exp_d, kernel_c, load_levels, render_grey
from panelgen.panels import GDEY075T7, GREY_PANELS

PLACEHOLDER = PANELGEN_DIR / "tests" / "grey-levels-placeholder.json"
OUT = "src/panels/gdey075t7.c"
CC = shutil.which("cc")
needs_cc = pytest.mark.skipif(CC is None, reason="no C compiler")
# The library's own flags that matter for the init-time double code (CMakeLists.txt).
CFLAGS = ["-std=c11", "-O2", "-ffp-contract=off"]
INCLUDES = ["-I", str(REPO / "include"), "-I", str(REPO / "src"), "-I", str(REPO / "src" / "panels")]


def panelgen(*args, cwd=PANELGEN_DIR):
    return subprocess.run([sys.executable, "-m", "panelgen", *map(str, args)], cwd=cwd,
                          capture_output=True, text=True)


def photocal(date="2026-10-30", waveform="otp-4grey@idfxx-abc1234"):
    return {"format": "photocal-grey/1", "date": date, "waveform": waveform, "chart_version": 1,
            "photos": ["a.dng", "b.dng"], "warnings": []}


def mode_entry(perceived, codes=None, meta=True):
    n = len(perceived)
    nominal = [[round(255 * j / max(1, n - 1))] * 3 for j in range(n)]
    e = {"nominal": nominal, "perceived": perceived, "codes": codes or list(range(n))}
    if meta:
        e["photocal"] = photocal()
    return e


def random_ramp(rng, n):
    """n strictly increasing (after Q16) sRGB8 levels, slightly tinted, black L* < 66."""
    while True:
        v = sorted(rng.sample(range(0, 256), n))
        lv = [[min(255, x + rng.randint(0, 3)), x, max(0, x - rng.randint(0, 3))] for x in v]
        e = mode_entry(lv)
        try:
            derive_grey(GDEY075T7, "x", e)
            return e
        except GreyError:
            continue


def write_json(path, data):
    path.write_text(json.dumps(data, indent=1) + "\n")
    return path


def levels_json(tmp_path, name, mono, grey4):
    return write_json(tmp_path / name, {"gdey075t7_mono": mono, "gdey075t7_grey4": grey4})


# -- the C mirrors ---------------------------------------------------------------

def test_kernel_c_matches_panelgen_kernel_for_every_centi_sigma():
    # epi_grey_panel.c claims it reproduces kernel_q15 for every float sigma = k/100.
    for k in range(1, 1051):
        s = k / 100
        assert kernel_c(s) == kernel_q15(_f32(s)), s


def test_cbrt_exp_mirrors_are_accurate():
    rng = random.Random(7)
    for _ in range(20000):
        x = rng.uniform(1.0, 2.0) * 2.0 ** rng.randint(-16, 0)
        c = cbrt_d(x)
        u = Fraction(math.ulp(c))  # within 1 ulp of the true root; libm cbrt can be further off
        assert (Fraction(c) - u) ** 3 < Fraction(x) < (Fraction(c) + u) ** 3
        e = -rng.uniform(0.0, 50.0)
        assert abs(exp_d(e) - math.exp(e)) <= math.ulp(math.exp(e))
    assert cbrt_d(0.0) == 0.0 and cbrt_d(0.125) == 0.5 and exp_d(0.0) == 1.0
    assert exp_d(-0.0) == 1.0 and exp_d(-745.1) == 2.0 ** -1074 and exp_d(-2.0 ** 297) == 0.0


# -- refusals --------------------------------------------------------------------

def test_refuses_placeholder_without_flag(tmp_path):
    r = panelgen("gdey075t7", "--levels", PLACEHOLDER, "--root", tmp_path)
    assert r.returncode == 2 and "placeholder" in r.stderr
    assert not (tmp_path / OUT).exists() and not any(tmp_path.iterdir())
    r = panelgen("gdey075t7", "--levels", PLACEHOLDER, "--root", tmp_path, "--allow-placeholder")
    assert r.returncode == 0, r.stderr
    text = (tmp_path / OUT).read_text()
    assert "PLACEHOLDER LEVELS (NOT MEASURED)" in text and "--allow-placeholder" in text


def test_per_mode_placeholder_flag_alone_is_refused(tmp_path):
    e = mode_entry([[20, 20, 20], [200, 200, 200]], meta=False)
    e["placeholder"] = True
    p = levels_json(tmp_path, "l.json", e, mode_entry([[20, 20, 20], [90, 90, 90], [150, 150, 150], [200, 200, 200]]))
    with pytest.raises(GreyError, match="placeholder"):
        render_grey(GDEY075T7, load_levels(p))


@pytest.mark.parametrize("mono, why", [
    ([[20, 20, 20], [20, 20, 20]], "not strictly increasing"),
    ([[200, 200, 200], [100, 100, 100]], "not strictly increasing"),
    ([[150, 150, 150], [200, 200, 200]], "too light"),
    ([[0, 0, 0], [0, 0, 0]], "Y = 0"),
    ([[20, 20, 20]], "levels"),
    ([[20, 20, 256], [200, 200, 200]], "0..255"),
])
def test_rejects_what_epi_grey_panel_init_rejects(tmp_path, mono, why):
    grey4 = mode_entry([[20, 20, 20], [90, 90, 90], [150, 150, 150], [200, 200, 200]])
    p = levels_json(tmp_path, "l.json", mode_entry(mono), grey4)
    r = panelgen("gdey075t7", "--levels", p, "--root", tmp_path / "out")
    assert r.returncode == 2 and why in r.stderr, r.stderr
    assert not (tmp_path / "out").exists()


@pytest.mark.parametrize("edit, why", [
    (lambda d: d.pop("gdey075t7_grey4"), "no 'gdey075t7_grey4'"),
    (lambda d: d["gdey075t7_mono"].pop("photocal"), "no photocal block"),
    (lambda d: d["gdey075t7_mono"]["photocal"].update(waveform="UNRECORDED"), "waveform"),
])
def test_rejects_incomplete_json(tmp_path, edit, why):
    d = {"gdey075t7_mono": mode_entry([[20, 20, 20], [200, 200, 200]]),
         "gdey075t7_grey4": mode_entry([[20, 20, 20], [90, 90, 90], [150, 150, 150], [200, 200, 200]])}
    edit(d)
    r = panelgen("gdey075t7", "--levels", write_json(tmp_path / "l.json", d), "--root", tmp_path / "out")
    assert r.returncode == 2 and why in r.stderr, r.stderr
    assert not (tmp_path / "out").exists()


# -- determinism and --check -----------------------------------------------------

def test_rerun_is_byte_identical_and_check(tmp_path):
    roots = [tmp_path / "a", tmp_path / "b"]
    for root in roots:
        r = panelgen("gdey075t7", "--levels", PLACEHOLDER, "--allow-placeholder", "--root", root)
        assert r.returncode == 0, r.stderr
    a, b = ((root / OUT).read_bytes() for root in roots)
    assert a == b
    sha = hashlib.sha256(PLACEHOLDER.read_bytes()).hexdigest()
    assert f"Levels JSON SHA-256: {sha}" in a.decode()

    chk = ("gdey075t7", "--levels", PLACEHOLDER, "--allow-placeholder", "--check", "--root")
    assert panelgen(*chk, roots[0]).returncode == 0
    assert panelgen(*chk, tmp_path / "empty").returncode == 1  # missing file is stale
    # an edited JSON (same levels, different bytes) is stale: the header records its hash
    edited = tmp_path / "grey-levels-placeholder.json"
    edited.write_bytes(PLACEHOLDER.read_bytes() + b"\n")
    r = panelgen("gdey075t7", "--levels", edited, "--allow-placeholder", "--check", "--root", roots[0])
    assert r.returncode == 1 and "stale: " + OUT in r.stderr
    # --check writes nothing
    (roots[1] / OUT).unlink()
    assert panelgen(*chk, roots[1]).returncode == 1 and not (roots[1] / OUT).exists()


@pytest.mark.parametrize("panel", sorted(GREY_PANELS))
def test_committed_grey_files_up_to_date(panel):
    p = GREY_PANELS[panel]
    if p.levels is None:
        assert not (REPO / f"src/panels/{p.ident}.c").exists(), "shipped without panels.py `levels`"
        pytest.skip(f"{panel}: not shipped yet (panels.py levels is None)")
    r = panelgen(panel, "--check")
    assert r.returncode == 0, r.stdout + r.stderr


def test_colour_path_unchanged():
    r = panelgen("--check")
    assert r.returncode == 0, r.stdout + r.stderr


# -- the equivalence gate --------------------------------------------------------

@pytest.fixture(scope="module")
def libobjs(tmp_path_factory):
    """The library's sources compiled once, as the repo's CMake compiles them."""
    if CC is None:
        pytest.skip("no C compiler")
    d = tmp_path_factory.mktemp("lib")
    # Colour panels only: a committed grey glass would clash with the one generated here.
    grey = {f"{g}.c" for g in GREY_PANELS}
    srcs = sorted((REPO / "src").glob("*.c")) + sorted(
        s for s in (REPO / "src" / "panels").glob("*.c") if s.name not in grey)
    objs = []
    for s in srcs:
        o = d / (s.stem + ".o")
        r = subprocess.run([CC, *CFLAGS, *INCLUDES, "-c", str(s), "-o", str(o)], capture_output=True, text=True)
        assert r.returncode == 0, f"{s}\n{r.stderr}"
        objs.append(o)
    return objs


def _cases_header(levels_path) -> str:
    """The levels straight from the JSON (not via the generator), as C."""
    data = json.loads(levels_path.read_text())
    p = GDEY075T7
    out = [f'#define GREY_NAME "{p.name}"', f"#define GREY_WIDTH {p.width}", f"#define GREY_HEIGHT {p.height}",
           f"#define GREY_SIGMA {p.detail_sigma!r}f"]
    for mode in p.modes:
        e = data[f"{p.ident}_{mode}"]
        out.append(f"extern const epi_panel_t epi_panel_{p.ident}_{mode};")
        rows = ", ".join(f"{{{c}, {{{n[0]}, {n[1]}, {n[2]}}}, {{{m[0]}, {m[1]}, {m[2]}}}}}"
                         for c, n, m in zip(e["codes"], e["nominal"], e["perceived"]))
        out.append(f"static const epi_ink_t lv_{mode}[] = {{{rows}}};")
    out.append("static const struct grey_case cases[] = {")
    for mode in p.modes:
        out.append(f'    {{"{p.ident}_{mode}", &epi_panel_{p.ident}_{mode}, lv_{mode}, '
                   f"sizeof lv_{mode} / sizeof lv_{mode}[0]}},")
    out.append("};")
    return "\n".join(out) + "\n"


def _gate(tmp_path, libobjs, levels_path, allow_placeholder, tamper=None):
    """Generate, compile with the library and run grey_equiv; returns (rc, stdout)."""
    root = tmp_path / "root"
    args = ["gdey075t7", "--levels", levels_path, "--root", root] + (["--allow-placeholder"] if allow_placeholder else [])
    r = panelgen(*args)
    assert r.returncode == 0, r.stderr
    gen = root / OUT
    if tamper:
        gen.write_text(tamper(gen.read_text()))
    # the generated file on its own, warnings as errors
    r = subprocess.run([CC, *CFLAGS, "-Wall", "-Wextra", "-Wpedantic", "-Werror", *INCLUDES, "-c", str(gen),
                        "-o", str(tmp_path / "gen.o")], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    (tmp_path / "grey_equiv_cases.h").write_text(_cases_header(levels_path))
    exe = tmp_path / "grey_equiv"
    r = subprocess.run([CC, *CFLAGS, "-Wall", "-Wextra", *INCLUDES, "-I", str(tmp_path),
                        str(PANELGEN_DIR / "tests" / "grey_equiv.c"), str(tmp_path / "gen.o"),
                        *map(str, libobjs), "-lm", "-o", str(exe)], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    r = subprocess.run([str(exe)], capture_output=True, text=True)
    print(r.stdout)
    if tamper is None:
        assert r.returncode == 0, r.stdout + r.stderr
        lines = r.stdout.splitlines()
        assert len(lines) == len(GDEY075T7.modes) and all(ln.startswith("OK ") for ln in lines), r.stdout
    return r.returncode, r.stdout


@needs_cc
def test_gate_placeholder_matches_runtime(tmp_path, libobjs):
    _gate(tmp_path, libobjs, PLACEHOLDER, allow_placeholder=True)


@needs_cc
@pytest.mark.parametrize("seed, n_mono, n_grey", [(1, 2, 4), (2, 3, 8), (3, 5, 16), (4, 2, 13)])
def test_gate_random_levels_match_runtime(tmp_path, libobjs, seed, n_mono, n_grey):
    rng = random.Random(seed)
    mono = random_ramp(rng, n_mono)
    grey4 = random_ramp(rng, n_grey)
    _gate(tmp_path, libobjs, levels_json(tmp_path, f"levels-{seed}.json", mono, grey4), allow_placeholder=False)


@needs_cc
def test_gate_edge_levels_match_runtime(tmp_path, libobjs):
    # pure black (level_y 0, cbrt(0)), codes out of order, adjacent levels one Q16 step apart-ish
    mono = mode_entry([[0, 0, 0], [255, 255, 255]], codes=[1, 0])
    grey4 = mode_entry([[0, 0, 0], [1, 1, 1], [2, 2, 2], [255, 255, 255]], codes=[3, 2, 1, 0])
    _gate(tmp_path, libobjs, levels_json(tmp_path, "edge.json", mono, grey4), allow_placeholder=False)


@needs_cc
@pytest.mark.parametrize("old, new, field", [
    (".level_thr = {10666, 26259, 49378}", ".level_thr = {10666, 26260, 49378}", "level_thr"),
    (".level_y = {5485, 18378", ".level_y = {5486, 18378", "level_y"),
    ("29, 52, 89,", "29, 53, 88,", "kernel"),
    (".preset_count = EPI_GREY_PRESET_COUNT,", ".preset_count = 2,", "presets"),
])
def test_gate_catches_a_one_lsb_change(tmp_path, libobjs, old, new, field):
    def tamper(text):
        assert old in text
        return text.replace(old, new, 1)
    rc, out = _gate(tmp_path, libobjs, PLACEHOLDER, allow_placeholder=True, tamper=tamper)
    assert rc != 0 and f": {field}" in out, out
