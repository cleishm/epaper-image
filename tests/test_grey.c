/*
 * Tests for grey panels (docs/design.md, "Grey panels"): the runtime
 * constructor (src/epi_grey_panel.c), the grey intent rows, the grey
 * ditherer and the converter on grey panels, plus regression goldens.
 *
 * The test panels are plausible measured ramps, not calibrations; the shipped
 * GDEY075T7 panels are the measured ones.
 */
#include "epaper_image.h"
#include "epi_dither.h"
#include "epi_intent.h"
#include "epi_math.h"
#include "epi_panel_tables.h"
#include "epi_srgb.h"
#include "test_util.h"
#include "unity.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- test panels ------------------------------------------------------------ */

static const epi_ink_t levels4[4] = {
    {0, {0, 0, 0}, {20, 20, 20}},
    {1, {85, 85, 85}, {90, 90, 90}},
    {2, {170, 170, 170}, {150, 150, 150}},
    {3, {255, 255, 255}, {200, 200, 200}},
};

static const epi_ink_t levels2[2] = {
    {0, {0, 0, 0}, {20, 20, 20}},
    {1, {255, 255, 255}, {200, 200, 200}},
};

static epi_ink_t levels16[16];

typedef union {
    long long ll;
    long double ld;
    void* p;
    unsigned char bytes[8192];
} storage_t;

/* One scratch panel storage shared by tests that build a panel for their own
 * use (keeps .bss small enough for the ESP32 test app). */
static storage_t scratch_storage;

static const epi_panel_t* build(storage_t* st, const epi_ink_t* levels, size_t n, float sigma) {
    epi_grey_panel_config_t cfg = {"TEST", 640, 480, levels, n, sigma};
    size_t size = epi_grey_panel_size(&cfg);
    TEST_ASSERT_NOT_EQUAL(0, size);
    TEST_ASSERT_TRUE(size <= sizeof st->bytes);
    const epi_panel_t* p = NULL;
    TEST_ASSERT_EQUAL_INT(EPI_OK, epi_grey_panel_init(&cfg, st->bytes, size, &p, NULL));
    TEST_ASSERT_NOT_NULL(p);
    return p;
}

static const epi_panel_t* panel4(void) {
    static storage_t st;
    return build(&st, levels4, 4, 6.0f);
}

static const epi_panel_t* panel2(void) {
    static storage_t st;
    return build(&st, levels2, 2, 6.0f);
}

static const epi_panel_t* panel16(void) {
    static storage_t st;
    for (int i = 0; i < 16; i++) {
        uint8_t m = (uint8_t)(15 + 16 * i), nom = (uint8_t)(17 * i);
        levels16[i] = (epi_ink_t){(uint8_t)i, {nom, nom, nom}, {m, m, m}};
    }
    return build(&st, levels16, 16, 6.0f);
}

/* A small radius, for push/pop counts at a second r. */
static const epi_panel_t* panel4_small(void) {
    static storage_t st;
    return build(&st, levels4, 4, 1.5f);
}

/* ---- helpers ---------------------------------------------------------------- */

