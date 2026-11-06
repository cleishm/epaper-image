/*
 * Per-stage intermediates of epi_intent_row, for checking each stage on its
 * own (tests/test_intent.c). Not used by the converter.
 */
#ifndef EPI_INTENT_STAGES_H
#define EPI_INTENT_STAGES_H

#include "epi_intent.h"

typedef struct {
    int32_t lsrc;           /* S1: L*src */
    int32_t d;              /* S1: adaptation degree D */
    int32_t lab_panel[3];   /* S1: ink-relative CIELAB of the panel colour */
    int32_t lab_tone[3];    /* S3: after tone + detail */
    int32_t lab_colour[3];  /* S4: after vibrance and veil */
    int32_t lab_neutral[3]; /* after the neutral cast (lab_colour when the panel has none) */
    int32_t xyz[3];         /* S5: after the cusp move */
    uint8_t moved;          /* S5: 1 if the pixel was outside the hull */
} epi_intent_stages_t;

/* One pixel of epi_intent_row, also writing its intermediates. */
void epi_intent_pixel_stages(
    const epi_intent_ctx_t* ctx,
    const epi_rgb_t* src,
    int32_t base_q8,
    int32_t target[3],
    epi_intent_stages_t* st
);

#endif /* EPI_INTENT_STAGES_H */
