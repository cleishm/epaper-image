"""panelgen: generate the C library's constant panel tables.

palette.py holds the ink tables and panels.py the panel definitions. gen.py
renders colour panels from the library's own derivation (colour_tables.c, in
this tool's directory), and the sRGB tables from colour.py; grey.py computes
and renders grey calibrations. See README.md.
"""

from __future__ import annotations

from pathlib import Path

PANELGEN_DIR = Path(__file__).resolve().parent.parent
REPO = PANELGEN_DIR.parent.parent


class PanelError(ValueError):
    """The panel or its inks cannot become C tables (nothing is written)."""