static double srgb_lin_d(int u) {
    double c = u / 255.0;
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

static double lstar_d(double y) {
    const double d = 6.0 / 29.0;
    double f = y > d * d * d ? cbrt(y) : y / (3.0 * d * d) + 4.0 / 29.0;
    return 116.0 * f - 16.0;
}

static void fill_flat(epi_rgb_t* img, size_t n, epi_rgb_t c) {
    for (size_t i = 0; i < n; i++) {
        img[i] = c;
    }
}

static double mean_y(const epi_panel_t* p, const uint8_t* codes, size_t n) {
    double s = 0;
    for (size_t i = 0; i < n; i++) {
        s += epi_grey_tables(p->tables)->level_y[code_to_index(p, codes[i])] / 65536.0;
    }
    return s / (double)n;
}

/* ---- epi_grey_panel_init ----------------------------------------------------- */

void test_grey_panel_validation(void) {
    epi_grey_panel_config_t ok = {"TEST", 800, 480, levels4, 4, 6.0f};
    size_t size = epi_grey_panel_size(&ok);
    TEST_ASSERT_NOT_EQUAL(0, size);
    storage_t* st = &scratch_storage;
    const epi_panel_t* p = NULL;
    TEST_ASSERT_EQUAL_INT(EPI_OK, epi_grey_panel_init(&ok, st->bytes, size, &p, NULL));
    TEST_ASSERT_NOT_NULL(p);

    const char* why = "stale";
    TEST_ASSERT_EQUAL_INT(EPI_OK, epi_grey_panel_init(&ok, st->bytes, size, &p, &why));
    TEST_ASSERT_NULL(why); /* success clears a stale reason */

    TEST_ASSERT_EQUAL_size_t(0, epi_grey_panel_size(NULL));
    p = panel4();
    why = "stale";
    TEST_ASSERT_EQUAL_INT(EPI_ERR_INVALID_ARG, epi_grey_panel_init(NULL, st->bytes, size, &p, &why));
    TEST_ASSERT_NULL(p);
    TEST_ASSERT_EQUAL_STRING("config is NULL", why);
    p = panel4();
    why = "stale";
    TEST_ASSERT_EQUAL_INT(EPI_ERR_INVALID_ARG, epi_grey_panel_init(&ok, NULL, size, &p, &why));
    TEST_ASSERT_NULL(p);
    TEST_ASSERT_EQUAL_STRING("storage or out is NULL", why);
    why = "stale";
    TEST_ASSERT_EQUAL_INT(EPI_ERR_INVALID_ARG, epi_grey_panel_init(&ok, st->bytes, size, NULL, &why));
    TEST_ASSERT_EQUAL_STRING("storage or out is NULL", why);
    /* An invalid config is reported as such even when the storage is NULL too. */
    epi_grey_panel_config_t one_level = ok;
    one_level.level_count = 1;
    why = "stale";
    TEST_ASSERT_EQUAL_INT(EPI_ERR_INVALID_ARG, epi_grey_panel_init(&one_level, NULL, 0, &p, &why));
    TEST_ASSERT_EQUAL_STRING("a grey panel needs 2 to 16 levels", why);
    /* A NULL reason is accepted on every kind of return. */
    TEST_ASSERT_EQUAL_INT(EPI_ERR_INVALID_ARG, epi_grey_panel_init(NULL, st->bytes, size, &p, NULL));
    TEST_ASSERT_EQUAL_INT(EPI_ERR_INVALID_ARG, epi_grey_panel_init(&ok, st->bytes, size, NULL, NULL));
    p = panel4();
    why = "stale";
    TEST_ASSERT_EQUAL_INT(EPI_ERR_WORKSPACE_TOO_SMALL, epi_grey_panel_init(&ok, st->bytes, size - 1, &p, &why));
    TEST_ASSERT_NULL(p);
    TEST_ASSERT_NULL(why);
    why = "stale";
    TEST_ASSERT_EQUAL_INT(EPI_ERR_WORKSPACE_TOO_SMALL, epi_grey_panel_init(&ok, st->bytes, 0, &p, &why));
    TEST_ASSERT_NULL(why);
    TEST_ASSERT_EQUAL_INT(EPI_ERR_WORKSPACE_TOO_SMALL, epi_grey_panel_init(&ok, st->bytes, 0, &p, NULL));

    /* Equal, decreasing, lighter-than-last, too-light-darkest levels. */
    static const epi_ink_t equal[3] = {
        {0, {0, 0, 0}, {20, 20, 20}}, {1, {0, 0, 0}, {90, 90, 90}}, {2, {0, 0, 0}, {90, 90, 90}}
    };
    static const epi_ink_t decreasing[2] = {{0, {0, 0, 0}, {200, 200, 200}}, {1, {0, 0, 0}, {20, 20, 20}}};
    /* The first level lighter than the last: the relative Y would exceed
     * INT32_MAX and must be rejected before the cast. */
    static const epi_ink_t lighter[2] = {{0, {0, 0, 0}, {255, 255, 255}}, {1, {0, 0, 0}, {0, 0, 1}}};
    static const epi_ink_t black0[2] = {{0, {0, 0, 0}, {0, 0, 0}}, {1, {0, 0, 0}, {200, 200, 200}}};
    static const epi_ink_t dark_ok[2] = {{0, {0, 0, 0}, {70, 70, 70}}, {1, {0, 0, 0}, {200, 200, 200}}};
    /* L*min ≈ 39.9: past a knee of 60, so the knee moves up. */
    static const epi_ink_t light_black[2] = {{0, {0, 0, 0}, {72, 72, 72}}, {1, {0, 0, 0}, {200, 200, 200}}};
    /* About 6:1, L*min ≈ 54: still a valid (higher) knee. */
    static const epi_ink_t low_contrast[2] = {{0, {0, 0, 0}, {100, 100, 100}}, {1, {0, 0, 0}, {200, 200, 200}}};
    /* L*min ≈ 68 (contrast below 2.8:1): no monotone knee below 100. */
    static const epi_ink_t too_light[2] = {{0, {0, 0, 0}, {130, 130, 130}}, {1, {0, 0, 0}, {200, 200, 200}}};
    static const epi_ink_t all_black[2] = {{0, {0, 0, 0}, {0, 0, 0}}, {1, {0, 0, 0}, {0, 0, 0}}};
    static const epi_ink_t dip[3] = {
        {0, {0, 0, 0}, {20, 20, 20}}, {1, {0, 0, 0}, {0, 0, 0}}, {2, {0, 0, 0}, {200, 200, 200}}
    };
    static const epi_ink_t dup_codes[2] = {{7, {0, 0, 0}, {20, 20, 20}}, {7, {0, 0, 0}, {200, 200, 200}}};

    const char* bad_reason[20];
    epi_grey_panel_config_t bad[20];
    int nbad = 0;
    bad_reason[nbad] = "name or levels is NULL";
    bad[nbad] = ok, bad[nbad++].name = NULL;
    bad_reason[nbad] = "name or levels is NULL";
    bad[nbad] = ok, bad[nbad++].levels = NULL;
    bad_reason[nbad] = "zero width or height";
    bad[nbad] = ok, bad[nbad++].width = 0;
    bad_reason[nbad] = "zero width or height";
    bad[nbad] = ok, bad[nbad++].height = 0;
    bad_reason[nbad] = "a grey panel needs 2 to 16 levels";
    bad[nbad] = ok, bad[nbad++].level_count = 1;
    bad_reason[nbad] = "a grey panel needs 2 to 16 levels";
    bad[nbad] = ok, bad[nbad++].level_count = 0;
    bad_reason[nbad] = "a grey panel needs 2 to 16 levels";
    bad[nbad] = ok, bad[nbad].levels = panel16()->inks, bad[nbad++].level_count = 17;
    bad_reason[nbad] = "detail sigma outside (0, 10.5]";
    bad[nbad] = ok, bad[nbad++].detail_sigma = 0.0f;
    bad_reason[nbad] = "detail sigma outside (0, 10.5]";
    bad[nbad] = ok, bad[nbad++].detail_sigma = -1.0f;
    bad_reason[nbad] = "detail sigma outside (0, 10.5]";
    bad[nbad] = ok, bad[nbad++].detail_sigma = 10.500001f;
    bad_reason[nbad] = "detail sigma outside (0, 10.5]";
    bad[nbad] = ok, bad[nbad++].detail_sigma = NAN;
    bad_reason[nbad] = "detail sigma outside (0, 10.5]";
    bad[nbad] = ok, bad[nbad++].detail_sigma = INFINITY;
    bad_reason[nbad] = "level luminance is not strictly increasing";
    bad[nbad] = ok, bad[nbad].levels = equal, bad[nbad++].level_count = 3;
    bad_reason[nbad] = "a level is lighter than the last";
    bad[nbad] = ok, bad[nbad].levels = decreasing, bad[nbad++].level_count = 2;
    bad_reason[nbad] = "a level is lighter than the last";
    bad[nbad] = ok, bad[nbad].levels = lighter, bad[nbad++].level_count = 2;
    bad_reason[nbad] = "darkest level is too light for the tone curve";
    bad[nbad] = ok, bad[nbad].levels = too_light, bad[nbad++].level_count = 2;
    bad_reason[nbad] = "lightest level has no luminance";
    bad[nbad] = ok, bad[nbad].levels = all_black, bad[nbad++].level_count = 2;
    bad_reason[nbad] = "level luminance is not strictly increasing";
    bad[nbad] = ok, bad[nbad].levels = dip, bad[nbad++].level_count = 3;
    for (int i = 0; i < nbad; i++) {
        char msg[32];
        snprintf(msg, sizeof msg, "bad config %d", i);
        TEST_ASSERT_EQUAL_size_t_MESSAGE(0, epi_grey_panel_size(&bad[i]), msg);
        p = panel4();
        why = "stale";
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            EPI_ERR_INVALID_ARG, epi_grey_panel_init(&bad[i], st->bytes, sizeof st->bytes, &p, &why), msg
        );
        TEST_ASSERT_NULL(p);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(bad_reason[i], why, msg);
    }

    /* Accepted: the ends of the sigma range, a true black, the lightest
     * darkest level, duplicate codes, 16 levels. */
    epi_grey_panel_config_t good[10];
    int ngood = 0;
    good[ngood] = ok, good[ngood++].detail_sigma = 10.5f;
    good[ngood] = ok, good[ngood++].detail_sigma = 1e-30f;
    good[ngood] = ok, good[ngood++].detail_sigma = 0.01f;
    good[ngood] = ok, good[ngood].levels = black0, good[ngood++].level_count = 2;
    good[ngood] = ok, good[ngood].levels = dark_ok, good[ngood++].level_count = 2;
    good[ngood] = ok, good[ngood].levels = dup_codes, good[ngood++].level_count = 2;
    good[ngood] = ok, good[ngood].levels = light_black, good[ngood++].level_count = 2;
    good[ngood] = ok, good[ngood].levels = low_contrast, good[ngood++].level_count = 2;
    good[ngood] = ok, good[ngood].levels = panel16()->inks, good[ngood++].level_count = 16;
    good[ngood] = ok, good[ngood++].width = 1;
    for (int i = 0; i < ngood; i++) {
        char msg[32];
        snprintf(msg, sizeof msg, "good config %d", i);
        size_t s = epi_grey_panel_size(&good[i]);
        TEST_ASSERT_NOT_EQUAL_MESSAGE(0, s, msg);
        TEST_ASSERT_EQUAL_INT_MESSAGE(EPI_OK, epi_grey_panel_init(&good[i], st->bytes, s, &p, NULL), msg);
        /* Every accepted panel also makes a converter (tables_valid agrees). */
        epi_config_t cc = config(p, 9, 3);
        size_t ws = epi_converter_workspace_size(&cc);
        TEST_ASSERT_NOT_EQUAL_MESSAGE(0, ws, msg);
        void* w = xmalloc(ws);
        epi_converter_t* c = NULL;
        TEST_ASSERT_EQUAL_INT_MESSAGE(EPI_OK, epi_converter_init(&cc, w, ws, &c), msg);
        free(w);
    }
    /* σ = 10.5 reaches the largest radius; a tiny σ is a unit impulse. */
    TEST_ASSERT_EQUAL_INT(EPI_OK, epi_grey_panel_init(&good[0], st->bytes, sizeof st->bytes, &p, NULL));
    TEST_ASSERT_EQUAL_UINT8(EPI_MAX_KERNEL_RADIUS, p->tables->kernel_radius);
    TEST_ASSERT_EQUAL_INT(EPI_OK, epi_grey_panel_init(&good[1], st->bytes, sizeof st->bytes, &p, NULL));
    TEST_ASSERT_EQUAL_UINT8(1, p->tables->kernel_radius);
    TEST_ASSERT_EQUAL_UINT16(0, p->tables->kernel[0]);
    TEST_ASSERT_EQUAL_UINT16(32768, p->tables->kernel[1]);
    TEST_ASSERT_EQUAL_UINT16(0, p->tables->kernel[2]);
}

