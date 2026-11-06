"""panelgen: the rendered tables, bad inks and byte-identical regeneration. What the
tables hold is the library's: tests/test_colour_panel.c holds them to the runtime panel."""

import json
import math
import re
import shutil
import subprocess
import sys
from dataclasses import replace

import pytest

from panelgen import PANELGEN_DIR, REPO, PanelError
from panelgen.__main__ import main
from panelgen.gen import (_colour_tables_program, check_presets, derive, kernel_q15, outputs, panel_inputs, q,
                          render_panel, render_srgb)
from panelgen.palette import BUILTIN_TABLES
from panelgen.panels import EL133UF1, PANELS, Panel, Preset


@pytest.fixture(scope="module")
def t():
    return derive(EL133UF1)


def test_q_rounds_half_away_from_zero():
    assert [q(v, 0) for v in (0.5, 1.5, 2.5, -0.5, -1.5, -2.5, 0.49, -0.49)] == [1, 2, 3, -1, -2, -3, 0, 0]
    assert q(1.0, 16) == 65536 and q(-1.0 / 65536 * 0.5, 16) == -1


def test_preset_check():
    check_presets(EL133UF1)
    limits = dict(tone=0.5, detail=0.75, adapt=0.5)
    check_presets(replace(EL133UF1, presets=(Preset("x", **{**limits, "tone": 1.0}, vibrance=3.0, veil=3.0, lift=20.0),)))
    for knob, bad in (("tone", 1.5), ("detail", -0.1), ("adapt", 1.1), ("vibrance", 3.1), ("veil", float("nan")), ("lift", 20.1)):
        panel = replace(EL133UF1, presets=(Preset("x", **{**limits, knob: bad}),))
        with pytest.raises(PanelError, match=f"{EL133UF1.name}: preset x {knob} "):
            check_presets(panel)
    with pytest.raises(PanelError, match="no presets"):
        check_presets(replace(EL133UF1, presets=()))


def _panel_with_inks(monkeypatch, table, entries):
    monkeypatch.setitem(BUILTIN_TABLES, "bad", {table: entries})
    panel = Panel(ident="bad", name="BAD", palette="bad", table=table, width=8, height=8,
                  ink_names=tuple(entries), codes=tuple(range(len(entries))), detail_sigma=1.0,
                  presets=(Preset("Default", tone=0.5, detail=0.75, adapt=0.5),), doc="bad inks")
    monkeypatch.setattr("panelgen.__main__.PANELS", {"bad": panel})
    return panel


@pytest.mark.parametrize("table, entries, message", [
    # Black so light that the midtone curve has no knee.
    ("light", {"black": ((0, 0, 0), (170, 170, 170)), "white": ((255, 255, 255), (240, 240, 240)),
               "red": ((255, 0, 0), (235, 170, 170)), "green": ((0, 255, 0), (170, 235, 170)),
               "blue": ((0, 0, 255), (170, 180, 240))},
     "darkest neutral L\\* is too light for the tone curve"),
    # A strongly tinted black: the neutral axis enters the hull above it, so hues
    # towards it peak in chroma at the very bottom of the neutral range.
    ("tinted", {"black": ((0, 0, 0), (0, 0, 120)), "white": ((255, 255, 255), (240, 240, 240)),
                "yellow": ((255, 255, 0), (230, 220, 40)), "red": ((255, 0, 0), (200, 30, 30)),
                "green": ((0, 255, 0), (30, 170, 40))},
     "a hue peaks too close to the end of the neutral range"),
    ("four", {"black": ((0, 0, 0), (40, 40, 40)), "white": ((255, 255, 255), (200, 200, 200)),
              "red": ((255, 0, 0), (170, 40, 40)), "yellow": ((255, 255, 0), (205, 185, 50))},
     "5 to 16 inks"),
    # A measured colour lighter than the white ink, whose nominal colour is not.
    ("pale-yellow", {"black": ((0, 0, 0), (40, 40, 40)), "white": ((255, 255, 255), (204, 204, 204)),
                     "yellow": ((255, 255, 0), (216, 213, 183)), "red": ((255, 0, 0), (170, 40, 40)),
                     "blue": ((0, 0, 255), (50, 70, 150))},
     "lightest measured ink is not the white ink"),
    ("seventeen", {f"ink{i}": ((i, i, i), (i, i, i)) for i in range(17)}, "5 to 16 inks"),
])
def test_bad_inks_fail_generation(monkeypatch, capsys, tmp_path, table, entries, message):
    panel = _panel_with_inks(monkeypatch, table, entries)
    with pytest.raises(PanelError, match=message):
        derive(panel)
    assert main(["--root", str(tmp_path)]) == 2
    assert re.search(message, capsys.readouterr().err)
    assert not list(tmp_path.iterdir())


