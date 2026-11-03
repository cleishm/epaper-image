"""panelgen: generate the C library's constant panel tables.

The colour maths: colour.py (sRGB, XYZ, Lab, Bradford), palette.py (the ink
tables), gamut.py (the ink hull, Bradford/BPC) and model.py (OKLab, the cusp
table, partial adaptation). gen.py and grey.py only quantise and write C.
See README.md.
"""

from __future__ import annotations

from pathlib import Path

PANELGEN_DIR = Path(__file__).resolve().parent.parent
REPO = PANELGEN_DIR.parent.parent


class PanelError(ValueError):
    """The panel or its inks cannot become C tables (nothing is written)."""