/* Re-initialising from the panel's own inks, in the same storage, keeps them. */
void test_grey_panel_reinit_from_own_inks(void) {
    storage_t* st = &scratch_storage;
    epi_grey_panel_config_t cfg = {"TEST", 800, 480, levels4, 4, 6.0f};
    size_t size = epi_grey_panel_size(&cfg);
    const epi_panel_t* p = NULL;
    TEST_ASSERT_EQUAL_INT(EPI_OK, epi_grey_panel_init(&cfg, st->bytes, size, &p, NULL));
    cfg.levels = p->inks;
    cfg.detail_sigma = 4.0f;
    const epi_panel_t* q = NULL;
    TEST_ASSERT_EQUAL_INT(EPI_OK, epi_grey_panel_init(&cfg, st->bytes, size, &q, NULL));
    TEST_ASSERT_EQUAL_size_t(4, q->ink_count);
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_UINT8(levels4[i].code, q->inks[i].code);
        TEST_ASSERT_EQUAL_RGB(levels4[i].measured, q->inks[i].measured);
    }
}

/* Size, alignment, copy semantics, the panel's fields and presets. */
void test_grey_panel_storage(void) {
    epi_ink_t* lv = xmalloc(sizeof levels4);
    memcpy(lv, levels4, sizeof levels4);
    static const char name[] = "GREY4";
    epi_grey_panel_config_t cfg = {name, 800, 480, lv, 4, 6.0f};
    size_t size = epi_grey_panel_size(&cfg);
    TEST_ASSERT_TRUE(size >= sizeof(epi_panel_t) + sizeof(epi_grey_tables_t));
    uint8_t* buf = xmalloc(size + 64);
    uint8_t* ref = xmalloc(sizeof(epi_grey_tables_t));
    const epi_panel_t* p = NULL;
    for (int off = 0; off < 32; off++) {
        memset(buf, off & 1 ? 0xff : 0xa5, size + 64);
        TEST_ASSERT_EQUAL_INT(EPI_ERR_WORKSPACE_TOO_SMALL, epi_grey_panel_init(&cfg, buf + off, size - 1, &p, NULL));
        TEST_ASSERT_EQUAL_INT(EPI_OK, epi_grey_panel_init(&cfg, buf + off, size, &p, NULL));
        TEST_ASSERT_TRUE((const uint8_t*)p >= buf + off);
        TEST_ASSERT_TRUE((const uint8_t*)p + sizeof *p <= buf + off + size);
        TEST_ASSERT_TRUE((const uint8_t*)p->tables + sizeof(epi_grey_tables_t) <= buf + off + size);
        TEST_ASSERT_TRUE((const uint8_t*)(p->inks + 4) <= buf + off + size);
        /* Same tables at every offset and on any prior contents. */
        if (off == 0) {
            memcpy(ref, p->tables, sizeof(epi_grey_tables_t));
        } else {
            TEST_ASSERT_EQUAL_MEMORY(ref, p->tables, sizeof(epi_grey_tables_t));
        }
    }
    /* The caller's levels may be changed and freed. */
    memset(lv, 0x55, sizeof levels4);
    free(lv);
    TEST_ASSERT_EQUAL_MEMORY(levels4, p->inks, sizeof levels4);
    TEST_ASSERT_TRUE(p->name == name);
    TEST_ASSERT_EQUAL_UINT16(800, p->width);
    TEST_ASSERT_EQUAL_UINT16(480, p->height);
    TEST_ASSERT_EQUAL_size_t(4, p->ink_count);
    TEST_ASSERT_TRUE(p->detail_sigma == 6.0f);
    TEST_ASSERT_EQUAL_UINT8(EPI_PANEL_GREY, p->tables->kind);
    TEST_ASSERT_EQUAL_UINT8(4, p->tables->ink_count);

    /* Presets: Default, Soft, Deep. */
    TEST_ASSERT_EQUAL_size_t(3, p->preset_count);
    TEST_ASSERT_TRUE(p->presets == epi_grey_presets);
    TEST_ASSERT_TRUE(epi_panel_gdey075t7_mono.presets == epi_grey_presets);
    TEST_ASSERT_TRUE(epi_panel_gdey075t7_grey4.presets == epi_grey_presets);
    static const char* names[3] = {"Default", "Soft", "Deep"};
    static const float tones[3] = {0.5f, 0.0f, 1.0f};
    for (int i = 0; i < 3; i++) {
        const epi_intent_t* in = p->presets[i].intent;
        TEST_ASSERT_EQUAL_STRING(names[i], p->presets[i].name);
        TEST_ASSERT_TRUE(in->tone == tones[i]);
        TEST_ASSERT_TRUE(
            in->detail == 0.75f && in->adapt == 0.5f && in->vibrance == 1.0f && in->veil == 0.0f && in->lift == 0.0f
        );
    }

    /* A converter on this panel runs from storage at an odd offset. */
    epi_config_t cc = config(p, 31, 23);
    epi_rgb_t img[31 * 23];
    fill_noise(img, 31 * 23, 99);
    uint8_t a[31 * 23], b[31 * 23];
    run_converter(&cc, img, a, NULL);
    cc.panel = panel4();
    run_converter(&cc, img, b, NULL);
    TEST_ASSERT_EQUAL_MEMORY(a, b, sizeof a);
    free(ref);
    free(buf);
}

/* Derived tables: level Y, thresholds, the kernel rule. */
void test_grey_panel_tables(void) {
    const epi_panel_t* ps[3] = {panel2(), panel4(), panel16()};
    for (int k = 0; k < 3; k++) {
        const epi_grey_tables_t* t = epi_grey_tables(ps[k]->tables);
        const size_t n = ps[k]->ink_count;
        /* Y of a neutral sRGB8 is its linear value (the matrix rows sum to 1). */
        const double y_last = srgb_lin_d(ps[k]->inks[n - 1].measured.r);
        TEST_ASSERT_EQUAL_INT32(65536, t->level_y[n - 1]);
        for (size_t i = 0; i < n; i++) {
            double want = srgb_lin_d(ps[k]->inks[i].measured.r) / y_last * 65536.0;
            /* ≤ 2 LSB: the Q16 sRGB tables' rounding, scaled by 1/Y_last. */
            TEST_ASSERT_TRUE_MESSAGE(fabs(t->level_y[i] - want) <= 2.0, "level_y vs libm");
        }
        for (size_t i = 0; i + 1 < n; i++) {
            double m = (cbrt(t->level_y[i] / 65536.0) + cbrt(t->level_y[i + 1] / 65536.0)) / 2.0;
            double want = m * m * m * 65536.0;
            TEST_ASSERT_TRUE_MESSAGE(
                t->level_thr[i] <= want && want < t->level_thr[i] + 1.0 + 1e-6, "threshold is the floored cbrt midpoint"
            );
            TEST_ASSERT_TRUE(t->level_thr[i] >= t->level_y[i] && t->level_thr[i] < t->level_y[i + 1]);
        }
        for (size_t i = n; i < EPI_MAX_INKS; i++) {
            TEST_ASSERT_EQUAL_INT32(0, t->level_y[i]);
        }
    }
    /* Exact values for the 4-level panel (Y of 20, 90, 150 and 200 grey). */
    static const int32_t want4[4] = {793, 11602, 34607, 65536};
    TEST_ASSERT_EQUAL_INT32_ARRAY(want4, epi_grey_tables(panel4()->tables)->level_y, 4);

    /* σ = 6 reproduces the EL133UF1 kernel (generated offline by panelgen). */
    const epi_panel_tables_t* el = epi_panel_el133uf1.tables;
    TEST_ASSERT_TRUE(epi_panel_el133uf1.detail_sigma == 6.0f);
    TEST_ASSERT_EQUAL_UINT8(el->kernel_radius, panel4()->tables->kernel_radius);
    TEST_ASSERT_EQUAL_UINT16_ARRAY(el->kernel, panel4()->tables->kernel, 2 * EPI_MAX_KERNEL_RADIUS + 1);

    /* Every σ: radius ⌈3σ⌉, symmetric, sum exactly 2^15, unimodal, zero beyond. */
    storage_t* st = &scratch_storage;
    for (int s100 = 1; s100 <= 1050; s100 += 7) {
        float sigma = (float)(s100 / 100.0);
        const epi_panel_t* p = build(st, levels4, 4, sigma);
        const epi_panel_tables_t* t = p->tables;
        const int r = t->kernel_radius;
        TEST_ASSERT_EQUAL_INT((int)ceil(3.0 * (double)sigma), r);
        int32_t sum = 0;
        for (int j = 0; j <= 2 * r; j++) {
            sum += t->kernel[j];
            TEST_ASSERT_EQUAL_UINT16(t->kernel[j], t->kernel[2 * r - j]);
            if (j > 0 && j <= r) {
                TEST_ASSERT_TRUE(t->kernel[j] >= t->kernel[j - 1]);
            }
        }
        TEST_ASSERT_EQUAL_INT32(32768, sum);
        for (int j = 2 * r + 1; j <= 2 * EPI_MAX_KERNEL_RADIUS; j++) {
            TEST_ASSERT_EQUAL_UINT16(0, t->kernel[j]);
        }
    }
}