def test_bad_sigma_fails_generation():
    for sigma in (0.0, 10.6):
        with pytest.raises(PanelError, match=r"detail sigma outside"):
            derive(replace(EL133UF1, detail_sigma=sigma))


def test_generator_bugs_are_not_reported_as_bad_inks(monkeypatch):
    def bug(_panels):
        raise ValueError("a bug")
    monkeypatch.setattr("panelgen.__main__.outputs", bug)
    with pytest.raises(ValueError, match="a bug"):
        main([])


@pytest.mark.skipif(shutil.which("false") is None, reason="no false")
def test_a_failed_compile_is_a_panel_error(monkeypatch, capsys):
    monkeypatch.setenv("CC", "false")
    _colour_tables_program.cache_clear()
    assert main([]) == 2
    assert "panelgen: false failed to compile colour_tables.c" in capsys.readouterr().err


def test_a_compiler_that_cannot_run_is_a_panel_error(monkeypatch, tmp_path, capsys):
    not_executable = tmp_path / "cc"
    not_executable.write_text("not a program\n")
    not_executable.chmod(0o755)
    monkeypatch.setenv("CC", str(not_executable))
    _colour_tables_program.cache_clear()
    assert main([]) == 2
    assert "panelgen: cannot run the C compiler" in capsys.readouterr().err


def test_no_compiler_says_so(monkeypatch):
    monkeypatch.setenv("CC", "no-such-compiler")
    _colour_tables_program.cache_clear()
    with pytest.raises(PanelError, match="no C compiler"):
        derive(EL133UF1)


@pytest.mark.parametrize("sigma", [1.0, 2.5, 6.0, 10.0])
def test_kernel_symmetric_sums_to_2_15(sigma):
    taps = kernel_q15(sigma)
    assert sum(taps) == 32768 and taps == taps[::-1] and len(taps) == 2 * math.ceil(3 * sigma) + 1


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
    for f in ("kernel", "hull_d", "cusp_lstar"):
        assert _ints_after(text, f) == t.ints[f], f
    for f in ("lin_to_panel_adapt", "ink_oklab", "hull_n", "neutral_q8"):
        assert _ints_after(text, f) == [v for row in t.ints[f] for v in row], f
    assert ".has_neutral = 1," in text


def _with_neutral(tmp_path, data):
    path = tmp_path / "neutral.json"
    path.write_text(json.dumps(data))
    return replace(EL133UF1, neutral=str(path))


def test_neutral_json_is_an_input(tmp_path, t):
    data = json.loads((REPO / EL133UF1.neutral).read_text())
    assert panel_inputs(_with_neutral(tmp_path, data))["neutral"] == data
    data["knots"][5][2] -= 1.0  # b* at L* 57.7
    edited = derive(_with_neutral(tmp_path, data))
    assert render_panel(edited)[0] != render_panel(t)[0]
    # The knot's two segments, L* 52..64, move; nothing else does.
    old, new = t.ints["neutral_q8"], edited.ints["neutral_q8"]
    assert [i for i in range(101) if new[i] != old[i]] == list(range(52, 65))
    assert all(new[i][1] < old[i][1] and new[i][0] == old[i][0] for i in range(52, 65))


def test_no_neutral_json_no_table():
    d = derive(replace(EL133UF1, neutral=None))
    assert d.ints["has_neutral"] == 0 and all(v == [0, 0] for v in d.ints["neutral_q8"])
    assert "neutral" not in render_panel(d)[1].split("*/", 1)[1]


@pytest.mark.parametrize("data, message", [
    ({"panel": "el133uf1", "knots": [[50, 0, 0], [50, 1, 1]]}, r"neutral knot L\* not strictly ascending"),
    ({"panel": "el133uf1", "knots": [[50, 31, 0]]}, r"neutral knot a\* or b\* outside"),
    ({"panel": "el133uf1", "knots": [[50, 0, 0]] * 17}, "more than 16 neutral knots"),
    ({"panel": "other", "knots": []}, "is not for panel el133uf1"),
    ({"panel": "el133uf1", "knots": [[50, 0]]}, r"knots must be \[L\*, a\*, b\*\] numbers"),
])
def test_bad_neutral_fails_generation(tmp_path, data, message):
    with pytest.raises(PanelError, match=message):
        derive(_with_neutral(tmp_path, data))


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
    for rel in ["src/epi_srgb.c", "tools/panelgen/colour_tables.c"] + [f"src/panels/{p}.c" for p in PANELS]:
        r = subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-Iinclude", "-Isrc",
                            "-c", rel, "-o", str(tmp_path / "x.o")], cwd=REPO, capture_output=True, text=True)
        assert r.returncode == 0, rel + "\n" + r.stderr
