/*
 * Rendering intent, stages S1–S6 of docs/design.md, "Rendering intent" (colour panels):
 * source sRGB8 → in-hull target in linear sRGB of the measured inks; and
 * the grey pipeline (docs/design.md, "Grey panels"): source sRGB8 → target Y relative to the
 * lightest level.
 *
 * Row-oriented. The converter owns the row rings and calls, per row:
 *   epi_intent_lstar_row  (S1: L*src, the blur input)
 *   epi_blur_h            (S2, horizontal pass, as each row arrives)
 *   epi_blur_v            (S2, vertical pass, once 2r+1 rows are available)
 *   epi_intent_row        (colour: S1 recomputed + S3–S6, given the row's base)
 *   epi_intent_grey_row   (grey: S3–S6 from the stored L*src and the base)
 */
#ifndef EPI_INTENT_H
#define EPI_INTENT_H

#include "epaper_image.h"
#include "epi_panel_tables.h"

/* Tone/gain table over base L*: 0..100 in steps of 1/4 L*. */
#define EPI_TONE_TABLE_LEN 401

/* Plain struct of fixed size, so the converter can embed it in its
 * workspace. Holds no pointers except to the (const) panel tables. */
typedef struct {
    const epi_panel_tables_t* tables;
    int32_t tone_t_q8[EPI_TONE_TABLE_LEN];     /* T(base), L* Q8 */
    int32_t tone_g_q16[EPI_TONE_TABLE_LEN];    /* g(base) = T' + d(1 − T'), Q16 */
    int32_t unlifted_t_q8[EPI_TONE_TABLE_LEN]; /* colour, lift > 0: T and g at lift 0, for saturated colours */
    int32_t unlifted_g_q16[EPI_TONE_TABLE_LEN];
    int32_t adapt_q16;         /* intent adapt */
    int32_t vibrance_q16;      /* 65536 = off */
    int32_t veil_q8;           /* L* Q8, 0 = off */
    int32_t lift_q8;           /* L* Q8, 0 = off */
    int32_t white_recip[3];    /* colour: round(2^40 / white_xyz[c]): X/Xw Q25 = (X Q32 · r) >> 31 */
    int32_t grey_lstar_min_q8; /* grey: L*min = L*(level_y[0]), Q8 (0 for colour) */
} epi_intent_ctx_t;

/* Validates the intent (ranges per include/epaper_image.h) and builds the
 * tables, deterministically (design.md "Fixed-point arithmetic and
 * determinism"). Pointers non-NULL; panel tables already validated
 * (tables_valid, epi_converter.c). */
epi_err_t epi_intent_init(epi_intent_ctx_t* ctx, const epi_panel_tables_t* tables, const epi_intent_t* intent);

/* S1: L*src (Q8) of each pixel. */
void epi_intent_lstar_row(const epi_rgb_t* src, int16_t* lstar_q8, uint16_t width);

/* S2 horizontal pass: the panel kernel along the row, edges replicated. */
void epi_blur_h(const epi_panel_tables_t* tables, const int16_t* in_q8, int16_t* out_q8, uint16_t width);

/* S2 vertical pass: rows[0..2r] are the horizontally blurred rows y−r..y+r
 * (the caller replicates edge rows by repeating pointers); out is base(y). */
void epi_blur_v(const epi_panel_tables_t* tables, const int16_t* const* rows, int16_t* out_q8, uint16_t width);

/* S1 (recomputed from src) + S3–S6: target linear sRGB, Q16. Colour only. */
void epi_intent_row(
    const epi_intent_ctx_t* ctx,
    const epi_rgb_t* src,
    const int16_t* base_q8,
    int32_t (*target_lin_q16)[3],
    uint16_t width
);

/* Grey (design.md "Grey panels"), from L*src as epi_intent_lstar_row computed it: S3 tone
 * + detail, S4 veil, S5 clamp to [L*min, 100], S6 target Y relative to
 * the lightest level, Q16. */
void epi_intent_grey_row(
    const epi_intent_ctx_t* ctx,
    const int16_t* lsrc_q8,
    const int16_t* base_q8,
    int32_t* target_y_q16,
    uint16_t width
);

/* L*(y) of a relative luminance y (Q16), in Q8, rounded (deterministic
 * double, design.md "Fixed-point arithmetic and determinism"): a grey panel's L*min from level_y[0]. */
int32_t epi_grey_lstar_q8(int32_t y_q16);

/* Whether the midtone curve has a monotone knee for L*min in Q8:
 * the knee is 60 up to L*min ≈ 39.77 and moves up beyond that; false once
 * it would reach 100 (L*min ≥ 66.29). */
int epi_midtone_monotone(int32_t lstar_min_q8);

/* CIELAB f and f⁻¹ in deterministic double, for init-time code. */
double epi_lab_f_d(double t);
double epi_lab_finv_d(double f);

#endif /* EPI_INTENT_H */