/* ---- grey intent -------------------------------------------------------------- */

void test_grey_intent(void) {
    const epi_panel_t* ps[3] = {panel2(), panel4(), panel16()};
    for (int k = 0; k < 3; k++) {
        const epi_panel_t* p = ps[k];
        for (size_t i = 0; i < p->preset_count; i++) {
            epi_intent_ctx_t ctx;
            TEST_ASSERT_EQUAL_INT(EPI_OK, epi_intent_init(&ctx, p->tables, p->presets[i].intent));
            /* L*min = L*(darkest level relative to the lightest). */
            double lmin = lstar_d(epi_grey_tables(p->tables)->level_y[0] / 65536.0) * 256.0;
            TEST_ASSERT_TRUE(fabs(ctx.grey_lstar_min_q8 - lmin) <= 1.0);
            /* Facts the black/white tests rely on: T(100) = 100 and T(0) = L*min. */
            TEST_ASSERT_EQUAL_INT32(100 * 256, ctx.tone_t_q8[EPI_TONE_TABLE_LEN - 1]);
            TEST_ASSERT_EQUAL_INT32(ctx.grey_lstar_min_q8, ctx.tone_t_q8[0]);
            /* Flat white → exactly the lightest level's Y; flat black → the darkest's. */
            int16_t l[2] = {100 * 256, 0};
            int32_t tgt[2];
            epi_intent_grey_row(&ctx, l, l, tgt, 2);
            TEST_ASSERT_EQUAL_INT32(65536, tgt[0]);
            TEST_ASSERT_INT32_WITHIN(4, epi_grey_tables(p->tables)->level_y[0], tgt[1]);
            /* Monotone in L*src on flat input, inside [Y(L*min), 1]. */
            int32_t prev = 0;
            for (int32_t q = 0; q <= 100 * 256; q += 16) {
                int16_t lq = (int16_t)q;
                int32_t y;
                epi_intent_grey_row(&ctx, &lq, &lq, &y, 1);
                TEST_ASSERT_TRUE(y >= prev);
                TEST_ASSERT_TRUE(y >= tgt[1] && y <= 65536);
                prev = y;
            }
        }
        /* Veil: source white sits below paper white. */
        epi_intent_t in = *p->presets[0].intent;
        in.veil = 2.0f;
        epi_intent_ctx_t ctx;
        TEST_ASSERT_EQUAL_INT(EPI_OK, epi_intent_init(&ctx, p->tables, &in));
        int16_t w = 100 * 256;
        int32_t y;
        epi_intent_grey_row(&ctx, &w, &w, &y, 1);
        TEST_ASSERT_INT32_WITHIN(2, (int32_t)(pow((98.0 + 16.0) / 116.0, 3.0) * 65536.0 + 0.5), y);
        /* Intent ranges are checked for grey too. */
        in.veil = 4.0f;
        TEST_ASSERT_EQUAL_INT(EPI_ERR_INVALID_ARG, epi_intent_init(&ctx, p->tables, &in));
        in = *p->presets[0].intent;
        in.tone = 1.5f;
        TEST_ASSERT_EQUAL_INT(EPI_ERR_INVALID_ARG, epi_intent_init(&ctx, p->tables, &in));
    }
    /* Vibrance and adapt do nothing on grey. */
    const epi_panel_t* p = panel4();
    epi_intent_t a = *p->presets[0].intent, b = a;
    b.vibrance = 2.5f;
    b.adapt = 0.0f;
    epi_intent_ctx_t ca, cb;
    TEST_ASSERT_EQUAL_INT(EPI_OK, epi_intent_init(&ca, p->tables, &a));
    TEST_ASSERT_EQUAL_INT(EPI_OK, epi_intent_init(&cb, p->tables, &b));
    enum { W = 64 };
    epi_rgb_t src[W];
    int16_t ls[W];
    int32_t ya[W], yb[W];
    fill_noise(src, W, 5);
    epi_intent_lstar_row(src, ls, W);
    epi_intent_grey_row(&ca, ls, ls, ya, W);
    epi_intent_grey_row(&cb, ls, ls, yb, W);
    TEST_ASSERT_EQUAL_INT32_ARRAY(ya, yb, W);
}

/* Grey: the lift is in the tone curve; the stored L*src doesn't move. */
void test_grey_lift(void) {
    const epi_panel_t* p = panel4();
    epi_intent_t off = *p->presets[0].intent;
    epi_intent_t on = off;
    on.lift = 8.0f;
    epi_intent_ctx_t c0, c8;
    TEST_ASSERT_EQUAL_INT(EPI_OK, epi_intent_init(&c0, p->tables, &off));
    TEST_ASSERT_EQUAL_INT(EPI_OK, epi_intent_init(&c8, p->tables, &on));
    TEST_ASSERT_EQUAL_INT32(c0.tone_t_q8[48 * 4], c8.tone_t_q8[40 * 4]);
    int16_t l = 40 * 256, raised = 48 * 256;
    int32_t y8, yr;
    epi_intent_grey_row(&c8, &l, &l, &y8, 1);
    epi_intent_grey_row(&c0, &raised, &raised, &yr, 1);
    TEST_ASSERT_EQUAL_INT32(yr, y8);
}

/* ---- grey ditherer ------------------------------------------------------------ */

/* The cbrt-skipping band is a pure optimisation: equal to the direct
 * formula (as epi_dither_damp_coeffs computes kl) for every Q16 t. */
void test_grey_dither_cutl(void) {
    for (int32_t t = -70000; t <= 140000; t++) {
        int64_t l100 = (int64_t)epi_cbrt_q16(t) * 100;
        int32_t ss = epi_smoothstep_q16((int32_t)l100, 90 * EPI_Q16_ONE, 97 * EPI_Q16_ONE);
        int32_t want = (int32_t)(((uint32_t)(EPI_Q16_ONE - ss) * 64u + 2u) / 5u);
        if (epi_dither_grey_cutl(t) != want) {
            char msg[64];
            snprintf(msg, sizeof msg, "cutl(%ld) fast path differs", (long)t);
            TEST_FAIL_MESSAGE(msg);
        }
    }
    TEST_ASSERT_EQUAL_INT32((EPI_Q16_ONE * 64 + 2) / 5, epi_dither_grey_cutl(0));
    TEST_ASSERT_EQUAL_INT32(0, epi_dither_grey_cutl(EPI_Q16_ONE));
}

/* The coverage toe (design.md "Grey panels") in real arithmetic: d = .03, c0 = .11. */
static double toe_real(double c) {
    if (c < 0.03) {
        return 0.0;
    }
    if (c >= 0.11) {
        return c;
    }
    double s = (c - 0.03) / 0.08;
    return 0.11 * (3 * s * s - 2 * s * s * s) + 0.08 * (s * s * s - s * s);
}

/* toe_q16 over every c in [0, 2^16]: 0 below D, the identity from C0,
 * monotone non-decreasing, continuous at D and C0 (to 1 LSB), never above
 * c, within 1 LSB of the real toe; the mirrored map is monotone across ½
 * and symmetric, m(c) + m(1 − c) = 1. */
