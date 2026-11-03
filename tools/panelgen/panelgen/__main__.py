"""Generate (or --check) the C panel tables. See README.md.

    .venv/bin/python -m panelgen [--check]
    .venv/bin/python -m panelgen gdey075t7 [--levels LEVELS.json] [--check]

Without a panel: src/epi_srgb.c and every colour panel. With a grey panel:
src/panels/<panel>.c from the measured levels JSON (README.md, "Grey calibrations");
--levels defaults to the panel's shipped JSON (panels.py `levels`).
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from . import REPO, PanelError
from .gen import outputs
from .grey import GreyError, grey_output
from .panels import GREY_PANELS, PANELS


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="panelgen", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("panel", nargs="?", choices=sorted(GREY_PANELS),
                    help="grey panel to generate from --levels (default: sRGB tables and colour panels)")
    ap.add_argument("--levels", type=Path, help="measured grey levels JSON (README.md, \"Grey calibrations\")")
    ap.add_argument("--allow-placeholder", action="store_true",
                    help="accept a levels JSON marked placeholder (tests only; never ship the result)")
    ap.add_argument("--check", action="store_true",
                    help="regenerate in memory and compare with the files on disk; write nothing")
    ap.add_argument("--root", type=Path, default=REPO,
                    help="tree to write into or check against (default: the repo)")
    a = ap.parse_args(argv)
    if a.panel is None:
        if a.levels or a.allow_placeholder:
            ap.error("--levels and --allow-placeholder need a grey panel")
        try:
            res = outputs(PANELS.values())
        except PanelError as e:
            print(f"panelgen: {e}", file=sys.stderr)
            return 2
    else:
        gp = GREY_PANELS[a.panel]
        if a.levels is None:
            if gp.levels is None:
                ap.error(f"{a.panel} needs --levels (no shipped levels JSON in panels.py)")
            a.levels = REPO / gp.levels
        try:
            res = grey_output(gp, a.levels, a.allow_placeholder)
        except (GreyError, OSError) as e:
            print(f"panelgen: {e}", file=sys.stderr)
            return 2
    stale = []
    for rel, (text, t) in res.items():
        path = a.root / rel
        data = text.encode()
        if a.check:
            if not path.exists() or path.read_bytes() != data:
                stale.append(rel)
        else:
            path.parent.mkdir(parents=True, exist_ok=True)
            if not path.exists() or path.read_bytes() != data:
                path.write_bytes(data)
                print(f"wrote {rel}")
            else:
                print(f"unchanged {rel}")
        if isinstance(t, list):  # grey: one GreyTables per mode
            for g in t:
                print(f"  epi_panel_{g.ident}: {len(g.levels)} levels, level_y {g.level_y}, "
                      f"thr {g.level_thr}, darkest L* {g.lstar_min_q8 / 256:.2f}, "
                      f"kernel r {(len(g.kernel) - 1) // 2}")
        elif t is not None:
            d, I = t.diag, t.ints
            nz = [v for v in d["hull_nudges"] if v]
            print(f"  {t.panel.name}: {I['hull_face_count']} faces, d nudged on {len(nz)} "
                  f"(max {max(d['hull_nudges'])} Q32 LSB); L* {I['lstar_min']}..{I['lstar_max']} Q8; "
                  f"kernel r {I['kernel_radius']}, centre {I['kernel'][I['kernel_radius']]}, "
                  f"residual {d['kernel_residual']:+d}; white OKLab Q16 path err {d['white_oklab_lsb_err']} LSB")
    if a.check:
        if stale:
            print("stale: " + ", ".join(stale), file=sys.stderr)
            return 1
        print(f"up to date: {', '.join(res)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
