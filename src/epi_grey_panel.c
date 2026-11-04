/*
 * Grey panels from measured levels (docs/design.md, "Grey panels").
 *
 * Everything is derived at init, deterministically (design.md "Fixed-point arithmetic and determinism"): level Y
 * in integers from the constant sRGB tables; thresholds, L*min and the
 * Gaussian kernel in double with + − × ÷ plus the library's epi_cbrt_d and
 * epi_exp_d (no libm), each rounded once to its integer format.
 */
#include "epaper_image.h"
#include "epi_fp.h"
#include "epi_intent.h"
#include "epi_math.h"
#include "epi_panel_tables.h"
#include "epi_srgb.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define GREY_INTENT(t) {.tone = (t), .detail = 0.75f, .adapt = 0.5f, .vibrance = 1.0f, .veil = 0.0f}

static const epi_intent_t grey_intent_default = GREY_INTENT(0.5f);
static const epi_intent_t grey_intent_soft = GREY_INTENT(0.0f);
static const epi_intent_t grey_intent_deep = GREY_INTENT(1.0f);

const epi_preset_t epi_grey_presets[EPI_GREY_PRESET_COUNT] = {
    {"Default", &grey_intent_default},
    {"Soft", &grey_intent_soft},
    {"Deep", &grey_intent_deep},
};

/* What the caller's storage holds (placed at its first aligned address). */
typedef struct {
    epi_panel_t panel;
    epi_grey_tables_t tables;
    epi_ink_t levels[EPI_MAX_INKS];
} grey_storage_t;

#define GREY_ALIGN ((size_t)_Alignof(grey_storage_t))
#define GREY_STORAGE_SIZE (sizeof(grey_storage_t) + GREY_ALIGN - 1)

/* Validates cfg and derives each level's Y relative to the last, Q16,
 * rounded once: (Y_i · 2^16 + Y_last/2) / Y_last. */
static bool derive_levels(const epi_grey_panel_config_t* cfg, int32_t level_y[EPI_MAX_INKS]) {
    if (cfg == NULL || cfg->name == NULL || cfg->levels == NULL || cfg->width < 1 || cfg->height < 1 ||
        cfg->level_count < 2 || cfg->level_count > EPI_MAX_INKS) {
        return false;
    }
    /* (0, 10.5]; false for NaN; radius ⌈3σ⌉ ≤ 32 = EPI_MAX_KERNEL_RADIUS. */
    if (!(cfg->detail_sigma > 0.0f && cfg->detail_sigma <= 10.5f)) {
        return false;
    }
    const size_t n = cfg->level_count;
    const int64_t y_last = epi_srgb_y_q32(cfg->levels[n - 1].measured);
    if (y_last <= 0) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        int64_t y = epi_srgb_y_q32(cfg->levels[i].measured); /* ≤ 2^32: ·2^16 fits */
        if (y > y_last) {
            return false; /* lighter than the last: the quotient might not fit int32 */
        }
        level_y[i] = (int32_t)((y * EPI_Q16_ONE + y_last / 2) / y_last);
        if (i > 0 && level_y[i] <= level_y[i - 1]) {
            return false; /* not strictly increasing (or equal after rounding) */
        }
    }
    /* The midtone curve needs a monotone knee below 100. */
    return epi_midtone_monotone(epi_grey_lstar_q8(level_y[0]));
}

size_t epi_grey_panel_size(const epi_grey_panel_config_t* cfg) {
    int32_t level_y[EPI_MAX_INKS];
    if (!derive_levels(cfg, level_y)) {
        return 0;
    }
    return GREY_STORAGE_SIZE;
}

epi_err_t epi_grey_panel_init(const epi_grey_panel_config_t* cfg, void* storage, size_t size, const epi_panel_t** out) {
    if (out != NULL) {
        *out = NULL;
    }
    int32_t level_y[EPI_MAX_INKS];
    if (storage == NULL || out == NULL || !derive_levels(cfg, level_y)) {
        return EPI_ERR_INVALID_ARG;
    }
    if (size < GREY_STORAGE_SIZE) {
        return EPI_ERR_WORKSPACE_TOO_SMALL;
    }
    grey_storage_t* s = epi_align_ptr(storage, GREY_ALIGN);
    /* cfg->levels may alias this storage (re-init from the panel's own inks):
     * take the levels before the memset. */
    const size_t n = cfg->level_count;
    epi_ink_t levels[EPI_MAX_INKS];
    memcpy(levels, cfg->levels, n * sizeof levels[0]);
    memset(s, 0, sizeof *s);
    memcpy(s->levels, levels, n * sizeof s->levels[0]);

    epi_grey_tables_t* t = &s->tables;
    t->base.kind = EPI_PANEL_GREY;
    t->base.ink_count = (uint8_t)n;
    t->base.kernel_radius = (uint8_t)epi_gaussian_kernel_q15(cfg->detail_sigma, t->base.kernel);
    for (size_t i = 0; i < n; i++) {
        t->level_y[i] = level_y[i];
    }
    /* Midpoints in cbrt(Y) (OKLab L of a neutral), cubed back to Y and
     * floored (see level_thr in epi_panel_tables.h). */
    for (size_t i = 0; i + 1 < n; i++) {
        double c0 = epi_cbrt_d(level_y[i] / 65536.0);
        double c1 = epi_cbrt_d(level_y[i + 1] / 65536.0);
        double m = (c0 + c1) / 2.0;
        double thr = m * m * m * 65536.0; /* in (y_i, y_{i+1}), ≥ 0 */
        int32_t q = (int32_t)thr;         /* floor */
        /* Keep y_i ≤ thr < y_{i+1} against rounding at the ends. */
        q = q < level_y[i] ? level_y[i] : (q >= level_y[i + 1] ? level_y[i + 1] - 1 : q);
        t->level_thr[i] = q;
    }

    epi_panel_t* p = &s->panel;
    p->name = cfg->name;
    p->width = cfg->width;
    p->height = cfg->height;
    p->inks = s->levels;
    p->ink_count = n;
    p->detail_sigma = cfg->detail_sigma;
    p->presets = epi_grey_presets;
    p->preset_count = EPI_GREY_PRESET_COUNT;
    p->tables = &t->base;
    *out = p;
    return EPI_OK;
}