void test_grey_toe_q16(void) {
    const int32_t D = 1966, C0 = 7209; /* round(.03·2^16), round(.11·2^16) */
    int32_t prev = epi_dither_grey_toe_q16(0), mprev = 0;
    for (int32_t c = 0; c <= EPI_Q16_ONE; c++) {
        int32_t h = epi_dither_grey_toe_q16(c);
        char msg[80];
        snprintf(msg, sizeof msg, "toe_q16(%ld) = %ld", (long)c, (long)h);
        if (c < D) {
            TEST_ASSERT_EQUAL_INT32_MESSAGE(0, h, msg);
        }
        if (c >= C0) {
            TEST_ASSERT_EQUAL_INT32_MESSAGE(c, h, msg);
        }
        TEST_ASSERT_TRUE_MESSAGE(h >= prev && h >= 0 && h <= c, msg);
        TEST_ASSERT_TRUE_MESSAGE(fabs(h - toe_real(c / 65536.0) * 65536.0) <= 1.0, msg);
        prev = h;
        int32_t m = c < EPI_Q16_ONE / 2 ? h : EPI_Q16_ONE - epi_dither_grey_toe_q16(EPI_Q16_ONE - c);
        int32_t mm = EPI_Q16_ONE - c < EPI_Q16_ONE / 2 ? epi_dither_grey_toe_q16(EPI_Q16_ONE - c)
                                                       : EPI_Q16_ONE - epi_dither_grey_toe_q16(c);
        TEST_ASSERT_TRUE_MESSAGE(m >= mprev, msg);
        TEST_ASSERT_EQUAL_INT32_MESSAGE(EPI_Q16_ONE, m + mm, msg);
        mprev = m;
    }
    /* Continuity: no jump at D, one LSB (the identity's own step) at C0. */
    TEST_ASSERT_EQUAL_INT32(0, epi_dither_grey_toe_q16(D));
    TEST_ASSERT_EQUAL_INT32(0, epi_dither_grey_toe_q16(D - 1));
    TEST_ASSERT_INT_WITHIN(1, C0 - 1, epi_dither_grey_toe_q16(C0 - 1));
    TEST_ASSERT_TRUE(epi_dither_grey_toe_q16(D + 1) <= 1);
}

/* The toe on one set of tables, every t in [−3000, 70000]: the ditherer's
 * fast path equals the full formula; t′ is t outside [y_0, y_last), the
 * gap's own end at the gap's ends, inside the gap, monotone in t, and
 * within 2 LSB of the real toe of the real coverage (c is rounded to Q16,
 * the toe's slope reaches about 1.35, and t′ is rounded). Returns the number of
 * t on the identity band (t′ = t inside a gap, c within [C0, 1 − C0]). */
static long check_toe_tables(const epi_grey_tables_t* tb) {
    epi_dither_ctx_t ctx;
    int32_t err[16];
    epi_dither_init(&ctx, &tb->base, 1, err);
    const unsigned n = tb->base.ink_count;
    long band = 0;
    int32_t prev = INT32_MIN;
    unsigned g = 0;
    for (int32_t t = -3000; t <= 70000; t++) {
        int32_t full = epi_dither_grey_toe_full(tb, t);
        int32_t fast = epi_dither_grey_toe(&ctx, t);
        char msg[96];
        snprintf(msg, sizeof msg, "%u levels: t %ld → full %ld, fast %ld", n, (long)t, (long)full, (long)fast);
        TEST_ASSERT_EQUAL_INT32_MESSAGE(full, fast, msg);
        TEST_ASSERT_TRUE_MESSAGE(full >= prev, msg);
        prev = full;
        if (t < tb->level_y[0] || t >= tb->level_y[n - 1]) {
            TEST_ASSERT_EQUAL_INT32_MESSAGE(t, full, msg);
            continue;
        }
        while (t >= tb->level_y[g + 1]) {
            g++;
        }
        const int32_t y0 = tb->level_y[g], y1 = tb->level_y[g + 1];
        TEST_ASSERT_TRUE_MESSAGE(full >= y0 && full <= y1, msg);
        if (t == y0) {
            TEST_ASSERT_EQUAL_INT32_MESSAGE(y0, full, msg);
        }
        const double c = (double)(t - y0) / (y1 - y0);
        const double cp = c < 0.5 ? toe_real(c) : 1.0 - toe_real(1.0 - c);
        TEST_ASSERT_TRUE_MESSAGE(fabs(full - (y0 + cp * (y1 - y0))) <= 2.0, msg);
        if (t >= ctx.toe_id_lo[g] && t <= ctx.toe_id_hi[g]) {
            TEST_ASSERT_EQUAL_INT32_MESSAGE(t, full, msg);
            band++;
        }
    }
    /* The band is the identity region, to one ulp of the rounded c. */
    for (unsigned k = 0; k + 1 < n; k++) {
        const double gap = tb->level_y[k + 1] - tb->level_y[k];
        if (ctx.toe_id_lo[k] <= ctx.toe_id_hi[k]) {
            double lo = (ctx.toe_id_lo[k] - tb->level_y[k]) / gap, hi = (ctx.toe_id_hi[k] - tb->level_y[k]) / gap;
            TEST_ASSERT_TRUE(lo >= 0.11 - 1.0 / gap - 1e-4 && lo <= 0.11 + 1.0 / gap + 1e-4);
            TEST_ASSERT_TRUE(hi >= 0.89 - 1.0 / gap - 1e-4 && hi <= 0.89 + 1.0 / gap + 1e-4);
        } else {
            TEST_ASSERT_TRUE(gap < 20); /* only tiny gaps have an empty band */
        }
    }
    return band;
}

/* The toe's identity fast path equals the full formula for every t on the
 * test panels, both shipped GDEY075T7 calibrations, and synthetic tables
 * with tiny gaps (empty bands), odd gaps and a full-range gap (y_0 = 0). */
void test_grey_toe_fast_path(void) {
    const epi_panel_t* ps[5] = {panel2(), panel4(), panel16(), &epi_panel_gdey075t7_mono, &epi_panel_gdey075t7_grey4};
    for (int k = 0; k < 5; k++) {
        const epi_grey_tables_t* t = epi_grey_tables(ps[k]->tables);
        long band = check_toe_tables(t);
        /* About 78% of each gap is on the band. */
        long span = EPI_Q16_ONE - t->level_y[0];
        TEST_ASSERT_TRUE(band > span * 77 / 100 && band < span * 79 / 100);
    }
    static const int32_t synth[2][11] = {
        {0, 1, 3, 6, 11, 18, 60, 200, 1001, 30000, 65536},
        {0, 65536},
    };
    static const unsigned synth_n[2] = {11, 2};
    epi_grey_tables_t* tb = xmalloc(sizeof *tb);
    for (int k = 0; k < 2; k++) {
        memset(tb, 0, sizeof *tb);
        tb->base.kind = EPI_PANEL_GREY;
        tb->base.ink_count = (uint8_t)synth_n[k];
        for (unsigned i = 0; i < synth_n[k]; i++) {
            tb->level_y[i] = synth[k][i];
        }
        TEST_ASSERT_TRUE(check_toe_tables(tb) > 0);
    }
    free(tb);
}

/* One pixel on a fresh row: v = t, so the index is the nearest level in
 * cbrt(Y), ties to the darker level, for every v in range. */
void test_grey_dither_nearest(void) {
    const epi_panel_t* ps[3] = {panel2(), panel4(), panel16()};
    int32_t err[16];
    for (int k = 0; k < 3; k++) {
        const epi_grey_tables_t* t = epi_grey_tables(ps[k]->tables);
        const unsigned n = t->base.ink_count;
        double c[EPI_MAX_INKS];
        for (unsigned i = 0; i < n; i++) {
            c[i] = cbrt(t->level_y[i] / 65536.0);
        }
        for (int32_t v = -3000; v <= 70000; v++) {
            epi_dither_ctx_t ctx;
            TEST_ASSERT_TRUE(epi_dither_err_size(&t->base, 1) <= sizeof err);
            epi_dither_init(&ctx, &t->base, 1, err);
            uint8_t idx;
            epi_dither_grey_row(&ctx, &v, &idx);
            double cv = cbrt(v / 65536.0);
            unsigned best = 0;
            double bd = fabs(cv - c[0]);
            for (unsigned i = 1; i < n; i++) {
                double d = fabs(cv - c[i]);
                if (d < bd) {
                    bd = d;
                    best = i;
                }
            }
            if (idx != best && fabs(fabs(cv - c[idx]) - bd) > 1e-12) {
                char msg[64];
                snprintf(msg, sizeof msg, "%u levels: v %ld → %u, nearest %u", n, (long)v, idx, best);
                TEST_FAIL_MESSAGE(msg);
            }
        }
    }
}

