/*
 * The panel ID (epi_panel_t.id; docs/design.md, "Panel ID"). Its own
 * translation unit, so firmware that links only const panels does not link
 * the hash.
 */
#ifndef EPI_PANEL_ID_H
#define EPI_PANEL_ID_H

#include "epaper_image.h"

#include <stddef.h>
#include <stdint.h>

#define EPI_PANEL_ID_LEN 16

/*
 * The ID of a calibration: the first 16 bytes of the SHA-256 of
 *
 *   "epi-panel-id-v1" (15 ASCII bytes, no NUL)
 *   u8 kind: 1 grey, 2 colour (from EPI_PANEL_GREY or EPI_PANEL_COLOUR)
 *   u16 width, u16 height
 *   u8 ink count, then per ink: u8 code, u8 measured r, g, b
 *   u32 detail_sigma's binary32 bits
 *   u8 knot count, then per knot: u32 lstar, a, b binary32 bits (-0 as +0)
 *
 * integers little-endian. Names and nominal colours are not part of it. At most
 * 16 inks and 16 knots (init's limits); knots may be NULL when knot_count is 0.
 */
void epi_panel_id(
    uint8_t id[EPI_PANEL_ID_LEN],
    uint8_t kind,
    uint16_t width,
    uint16_t height,
    const epi_ink_t* inks,
    size_t ink_count,
    float detail_sigma,
    const epi_neutral_knot_t* knots,
    size_t knot_count
);

#endif /* EPI_PANEL_ID_H */
