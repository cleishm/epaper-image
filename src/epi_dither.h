/*
 * Error diffusion, stage S7 of docs/design.md, "Error diffusion" (colour panels):
 * target linear sRGB rows → palette indices (0..ink_count−1, the order of
 * the panel's inks; the converter maps indices to codes). Grey panels
 * (docs/design.md, "Grey panels") use the one-channel row function,
 * epi_dither_grey_row; size and init dispatch on the tables' kind.
 */
#ifndef EPI_DITHER_H
#define EPI_DITHER_H

#include "epaper_image.h"
#include "epi_panel_tables.h"

#include <stddef.h>

typedef struct {
    const epi_panel_tables_t* tables;
    uint16_t width;
    uint32_t row; /* rows done so far; even rows run left-to-right */
    void* err;    /* caller memory, epi_dither_err_size(tables, width) bytes */
    /* Grey: per level gap g, the targets t in [toe_id_lo[g], toe_id_hi[g]]
     * where the coverage toe is the identity (lo > hi: none). Q16. */
    int32_t toe_id_lo[EPI_MAX_INKS - 1];
    int32_t toe_id_hi[EPI_MAX_INKS - 1];
} epi_dither_ctx_t;

/* Bytes of error memory for these tables (three Q16 channels, or one for
 * grey) and this width. */
size_t epi_dither_err_size(const epi_panel_tables_t* tables, uint16_t width);

/* Zeroes the error memory and, for grey, builds the toe's identity bands.
 * err_mem must be suitably aligned for int32_t. */
void epi_dither_init(epi_dither_ctx_t* ctx, const epi_panel_tables_t* tables, uint16_t width, void* err_mem);

/* Dithers the next row (rows must arrive in order, top to bottom). */
void epi_dither_row(epi_dither_ctx_t* ctx, const int32_t (*target_lin_q16)[3], uint8_t* idx_out);

/* Grey: dithers the next row of target Y (Q16, relative to the lightest
 * level) to level indices, darkest = 0. Each target first goes through the
 * per-gap coverage toe (docs/design.md, "Grey panels"). */
void epi_dither_grey_row(epi_dither_ctx_t* ctx, const int32_t* target_y_q16, uint8_t* idx_out);

/* ---- Exported for the host tests (tests/test_dither.c, tests/test_grey.c);
 * not part of the library's interface. ---- */

/* Padding cells on either side of the error row. */
#define EPI_DITHER_ERR_PAD 1

void epi_dither_damp_coeffs(const epi_colour_tables_t* tb, const int32_t t[3], int32_t* kl, int32_t* kc);
void epi_dither_damp(const epi_colour_tables_t* tb, const int32_t e[3], int32_t kl, int32_t kc, int32_t out[3]);
void epi_dither_fs_split(int32_t e, int32_t parts[4]);
int32_t epi_dither_grey_cutl(int32_t t);
int32_t epi_dither_grey_toe_q16(int32_t c);
int32_t epi_dither_grey_toe_full(const epi_grey_tables_t* tb, int32_t t);
int32_t epi_dither_grey_toe(const epi_dither_ctx_t* ctx, int32_t t);

#endif /* EPI_DITHER_H */