/* A flat target at a level's Y maps to that level everywhere (no error). */
void test_grey_dither_flat_levels(void) {
    const epi_panel_t* ps[3] = {panel2(), panel4(), panel16()};
    enum { W = 37, H = 6 };
    int32_t* err = xmalloc(epi_dither_err_size(ps[0]->tables, W));
    for (int k = 0; k < 3; k++) {
        const epi_grey_tables_t* t = epi_grey_tables(ps[k]->tables);
        for (unsigned i = 0; i < t->base.ink_count; i++) {
            epi_dither_ctx_t ctx;
            epi_dither_init(&ctx, &t->base, W, err);
            int32_t tgt[W];
            for (int x = 0; x < W; x++) {
                tgt[x] = t->level_y[i];
            }
            for (int y = 0; y < H; y++) {
                uint8_t idx[W];
                epi_dither_grey_row(&ctx, tgt, idx);
                for (int x = 0; x < W; x++) {
                    TEST_ASSERT_EQUAL_UINT8(i, idx[x]);
                }
            }
        }
    }
    free(err);
}

/* Flat targets between levels: the mean output Y tracks the target (the
 * damping biases it towards the nearer level by a few percent at most), and
 * the output uses only the two bracketing levels. Init zeroes dirty error
 * memory. */
void test_grey_dither_mean(void) {
    const epi_grey_tables_t* t = epi_grey_tables(panel4()->tables);
    enum { W = 64, H = 64 };
    int32_t* err = xmalloc(epi_dither_err_size(&t->base, W));
    for (int32_t tv = 2000; tv <= 64000; tv += 3100) {
        memset(err, 0x7f, epi_dither_err_size(&t->base, W));
        epi_dither_ctx_t ctx;
        epi_dither_init(&ctx, &t->base, W, err);
        int32_t tgt[W];
        for (int x = 0; x < W; x++) {
            tgt[x] = tv;
        }
        unsigned lo = 0;
        while (lo + 2 < t->base.ink_count && t->level_y[lo + 1] <= tv) {
            lo++;
        }
        double sum = 0;
        for (int y = 0; y < H; y++) {
            uint8_t idx[W];
            epi_dither_grey_row(&ctx, tgt, idx);
            for (int x = 0; x < W; x++) {
                TEST_ASSERT_TRUE(idx[x] == lo || idx[x] == lo + 1);
                sum += t->level_y[idx[x]];
            }
        }
        double mean = sum / (W * H);
        double span = t->level_y[lo + 1] - t->level_y[lo];
        TEST_ASSERT_TRUE_MESSAGE(fabs(mean - tv) <= 0.15 * span, "mean output Y tracks the target");
    }
    free(err);
}

/* ---- converter on grey panels -------------------------------------------------- */

/* Whole-image reference from the stage functions, against the converter. */
static void
check_streaming_equals_whole(const epi_panel_t* p, const epi_intent_t* in, const epi_rgb_t* img, int w, int h) {
    const epi_panel_tables_t* t = p->tables;
    const int r = t->kernel_radius;
    epi_intent_ctx_t* ctx = xmalloc(sizeof *ctx);
    TEST_ASSERT_EQUAL_INT(EPI_OK, epi_intent_init(ctx, t, in != NULL ? in : p->presets[0].intent));
    const size_t n = (size_t)w * h;
    int16_t* ls = xmalloc(n * sizeof *ls);
    int16_t* hb = xmalloc(n * sizeof *hb);
    int16_t* base = xmalloc((size_t)w * sizeof *base);
    int32_t* tgt = xmalloc((size_t)w * sizeof *tgt);
    uint8_t* ref = xmalloc(n);
    void* err = xmalloc(epi_dither_err_size(t, (uint16_t)w));
    epi_dither_ctx_t d;
    epi_dither_init(&d, t, (uint16_t)w, err);
    for (int y = 0; y < h; y++) {
        epi_intent_lstar_row(img + (size_t)y * w, ls + (size_t)y * w, (uint16_t)w);
        epi_blur_h(t, ls + (size_t)y * w, hb + (size_t)y * w, (uint16_t)w);
    }
    for (int y = 0; y < h; y++) {
        const int16_t* rows[2 * EPI_MAX_KERNEL_RADIUS + 1];
        clamp_rows(hb, w, h, y, r, rows);
        epi_blur_v(t, rows, base, (uint16_t)w);
        epi_intent_grey_row(ctx, ls + (size_t)y * w, base, tgt, (uint16_t)w);
        uint8_t* row = ref + (size_t)y * w;
        epi_dither_grey_row(&d, tgt, row);
        for (int x = 0; x < w; x++) {
            row[x] = p->inks[row[x]].code;
        }
    }
    epi_config_t cfg = config(p, (uint16_t)w, (uint16_t)h);
    cfg.intent = in;
    uint8_t* out = xmalloc(n);
    run_converter(&cfg, img, out, NULL);
    TEST_ASSERT_EQUAL_MEMORY(ref, out, n);
    free(out);
    free(err);
    free(ref);
    free(tgt);
    free(base);
    free(hb);
    free(ls);
    free(ctx);
}

/* Lag, flush and counts around the radius (two radii), odd widths, each
 * equal to the whole-image reference. */
void test_grey_converter_streaming(void) {
    const epi_panel_t* ps[2] = {panel4(), panel4_small()};
    for (int k = 0; k < 2; k++) {
        const int r = ps[k]->tables->kernel_radius;
        const int heights[] = {1, 2, r - 1, r, r + 1, r + 2, 2 * r, 2 * r + 1, 2 * r + 2, 3 * r + 5};
        const int widths[] = {1, 2, 7, 33};
        for (size_t hi = 0; hi < sizeof heights / sizeof heights[0]; hi++) {
            for (size_t wi = 0; wi < sizeof widths / sizeof widths[0]; wi++) {
                int w = widths[wi], h = heights[hi];
                epi_rgb_t* img = xmalloc((size_t)w * h * sizeof *img);
                fill_noise(img, (size_t)w * h, (uint32_t)(h * 131 + w + k));
                check_streaming_equals_whole(ps[k], NULL, img, w, h);
                free(img);
            }
        }
    }
    /* Larger images, every preset, 2 and 16 levels too. */
    const epi_panel_t* all[3] = {panel2(), panel4(), panel16()};
    const int w = 61, h = 47;
    epi_rgb_t* img = xmalloc((size_t)w * h * sizeof *img);
    fill_gradient(img, w, h);
    for (int k = 0; k < 3; k++) {
        for (size_t i = 0; i < all[k]->preset_count; i++) {
            check_streaming_equals_whole(all[k], all[k]->presets[i].intent, img, w, h);
        }
    }
    free(img);

    /* r + 1 undrained rows are within the drain contract. */
    const epi_panel_t* p = panel4();
    const int r = p->tables->kernel_radius, h2 = 50, w2 = 11;
    img = xmalloc((size_t)w2 * h2 * sizeof *img);
    fill_noise(img, (size_t)w2 * h2, 31337);
    epi_config_t cfg = config(p, (uint16_t)w2, (uint16_t)h2);
    uint8_t* ref = xmalloc((size_t)w2 * h2);
    run_converter(&cfg, img, ref, NULL);
    size_t size = epi_converter_workspace_size(&cfg);
    void* ws = xmalloc(size);
    epi_converter_t* c = NULL;
    TEST_ASSERT_EQUAL_INT(EPI_OK, epi_converter_init(&cfg, ws, size, &c));
    for (int y = 0; y <= r; y++) {
        epi_converter_push(c, img + (size_t)y * w2);
    }
    uint8_t row[11];
    TEST_ASSERT_EQUAL_INT(EPI_OK, epi_converter_pop(c, row));
    TEST_ASSERT_EQUAL_MEMORY(ref, row, (size_t)w2);
    TEST_ASSERT_EQUAL_INT(EPI_ERR_NOT_READY, epi_converter_pop(c, row));
    free(ws);
    free(ref);
    free(img);
}

/* Workspace: smaller than colour at the same width, height-independent,
 * any alignment, too small by one byte fails. */
