# Changelog

All notable changes to **epaper-image** are documented in this file. The project
follows [semantic versioning](https://semver.org/).

## v0.1.1

`epi_panel_t` can now be forward-declared, so a header can use it without
including `<epaper_image.h>`: `typedef struct epi_panel epi_panel_t;`. Its fields
and layout are unchanged, existing code still compiles, and panel output is
unchanged.

Also tidies the remaining American spellings in code comments and docs. The
public API was already British and doesn't change.

## v0.1.0

First release of epaper-image, a C11 library that converts images for e-paper
panels: streaming resampling, tone adjustment, palette matching and error
diffusion.

Built-in panels: EL133UF1 (Spectra 6) and GDEY075T7 (mono and 4 greys).