void test_grey_converter_workspace(void) {
    epi_config_t g = config(panel4(), 1600, 1200);
    epi_config_t col = config(&epi_panel_el133uf1, 1600, 1200);
    size_t gs = epi_converter_workspace_size(&g), cs = epi_converter_workspace_size(&col);
    printf("grey workspace at 1600 wide: %zu B (colour %zu B)\n", gs, cs);
    TEST_ASSERT_TRUE(gs > 0 && gs < cs);
    g.height = 7;
    TEST_ASSERT_EQUAL_size_t(gs, epi_converter_workspace_size(&g));

    g = config(panel4(), 37, 5);
    size_t size = epi_converter_workspace_size(&g);
    uint8_t* buf = xmalloc(size + 64);
    epi_rgb_t img[37 * 5];
    fill_noise(img, 37 * 5, 8);
    uint8_t ref[37 * 5], out[37 * 5];
    run_converter(&g, img, ref, NULL);
    for (int off = 0; off < 32; off++) {
        epi_converter_t* c = (epi_converter_t*)buf;
        TEST_ASSERT_EQUAL_INT(EPI_ERR_WORKSPACE_TOO_SMALL, epi_converter_init(&g, buf + off, size - 1, &c));
        TEST_ASSERT_NULL(c);
        memset(buf, off * 37, size + 64);
        run_converter_in(&g, img, buf + off, size, out, NULL);
        TEST_ASSERT_EQUAL_MEMORY(ref, out, sizeof ref);
    }
    free(buf);
}

/* Source white → all lightest; source black → all darkest, every preset. */
void test_grey_converter_white_black(void) {
    const epi_panel_t* ps[3] = {panel2(), panel4(), panel16()};
    enum { W = 23, H = 41 };
    epi_rgb_t img[W * H];
    uint8_t out[W * H];
    for (int k = 0; k < 3; k++) {
        const epi_panel_t* p = ps[k];
        for (size_t i = 0; i < p->preset_count; i++) {
            epi_config_t cfg = config(p, W, H);
            cfg.intent = p->presets[i].intent;
            fill_flat(img, W * H, (epi_rgb_t){255, 255, 255});
            run_converter(&cfg, img, out, NULL);
            for (int j = 0; j < W * H; j++) {
                TEST_ASSERT_EQUAL_UINT8(p->inks[p->ink_count - 1].code, out[j]);
            }
            fill_flat(img, W * H, (epi_rgb_t){0, 0, 0});
            run_converter(&cfg, img, out, NULL);
            for (int j = 0; j < W * H; j++) {
                TEST_ASSERT_EQUAL_UINT8(p->inks[0].code, out[j]);
            }
        }
    }
}

/* A horizontal grey ramp: per 16-column band, the darkest level's share
 * never rises, the lightest's never falls, and the mean Y never falls. */
void test_grey_converter_ramp(void) {
    const epi_panel_t* p = panel4();
    enum { W = 256, H = 64, BAND = 16, NB = W / BAND };
    epi_rgb_t* img = xmalloc((size_t)W * H * sizeof *img);
    uint8_t* out = xmalloc((size_t)W * H);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            img[(size_t)y * W + x] = (epi_rgb_t){(uint8_t)x, (uint8_t)x, (uint8_t)x};
        }
    }
    for (size_t i = 0; i < p->preset_count; i++) {
        epi_config_t cfg = config(p, W, H);
        cfg.intent = p->presets[i].intent;
        run_converter(&cfg, img, out, NULL);
        double prev_y = -1, prev_dark = 2, prev_light = -1;
        for (int b = 0; b < NB; b++) {
            int count[4] = {0, 0, 0, 0};
            double sy = 0;
            for (int y = 0; y < H; y++) {
                for (int x = b * BAND; x < (b + 1) * BAND; x++) {
                    int l = code_to_index(p, out[(size_t)y * W + x]);
                    count[l]++;
                    sy += epi_grey_tables(p->tables)->level_y[l];
                }
            }
            const double n = BAND * H;
            double my = sy / n, dark = count[0] / n, light = count[3] / n;
            char msg[80];
            snprintf(
                msg,
                sizeof msg,
                "%s band %d: Y %.4f dark %.3f light %.3f",
                p->presets[i].name,
                b,
                my / 65536.0,
                dark,
                light
            );
            TEST_ASSERT_TRUE_MESSAGE(my >= prev_y, msg);
            TEST_ASSERT_TRUE_MESSAGE(dark <= prev_dark, msg);
            TEST_ASSERT_TRUE_MESSAGE(light >= prev_light, msg);
            prev_y = my;
            prev_dark = dark;
            prev_light = light;
        }
        printf(
            "grey ramp %s: last band Y %.3f, lightest share %.3f\n", p->presets[i].name, prev_y / 65536.0, prev_light
        );
        /* The last band (source 240..255, Y .87..1) is mostly the lightest level. */
        TEST_ASSERT_TRUE(prev_y / 65536.0 > 0.9 && prev_light > 0.8);
    }
    free(out);
    free(img);
}

/* Colour to grey is by luminance (CIE Y): pure blue (Y .07) is dark, pure
 * yellow (Y .93) light. HSL lightness would make both 50%. */
void test_grey_converter_colour_luminance(void) {
    const epi_panel_t* ps[2] = {panel4(), panel16()};
    enum { W = 48, H = 48 };
    epi_rgb_t img[W * H];
    uint8_t out[W * H];
    for (int k = 0; k < 2; k++) {
        epi_config_t cfg = config(ps[k], W, H);
        fill_flat(img, W * H, (epi_rgb_t){0, 0, 255});
        run_converter(&cfg, img, out, NULL);
        double blue = mean_y(ps[k], out, W * H);
        fill_flat(img, W * H, (epi_rgb_t){255, 255, 0});
        run_converter(&cfg, img, out, NULL);
        double yellow = mean_y(ps[k], out, W * H);
        fill_flat(img, W * H, (epi_rgb_t){128, 128, 128});
        run_converter(&cfg, img, out, NULL);
        double grey = mean_y(ps[k], out, W * H);
        printf("grey %zu levels: mean Y blue %.3f, mid grey %.3f, yellow %.3f\n", ps[k]->ink_count, blue, grey, yellow);
        TEST_ASSERT_TRUE(blue < 0.15);
        TEST_ASSERT_TRUE(yellow > 0.8);
        TEST_ASSERT_TRUE(blue < grey && grey < yellow);
    }
}

/* Packed rows (2 levels → 1 bpp, 4 → 2 bpp, 16 → 4 bpp) equal the codes packed
 * MSB first, padded with the lightest level's code; previews use the measured
 * and nominal colours; code ranges are checked. */
void test_grey_converter_formats(void) {
    /* 4 levels with reversed codes, so code ≠ index and the pad is code 0. */
    static epi_ink_t rev4[4];
    for (int i = 0; i < 4; i++) {
        rev4[i] = levels4[i];
        rev4[i].code = (uint8_t)(3 - i);
    }
    storage_t* st = &scratch_storage;
    const epi_panel_t* ps[3] = {panel2(), build(st, rev4, 4, 6.0f), panel16()};
    const epi_pack_format_t packed[3] = {EPI_PACK_1BPP, EPI_PACK_2BPP, EPI_PACK_4BPP};
    const unsigned bits[3] = {1, 2, 4};
    const int widths[] = {1, 5, 13, 37};
    for (int k = 0; k < 3; k++) {
        const epi_panel_t* p = ps[k];
        const uint8_t pad = p->inks[p->ink_count - 1].code;
        for (size_t wi = 0; wi < sizeof widths / sizeof widths[0]; wi++) {
            const int w = widths[wi], h = 9;
            epi_rgb_t* img = xmalloc((size_t)w * h * sizeof *img);
            fill_noise(img, (size_t)w * h, (uint32_t)(77 + w + k));
            uint8_t* codes = xmalloc((size_t)w * h);
            epi_config_t cfg = config(p, (uint16_t)w, (uint16_t)h);
            run_converter(&cfg, img, codes, NULL);
            for (int f = 0; f < 3; f++) {
                const unsigned ob = bits[f], per = 8u / ob;
                const size_t rs = epi_pack_row_size(packed[f], (uint16_t)w);
                uint8_t* out = xmalloc(rs);
                if (ob < bits[k]) {
                    /* Codes too wide for this format. */
                    TEST_ASSERT_EQUAL_INT(EPI_ERR_INVALID_ARG, epi_pack_row(p, packed[f], codes, (uint16_t)w, out));
                    free(out);
                    continue;
                }
                for (int y = 0; y < h; y++) {
                    const uint8_t* cr = codes + (size_t)y * w;
                    TEST_ASSERT_EQUAL_INT(EPI_OK, epi_pack_row(p, packed[f], cr, (uint16_t)w, out));
                    for (size_t b = 0; b < rs; b++) {
                        unsigned v = 0;
                        for (unsigned j = 0; j < per; j++) {
                            size_t x = b * per + j;
                            v = (v << ob) | (x < (size_t)w ? cr[x] : pad);
                        }
                        TEST_ASSERT_EQUAL_HEX8(v, out[b]);
                    }
                }
                free(out);
            }
            epi_rgb_t* rgb = xmalloc((size_t)w * h * sizeof *rgb);
            for (int nominal = 0; nominal < 2; nominal++) {
                preview_same_size(p, nominal != 0, codes, w, h, rgb);
                for (int i = 0; i < w * h; i++) {
                    const epi_ink_t* ink = &p->inks[code_to_index(p, codes[i])];
                    TEST_ASSERT_EQUAL_RGB(nominal ? ink->nominal : ink->measured, rgb[i]);
                }
            }
            free(rgb);
            free(codes);
            free(img);
        }
    }
}

/* Same input → same output: dirty workspace, rebuilt panel, re-init. */
void test_grey_deterministic(void) {
    const int w = 29, h = 45;
    epi_rgb_t* img = xmalloc((size_t)w * h * sizeof *img);
    fill_noise(img, (size_t)w * h, 4243);
    epi_config_t cfg = config(panel4(), (uint16_t)w, (uint16_t)h);
    size_t size = epi_converter_workspace_size(&cfg);
    uint8_t* ws = xmalloc(size);
    uint8_t* a = xmalloc((size_t)w * h);
    uint8_t* b = xmalloc((size_t)w * h);
    check_dirty_workspace(&cfg, img, ws, size, a, b);
    run_converter_in(&cfg, img, ws, size, b, NULL); /* re-init a used workspace */
    TEST_ASSERT_EQUAL_MEMORY(a, b, (size_t)w * h);
    /* The same panel built again, in dirty storage. */
    storage_t* st = &scratch_storage;
    memset(st->bytes, 0xee, sizeof st->bytes);
    cfg.panel = build(st, levels4, 4, 6.0f);
    TEST_ASSERT_EQUAL_MEMORY(panel4()->tables, cfg.panel->tables, sizeof(epi_grey_tables_t));
    run_converter_in(&cfg, img, ws, size, b, NULL);
    TEST_ASSERT_EQUAL_MEMORY(a, b, (size_t)w * h);
    free(b);
    free(a);
    free(ws);
    free(img);
}

/*
 * Regression goldens: FNV-1a 64 of the ink codes for each input ×
 * preset (Default, Soft, Deep), on the 4-level and the 2-level test panel.
 * These freeze the output of the verified implementation. Update only on an
 * intentional change: copy the "got" values printed on failure.
 */
#define GOLDEN_W 157
#define GOLDEN_H 119
static const uint64_t grey_goldens[2][3][3] = {
    {
        /* 4 levels: gradient */ {
            UINT64_C(0xc5cc69fade8d27e8),
            UINT64_C(0xcd9073c7edad2066),
            UINT64_C(0xa025ce465defc27c)
        },
        /* 4 levels: hue ramp */
        {UINT64_C(0x95ebc4424ce34bc8), UINT64_C(0x6302ba3f4228847f), UINT64_C(0x3a146e01d91ccfae)},
        /* 4 levels: noise */
        {UINT64_C(0xb0626401ceb68118), UINT64_C(0x37615342db2e00d6), UINT64_C(0xeb03f06cc5d3f37d)},
    },
    {
        /* 2 levels: gradient */ {
            UINT64_C(0xbaf9a9ba00e987c1),
            UINT64_C(0xf078a297d4c9d7d7),
            UINT64_C(0x7ab16ac11562eefe)
        },
        /* 2 levels: hue ramp */
        {UINT64_C(0x119ed85f2b4dbbd4), UINT64_C(0x446b0e6b1fb07208), UINT64_C(0xd5b9a6ee353feefc)},
        /* 2 levels: noise */
        {UINT64_C(0x4ea492e16491f887), UINT64_C(0xaa71edfd0ca99049), UINT64_C(0x8ecc19246b05d02d)},
    },
};

void test_grey_goldens(void) {
    static const char* inputs[3] = {"gradient", "hue ramp", "noise"};
    const epi_panel_t* ps[2] = {panel4(), panel2()};
    const epi_pack_format_t formats[2] = {EPI_PACK_2BPP, EPI_PACK_1BPP};
    const unsigned bits[2] = {2, 1};
    const size_t n = (size_t)GOLDEN_W * GOLDEN_H;
    epi_rgb_t* img = xmalloc(n * sizeof *img);
    uint8_t* out = xmalloc(n);
    char report[2048];
    size_t len = 0;
    int bad = 0;
    for (int k = 0; k < 2; k++) {
        TEST_ASSERT_EQUAL_size_t(3, ps[k]->preset_count);
        len += (size_t)snprintf(report + len, sizeof report - len, "\n    {");
        for (int i = 0; i < 3; i++) {
            switch (i) {
            case 0:
                fill_gradient(img, GOLDEN_W, GOLDEN_H);
                break;
            case 1:
                fill_hue_ramp(img, GOLDEN_W, GOLDEN_H);
                break;
            default:
                fill_noise(img, n, 0x9e3779b9u);
                break;
            }
            len += (size_t)snprintf(
                report + len, sizeof report - len, "\n        /* %d levels: %s */ {", k == 0 ? 4 : 2, inputs[i]
            );
            for (int p = 0; p < 3; p++) {
                epi_config_t cfg = config(ps[k], GOLDEN_W, GOLDEN_H);
                cfg.intent = ps[k]->presets[p].intent;
                run_converter(&cfg, img, out, NULL);
                uint64_t hsh = fnv1a64(out, n);
                bad |= hsh != grey_goldens[k][i][p];
                /* Through the packer, the same codes reach the panel. */
                TEST_ASSERT_TRUE(packed_codes_hash(ps[k], formats[k], bits[k], out, GOLDEN_W, GOLDEN_H) == hsh);
                len += (size_t)snprintf(
                    report + len,
                    sizeof report - len,
                    "UINT64_C(0x%016llx)%s",
                    (unsigned long long)hsh,
                    p < 2 ? ", " : "},"
                );
            }
        }
        len += (size_t)snprintf(report + len, sizeof report - len, "\n    },");
    }
    free(out);
    free(img);
    if (bad) {
        printf("grey golden hashes, got:%s\n", report);
        TEST_FAIL_MESSAGE("grey converter output changed (see the golden hashes printed above)");
    }
}

/* Light blacks move the midtone knee up instead of being rejected: the tone
 * table stays monotone and starts at L*min, for every tone. */
void test_grey_light_black_knee(void) {
    static const uint8_t blacks[] = {72, 100, 120};
    for (size_t b = 0; b < sizeof blacks; b++) {
        const epi_ink_t levels[2] = {
            {0, {0, 0, 0}, {blacks[b], blacks[b], blacks[b]}}, {1, {255, 255, 255}, {200, 200, 200}}
        };
        epi_grey_panel_config_t cfg = {"light", 64, 64, levels, 2, 6.0f};
        const epi_panel_t* p = NULL;
        TEST_ASSERT_TRUE(epi_grey_panel_size(&cfg) <= sizeof scratch_storage.bytes);
        TEST_ASSERT_EQUAL_INT(
            EPI_OK, epi_grey_panel_init(&cfg, scratch_storage.bytes, sizeof scratch_storage.bytes, &p, NULL)
        );
        for (int t = 0; t <= 4; t++) {
            epi_intent_t in = {t / 4.0f, 0.75f, 0.5f, 1.0f, 0.0f, 0.0f};
            epi_intent_ctx_t ctx;
            TEST_ASSERT_EQUAL_INT(EPI_OK, epi_intent_init(&ctx, p->tables, &in));
            for (int i = 1; i < EPI_TONE_TABLE_LEN; i++) {
                TEST_ASSERT_TRUE(ctx.tone_t_q8[i] >= ctx.tone_t_q8[i - 1]);
            }
            TEST_ASSERT_INT_WITHIN(2, ctx.grey_lstar_min_q8, ctx.tone_t_q8[0]);
        }
    }
}
