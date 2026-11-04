/*
 * Tests for colour panels built at runtime (docs/design.md, "Colour panels from
 * inks"; src/epi_colour_panel.c).
 *
 * A successful init costs about a million in-gamut tests and this file also runs
 * under QEMU, so the few panels it builds are built once and shared. The ink sets
 * other than EL133UF1's are plausible measured inks, not calibrations.
 */
#include "epaper_image.h"
#include "epi_math.h"
#include "epi_panel_tables.h"
#include "test_util.h"
#include "unity.h"

#include <float.h>
#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define INK(code, r, g, b)                                                                                             \
    {                                                                                                                  \
        (code), {(r), (g), (b)}, {                                                                                     \
            (r), (g), (b)                                                                                              \
        }                                                                                                              \
    }

/* The smallest kind of set that can pass: white and black, with three inks around
 * the neutral axis. */
static const epi_ink_t inks5[5] = {
    INK(0, 45, 45, 50),
    INK(1, 200, 200, 195),
    INK(2, 170, 50, 50),
    INK(3, 60, 140, 90),
    INK(4, 60, 80, 170),
};

/* 7-colour ACeP-like. */
static const epi_ink_t inks7[7] = {
    INK(0, 45, 45, 50),
    INK(1, 205, 205, 200),
    INK(2, 70, 130, 90),
    INK(3, 60, 80, 160),
    INK(4, 170, 60, 60),
    INK(5, 210, 190, 70),
    INK(6, 190, 120, 70),
};
#define INKS7_WHITE 1

/* The corners of a cube in linear sRGB, so a parallelepiped in XYZ. */
static const epi_ink_t cube8[8] = {
    INK(0, 40, 40, 40),
    INK(1, 220, 40, 40),
    INK(2, 40, 220, 40),
    INK(3, 40, 40, 220),
    INK(4, 220, 220, 40),
    INK(5, 220, 40, 220),
    INK(6, 40, 220, 220),
    INK(7, 220, 220, 220),
};

typedef union {
    long long ll;
    long double ld;
    void* p;
    unsigned char bytes[3072];
} storage_t;

/* Built off alignment on purpose: the storage needs no particular alignment. */
static const epi_panel_t* build(storage_t* st, const epi_ink_t* inks, size_t n, float sigma) {
    epi_colour_panel_config_t cfg = {"TEST", 640, 480, inks, n, sigma, NULL, NULL};
    size_t size = epi_colour_panel_size(&cfg);
    TEST_ASSERT_NOT_EQUAL(0, size);
    TEST_ASSERT_TRUE(size + 3 <= sizeof st->bytes);
    const epi_panel_t* p = NULL;
    TEST_ASSERT_EQUAL_INT(EPI_OK, epi_colour_panel_init(&cfg, st->bytes + 3, size, &p, NULL));
    TEST_ASSERT_NOT_NULL(p);
    return p;
}

static const epi_panel_t* panel_el133(void) {
    static storage_t st;
    static const epi_panel_t* p;
    if (p == NULL) {
        p = build(&st, epi_panel_el133uf1.inks, epi_panel_el133uf1.ink_count, epi_panel_el133uf1.detail_sigma);
    }
    return p;
}

static const epi_panel_t* panel_inks7(void) {
    static storage_t st;
    static const epi_panel_t* p;
    if (p == NULL) {
        p = build(&st, inks7, 7, 5.0f);
    }
    return p;
}

static const epi_panel_t* panel_cube(void) {
    static storage_t st;
    static const epi_panel_t* p;
    if (p == NULL) {
        p = build(&st, cube8, 8, 1.0f);
    }
    return p;
}

/* ---- the structure of the config ---------------------------------------------- */

void test_colour_panel_validation(void) {
    static storage_t st;
    const epi_panel_t sentinel = {0};
    const epi_colour_panel_config_t ok = {"TEST", 800, 480, inks5, 5, 6.0f, NULL, NULL};
    size_t size = epi_colour_panel_size(&ok);
    TEST_ASSERT_NOT_EQUAL(0, size);

    TEST_ASSERT_EQUAL_size_t(0, epi_colour_panel_size(NULL));
    const epi_panel_t* p = &sentinel;
    const char* why = "stale";
    TEST_ASSERT_EQUAL_INT(EPI_ERR_INVALID_ARG, epi_colour_panel_init(NULL, st.bytes, size, &p, &why));
    TEST_ASSERT_NULL(p);
    TEST_ASSERT_EQUAL_STRING("config is NULL", why);
    p = &sentinel;
    why = "stale";
    TEST_ASSERT_EQUAL_INT(EPI_ERR_INVALID_ARG, epi_colour_panel_init(&ok, NULL, size, &p, &why));
    TEST_ASSERT_NULL(p);
    TEST_ASSERT_EQUAL_STRING("storage or out is NULL", why);
    why = "stale";
    TEST_ASSERT_EQUAL_INT(EPI_ERR_INVALID_ARG, epi_colour_panel_init(&ok, st.bytes, size, NULL, &why));
    TEST_ASSERT_EQUAL_STRING("storage or out is NULL", why);
    /* An invalid config is reported as such even when the storage is NULL too. */
    const epi_colour_panel_config_t four_inks = {"TEST", 800, 480, inks5, 4, 6.0f, NULL, NULL};
    why = "stale";
    TEST_ASSERT_EQUAL_INT(EPI_ERR_INVALID_ARG, epi_colour_panel_init(&four_inks, NULL, 0, &p, &why));
    TEST_ASSERT_EQUAL_STRING("a colour panel needs 5 to 16 inks", why);
    /* A NULL reason is accepted, on every kind of return. */
    TEST_ASSERT_EQUAL_INT(EPI_ERR_INVALID_ARG, epi_colour_panel_init(NULL, st.bytes, size, &p, NULL));
    TEST_ASSERT_EQUAL_INT(EPI_ERR_INVALID_ARG, epi_colour_panel_init(&ok, st.bytes, size, NULL, NULL));
    p = &sentinel;
    why = "stale";
    TEST_ASSERT_EQUAL_INT(EPI_ERR_WORKSPACE_TOO_SMALL, epi_colour_panel_init(&ok, st.bytes, size - 1, &p, &why));
    TEST_ASSERT_NULL(p);
    TEST_ASSERT_NULL(why);
    why = "stale";
    TEST_ASSERT_EQUAL_INT(EPI_ERR_WORKSPACE_TOO_SMALL, epi_colour_panel_init(&ok, st.bytes, 0, &p, &why));
    TEST_ASSERT_NULL(why);
    TEST_ASSERT_EQUAL_INT(EPI_ERR_WORKSPACE_TOO_SMALL, epi_colour_panel_init(&ok, st.bytes, 0, &p, NULL));
    /* The structure is judged before the size, and the size before the inks. */
    epi_colour_panel_config_t dud = ok;
    dud.ink_count = 4;
    why = "stale";
    TEST_ASSERT_EQUAL_INT(EPI_ERR_INVALID_ARG, epi_colour_panel_init(&dud, st.bytes, 0, &p, &why));
    TEST_ASSERT_EQUAL_STRING("a colour panel needs 5 to 16 inks", why);
    static const epi_ink_t same[5] = {
        INK(0, 100, 100, 100),
        INK(1, 100, 100, 100),
        INK(2, 100, 100, 100),
        INK(3, 100, 100, 100),
        INK(4, 100, 100, 100)
    };
    dud = ok;
    dud.inks = same;
    dud.ink_count = 5;
    why = "stale";
    TEST_ASSERT_EQUAL_INT(EPI_ERR_WORKSPACE_TOO_SMALL, epi_colour_panel_init(&dud, st.bytes, 0, &p, &why));
    TEST_ASSERT_NULL(why);
    /* Success clears a stale reason. */
    why = "stale";
    TEST_ASSERT_EQUAL_INT(EPI_OK, epi_colour_panel_init(&ok, st.bytes, size, &p, &why));
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_NULL(why);

    static const char* const count_reason = "a colour panel needs 5 to 16 inks";
    static const char* const sigma_reason = "detail sigma outside (0, 10.5]";
    const char* bad_reason[12];
    epi_colour_panel_config_t bad[12];
    size_t nbad = 0;
    bad_reason[nbad] = "name or inks is NULL";
    bad[nbad] = ok, bad[nbad++].name = NULL;
    bad_reason[nbad] = "name or inks is NULL";
    bad[nbad] = ok, bad[nbad++].inks = NULL;
    bad_reason[nbad] = "zero width or height";
    bad[nbad] = ok, bad[nbad++].width = 0;
    bad_reason[nbad] = "zero width or height";
    bad[nbad] = ok, bad[nbad++].height = 0;
    bad_reason[nbad] = count_reason;
    bad[nbad] = ok, bad[nbad++].ink_count = 0;
    bad_reason[nbad] = count_reason;
    bad[nbad] = ok, bad[nbad++].ink_count = 4;
    bad_reason[nbad] = count_reason;
    bad[nbad] = ok, bad[nbad++].ink_count = 17;
    bad_reason[nbad] = sigma_reason;
    bad[nbad] = ok, bad[nbad++].detail_sigma = 0.0f;
    bad_reason[nbad] = sigma_reason;
    bad[nbad] = ok, bad[nbad++].detail_sigma = -1.0f;
    bad_reason[nbad] = sigma_reason;
    bad[nbad] = ok, bad[nbad++].detail_sigma = 10.51f;
    bad_reason[nbad] = sigma_reason;
    bad[nbad] = ok, bad[nbad++].detail_sigma = bits_f(0x7F800000u); /* inf */
    bad_reason[nbad] = sigma_reason;
    bad[nbad] = ok, bad[nbad++].detail_sigma = bits_f(0x7FC00000u); /* NaN */
    for (size_t i = 0; i < nbad; i++) {
        TEST_ASSERT_EQUAL_size_t(0, epi_colour_panel_size(&bad[i]));
        p = &sentinel;
        why = "stale";
        TEST_ASSERT_EQUAL_INT(EPI_ERR_INVALID_ARG, epi_colour_panel_init(&bad[i], st.bytes, sizeof st.bytes, &p, &why));
        TEST_ASSERT_NULL(p);
        TEST_ASSERT_EQUAL_STRING(bad_reason[i], why);
    }

    /* The size doesn't look at the inks, and doesn't depend on the structure. */
    static const epi_ink_t all_black[5] = {
        INK(0, 0, 0, 0), INK(1, 0, 0, 0), INK(2, 0, 0, 0), INK(3, 0, 0, 0), INK(4, 0, 0, 0)
    };
    epi_colour_panel_config_t cfg = {"TEST", 1, 1, all_black, 5, FLT_TRUE_MIN, NULL, NULL};
    TEST_ASSERT_EQUAL_size_t(size, epi_colour_panel_size(&cfg));
    cfg.detail_sigma = 10.5f;
    TEST_ASSERT_EQUAL_size_t(size, epi_colour_panel_size(&cfg));
}

/* ---- the presets ------------------------------------------------------------------ */

void test_colour_panel_presets(void) {
    TEST_ASSERT_EQUAL_size_t(epi_panel_el133uf1.preset_count, EPI_COLOUR_PRESET_COUNT);
    for (size_t i = 0; i < EPI_COLOUR_PRESET_COUNT; i++) {
        const epi_preset_t* a = &epi_colour_presets[i];
        const epi_preset_t* b = &epi_panel_el133uf1.presets[i];
        TEST_ASSERT_EQUAL_STRING(b->name, a->name);
        TEST_ASSERT_TRUE(a->intent->tone == b->intent->tone);
        TEST_ASSERT_TRUE(a->intent->detail == b->intent->detail);
        TEST_ASSERT_TRUE(a->intent->adapt == b->intent->adapt);
        TEST_ASSERT_TRUE(a->intent->vibrance == b->intent->vibrance);
        TEST_ASSERT_TRUE(a->intent->veil == b->intent->veil);
    }
}

/* ---- EL133UF1 ---------------------------------------------------------------------- */

/* Every field of p's tables, exactly: const panels come from this derivation, so
 * this is where the generated tables and the runtime's meet on every target. */
static void assert_tables_equal_generated(const epi_panel_t* p) {
    const epi_colour_tables_t* t = epi_colour_tables(p->tables);
    const epi_colour_tables_t* r = epi_colour_tables(epi_panel_el133uf1.tables);
    TEST_ASSERT_EQUAL_UINT8(r->base.kind, t->base.kind);
    TEST_ASSERT_EQUAL_UINT8(r->base.ink_count, t->base.ink_count);
    TEST_ASSERT_EQUAL_UINT8(r->base.kernel_radius, t->base.kernel_radius);
    TEST_ASSERT_EQUAL_UINT16_ARRAY(r->base.kernel, t->base.kernel, 2 * EPI_MAX_KERNEL_RADIUS + 1);
    TEST_ASSERT_EQUAL_INT32_ARRAY(r->white_xyz, t->white_xyz, 3);
    TEST_ASSERT_EQUAL_INT32_ARRAY(r->black_xyz, t->black_xyz, 3);
    TEST_ASSERT_EQUAL_INT32_ARRAY(&r->lin_to_panel_adapt[0][0], &t->lin_to_panel_adapt[0][0], 9);
    TEST_ASSERT_EQUAL_INT32_ARRAY(&r->lin_to_panel_bpc[0][0], &t->lin_to_panel_bpc[0][0], 9);
    TEST_ASSERT_EQUAL_INT32_ARRAY(&r->xyz_to_lin[0][0], &t->xyz_to_lin[0][0], 9);
    TEST_ASSERT_EQUAL_INT32_ARRAY(&r->lin_to_lms[0][0], &t->lin_to_lms[0][0], 9);
    TEST_ASSERT_EQUAL_INT32_ARRAY(&r->lms_to_oklab[0][0], &t->lms_to_oklab[0][0], 9);
    TEST_ASSERT_EQUAL_UINT8(r->hull_face_count, t->hull_face_count);
    TEST_ASSERT_EQUAL_INT32_ARRAY(&r->hull_n[0][0], &t->hull_n[0][0], 3 * EPI_MAX_HULL_FACES);
    TEST_ASSERT_EQUAL_INT64_ARRAY(r->hull_d, t->hull_d, EPI_MAX_HULL_FACES);
    TEST_ASSERT_EQUAL_INT32(r->lstar_min, t->lstar_min);
    TEST_ASSERT_EQUAL_INT32(r->lstar_max, t->lstar_max);
    TEST_ASSERT_EQUAL_UINT16_ARRAY(r->cusp_lstar, t->cusp_lstar, 360);
    TEST_ASSERT_EQUAL_INT32_ARRAY(&r->ink_lin[0][0], &t->ink_lin[0][0], 3 * EPI_MAX_INKS);
    TEST_ASSERT_EQUAL_INT32_ARRAY(&r->ink_oklab[0][0], &t->ink_oklab[0][0], 3 * EPI_MAX_INKS);
    TEST_ASSERT_EQUAL_INT32_ARRAY(r->nhat, t->nhat, 3);
}

void test_colour_panel_el133uf1(void) {
    const epi_panel_t* p = panel_el133();
    TEST_ASSERT_TRUE(p->presets == epi_colour_presets);
    TEST_ASSERT_EQUAL_size_t(EPI_COLOUR_PRESET_COUNT, p->preset_count);
    TEST_ASSERT_EQUAL_size_t(6, p->ink_count);
    TEST_ASSERT_EQUAL_MEMORY(epi_panel_el133uf1.inks, p->inks, 6 * sizeof p->inks[0]);
    assert_tables_equal_generated(p);
}

/* ---- rejected inks -------------------------------------------------------------- */

static void assert_rejected(const epi_ink_t* inks, size_t n, const char* reason) {
    static epi_colour_tables_t t;
    static storage_t st;
    memset(&t, 0, sizeof t);
    const char* why = NULL;
    TEST_ASSERT_EQUAL_INT(EPI_ERR_INVALID_ARG, epi_colour_tables_derive(inks, n, &t, &why, NULL, NULL));
    TEST_ASSERT_EQUAL_STRING(reason, why);

    /* Sizing doesn't look at the inks (only at their count, which is why fewer than
     * five stop there with the ink-count reason). */
    const epi_colour_panel_config_t cfg = {"TEST", 800, 480, inks, n, 6.0f, NULL, NULL};
    TEST_ASSERT_EQUAL_INT(n >= 5, epi_colour_panel_size(&cfg) != 0);
    const epi_panel_t sentinel = {0};
    const epi_panel_t* p = &sentinel;
    const char* public_reason = "stale";
    TEST_ASSERT_EQUAL_INT(
        EPI_ERR_INVALID_ARG, epi_colour_panel_init(&cfg, st.bytes, sizeof st.bytes, &p, &public_reason)
    );
    TEST_ASSERT_NULL(p);
    TEST_ASSERT_EQUAL_STRING(n >= 5 ? reason : "a colour panel needs 5 to 16 inks", public_reason);
}

/* A strongly tinted black: the neutral axis enters the hull above it, so hues
 * towards it peak in chroma at the very bottom of the neutral range. */
static const epi_ink_t tinted_black[5] =
    {INK(0, 0, 0, 120), INK(1, 240, 240, 240), INK(2, 230, 220, 40), INK(3, 200, 30, 30), INK(4, 30, 170, 40)};
#define TINTED_BLACK_REASON "a hue peaks too close to the end of the neutral range; often a strongly tinted black"

void test_colour_panel_rejections(void) {
    /* In one plane through the origin: r = g throughout. */
    static const epi_ink_t coplanar[5] = {
        INK(0, 30, 30, 30),
        INK(1, 200, 200, 200),
        INK(2, 30, 30, 200),
        INK(3, 200, 200, 30),
        INK(4, 100, 100, 60),
    };
    assert_rejected(coplanar, 5, "inks do not span three dimensions");
    static const epi_ink_t duplicates[4] = {
        INK(0, 100, 100, 100), INK(1, 100, 100, 100), INK(2, 100, 100, 100), INK(3, 100, 100, 100)
    };
    assert_rejected(duplicates, 4, "inks do not span three dimensions");
    static const epi_ink_t collinear[4] = {
        INK(0, 30, 30, 30), INK(1, 100, 100, 100), INK(2, 150, 150, 150), INK(3, 200, 200, 200)
    };
    assert_rejected(collinear, 4, "inks do not span three dimensions");

    /* So dark that white's reciprocal wouldn't fit. */
    static const epi_ink_t too_dark[4] = {INK(0, 2, 2, 2), INK(1, 14, 14, 14), INK(2, 14, 4, 4), INK(3, 4, 4, 14)};
    assert_rejected(too_dark, 4, "white ink too dark");
    static const epi_ink_t white_no_blue[4] = {
        INK(0, 40, 40, 40), INK(1, 210, 210, 0), INK(2, 170, 40, 40), INK(3, 50, 70, 150)
    };
    assert_rejected(white_no_blue, 4, "white ink has a zero channel");
    /* The blue ink has far more short-wave light than the yellowish white. */
    static const epi_ink_t over_white[4] = {
        INK(0, 40, 40, 40), INK(1, 200, 230, 60), INK(2, 170, 40, 40), INK(3, 40, 70, 255)
    };
    assert_rejected(over_white, 4, "ink outside the range the white ink allows");

    /* Black just off the axis, to the far side: the axis meets the hull only at white. */
    static const epi_ink_t off_axis[4] = {
        INK(0, 40, 40, 41), INK(1, 200, 200, 200), INK(2, 170, 40, 40), INK(3, 205, 185, 50)
    };
    assert_rejected(off_axis, 4, "neutral axis misses the ink hull");
    /* The same light black as panelgen's test: a contrast of about 2:1. */
    static const epi_ink_t light_black[4] = {
        INK(0, 170, 170, 170), INK(1, 240, 240, 240), INK(2, 235, 170, 170), INK(3, 170, 180, 240)
    };
    assert_rejected(light_black, 4, "darkest neutral L* is too light for the tone curve");

    assert_rejected(tinted_black, 5, TINTED_BLACK_REASON);
    /* Four inks can't surround the neutral axis: white, black, and colours on one
     * side, so the hues on the other side have no chroma to peak. */
    static const epi_ink_t four[4] = {
        INK(0, 40, 40, 40), INK(1, 200, 200, 200), INK(2, 170, 40, 40), INK(3, 205, 185, 50)
    };
    assert_rejected(four, 4, "a hue peaks too close to the end of the neutral range; often a strongly tinted black");
}

/* ---- the white ink ---------------------------------------------------------------- */

/* INK with a nominal colour of its own. */
#define INKN(code, nr, ng, nb, r, g, b)                                                                                \
    {                                                                                                                  \
        (code), {(nr), (ng), (nb)}, {                                                                                  \
            (r), (g), (b)                                                                                              \
        }                                                                                                              \
    }

#define WHITE_REASON "lightest measured ink is not the white ink"

/* assert_rejected through the public init as well. */
static void assert_rejected_both(const epi_ink_t* inks, size_t n, const char* reason) {
    static storage_t st;
    assert_rejected(inks, n, reason);
    const epi_colour_panel_config_t cfg = {"TEST", 640, 480, inks, n, 6.0f, NULL, NULL};
    const epi_panel_t* p = NULL;
    const char* why = NULL;
    TEST_ASSERT_EQUAL_INT(
        EPI_ERR_INVALID_ARG, epi_colour_panel_init(&cfg, st.bytes, epi_colour_panel_size(&cfg), &p, &why)
    );
    TEST_ASSERT_NULL(p);
    TEST_ASSERT_EQUAL_STRING(reason, why);
}

/* The white ink is the lightest measured; its nominal colour must be the lightest
 * nominal too. */
void test_colour_panel_white_is_lightest_nominal(void) {
    /* A pale yellow, measured lighter than the white, nominally yellow. */
    static const epi_ink_t pale_yellow[5] = {
        INKN(0, 0, 0, 0, 40, 40, 40),
        INKN(1, 255, 255, 255, 204, 204, 204),
        INKN(2, 255, 255, 0, 216, 213, 183),
        INKN(3, 255, 0, 0, 170, 40, 40),
        INKN(4, 0, 0, 255, 50, 70, 150),
    };
    assert_rejected_both(pale_yellow, 5, WHITE_REASON);
    static const epi_ink_t bright_yellow[5] = {
        INKN(0, 0, 0, 0, 40, 40, 40),
        INKN(1, 255, 255, 255, 204, 204, 204),
        INKN(2, 255, 255, 0, 235, 225, 60),
        INKN(3, 255, 0, 0, 170, 40, 40),
        INKN(4, 0, 0, 255, 50, 70, 150),
    };
    assert_rejected_both(bright_yellow, 5, WHITE_REASON);

    /* With every nominal zeroed, or all equal, the check has nothing to compare. */
    epi_ink_t zeroed[5], equal[5];
    memcpy(zeroed, pale_yellow, sizeof zeroed);
    memcpy(equal, pale_yellow, sizeof equal);
    for (int i = 0; i < 5; i++) {
        zeroed[i].nominal = (epi_rgb_t){0, 0, 0};
        equal[i].nominal = (epi_rgb_t){255, 255, 255};
    }
    static epi_colour_tables_t t;
    const char* why = NULL;
    epi_colour_tables_derive(zeroed, 5, &t, &why, NULL, NULL);
    TEST_ASSERT_TRUE(why == NULL || strcmp(why, WHITE_REASON) != 0);
    why = NULL;
    epi_colour_tables_derive(equal, 5, &t, &why, NULL, NULL);
    TEST_ASSERT_TRUE(why == NULL || strcmp(why, WHITE_REASON) != 0);

    /* The white is the lightest nominal too: accepted. */
    static const epi_ink_t consistent[5] = {
        INKN(0, 0, 0, 0, 45, 45, 50),
        INKN(1, 255, 255, 255, 200, 200, 195),
        INKN(2, 255, 0, 0, 170, 50, 50),
        INKN(3, 0, 255, 0, 60, 140, 90),
        INKN(4, 0, 0, 255, 60, 80, 170),
    };
    TEST_ASSERT_EQUAL_INT(EPI_OK, epi_colour_tables_derive(consistent, 5, &t, &why, NULL, NULL));
}

/* ---- the progress callback ------------------------------------------------------- */

typedef struct {
    const void* self; /* the ctx the callback must be given */
    unsigned calls;
    unsigned cancel_at; /* the done at which to return false; 0 never */
    unsigned bad;       /* calls whose done, total or ctx was wrong */
} probe_t;

static bool probe_progress(void* ctx, unsigned done, unsigned total) {
    probe_t* pr = ctx;
    if (ctx != pr->self || done != pr->calls + 1 || total != 360) {
        pr->bad++;
    }
    pr->calls++;
    return done != pr->cancel_at;
}

static epi_err_t init_probed(
    storage_t* st,
    const epi_ink_t* inks,
    size_t n,
    float sigma,
    probe_t* pr,
    const epi_panel_t** p,
    const char** why
) {
    epi_colour_panel_config_t cfg = {"TEST", 640, 480, inks, n, sigma, probe_progress, pr};
    pr->self = pr;
    pr->calls = 0;
    pr->bad = 0;
    *p = (const epi_panel_t*)st; /* stale, to see it cleared */
    *why = "stale";
    return epi_colour_panel_init(&cfg, st->bytes + 3, epi_colour_panel_size(&cfg), p, why);
}

/* Two full derivations: the counting build and the cancel on the last hue. The
 * early cancel is first, so the build is also an init into storage that a cancel
 * left behind. */
void test_colour_panel_progress(void) {
    static storage_t st;
    const epi_ink_t* inks = epi_panel_el133uf1.inks;
    const size_t n = epi_panel_el133uf1.ink_count;
    const float sigma = epi_panel_el133uf1.detail_sigma;
    probe_t pr = {0};
    const epi_panel_t* p;
    const char* why;

    pr.cancel_at = 1;
    TEST_ASSERT_EQUAL_INT(EPI_ERR_CANCELLED, init_probed(&st, inks, n, sigma, &pr, &p, &why));
    TEST_ASSERT_NULL(p);
    TEST_ASSERT_NULL(why);
    TEST_ASSERT_EQUAL_UINT(1, pr.calls);
    TEST_ASSERT_EQUAL_UINT(0, pr.bad);

    /* Counts to the end, then the same panel as the generated one. */
    pr.cancel_at = 0;
    TEST_ASSERT_EQUAL_INT(EPI_OK, init_probed(&st, inks, n, sigma, &pr, &p, &why));
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_NULL(why);
    TEST_ASSERT_EQUAL_UINT(360, pr.calls);
    TEST_ASSERT_EQUAL_UINT(0, pr.bad); /* done 1..360 in order, total 360, ctx given */
    assert_tables_equal_generated(p);

    /* A false on the last call cancels too. */
    pr.cancel_at = 360;
    TEST_ASSERT_EQUAL_INT(EPI_ERR_CANCELLED, init_probed(&st, inks, n, sigma, &pr, &p, &why));
    TEST_ASSERT_NULL(p);
    TEST_ASSERT_NULL(why);
    TEST_ASSERT_EQUAL_UINT(360, pr.calls);
    TEST_ASSERT_EQUAL_UINT(0, pr.bad);
}

/* Inks that are rejected: none of it, or part of it, is reported, and the reason
 * is the usual one. */
void test_colour_panel_progress_rejected(void) {
    static storage_t st;
    probe_t pr = {0};
    const epi_panel_t* p;
    const char* why;

    static const epi_ink_t identical[5] = {
        INK(0, 100, 100, 100),
        INK(1, 100, 100, 100),
        INK(2, 100, 100, 100),
        INK(3, 100, 100, 100),
        INK(4, 100, 100, 100)
    };
    TEST_ASSERT_EQUAL_INT(EPI_ERR_INVALID_ARG, init_probed(&st, identical, 5, 6.0f, &pr, &p, &why));
    TEST_ASSERT_NULL(p);
    TEST_ASSERT_EQUAL_STRING("inks do not span three dimensions", why);
    TEST_ASSERT_EQUAL_UINT(0, pr.calls);

    TEST_ASSERT_EQUAL_INT(EPI_ERR_INVALID_ARG, init_probed(&st, tinted_black, 5, 6.0f, &pr, &p, &why));
    TEST_ASSERT_NULL(p);
    TEST_ASSERT_EQUAL_STRING(TINTED_BLACK_REASON, why);
    TEST_ASSERT_TRUE(pr.calls < 360);
    TEST_ASSERT_EQUAL_UINT(0, pr.bad);
}

/* ---- properties of a panel that isn't EL133UF1 ----------------------------------- */

/* The codes of a flat image of one colour, all of which must be `want`. */
static void assert_flat(const epi_panel_t* p, const epi_intent_t* intent, epi_rgb_t colour, uint8_t want) {
    enum { W = 12, H = 12 };
    const epi_config_t c = {p, intent, W, H};
    size_t need = epi_converter_workspace_size(&c);
    TEST_ASSERT_NOT_EQUAL(0, need);
    void* ws = xmalloc(need);
    epi_converter_t* cv = NULL;
    TEST_ASSERT_EQUAL_INT(EPI_OK, epi_converter_init(&c, ws, need, &cv));
    epi_rgb_t row[W];
    uint8_t out[W];
    for (int x = 0; x < W; x++) {
        row[x] = colour;
    }
    int got = 0;
    for (int y = 0; y < H; y++) {
        TEST_ASSERT_EQUAL_INT(EPI_OK, epi_converter_push(cv, row));
        while (epi_converter_pop(cv, out) == EPI_OK) {
            got++;
            for (int x = 0; x < W; x++) {
                TEST_ASSERT_EQUAL_UINT8(want, out[x]);
            }
        }
    }
    TEST_ASSERT_EQUAL_INT(H, got);
    free(ws);
}

/* ---- pure black ------------------------------------------------------------------- */

/* A (0, 0, 0) black is a panel like any other: accepted, and it converts under
 * every preset, a grey ramp and a saturated hue sweep. */
void test_colour_panel_pure_black(void) {
    static const epi_ink_t pure_black[5] = {
        INK(0, 0, 0, 0), INK(1, 200, 200, 200), INK(2, 170, 40, 40), INK(3, 60, 140, 90), INK(4, 50, 70, 150)
    };
    static storage_t st;
    const epi_panel_t* p = build(&st, pure_black, 5, 4.0f);
    TEST_ASSERT_TRUE(epi_colour_tables(p->tables)->lstar_min >= 0);
    TEST_ASSERT_EQUAL_size_t(EPI_COLOUR_PRESET_COUNT, p->preset_count);

    enum { W = 64, H = 40 };
    for (size_t k = 0; k < p->preset_count; k++) {
        const epi_config_t c = {p, p->presets[k].intent, W, H};
        size_t need = epi_converter_workspace_size(&c);
        TEST_ASSERT_NOT_EQUAL(0, need);
        void* ws = xmalloc(need);
        epi_converter_t* cv = NULL;
        TEST_ASSERT_EQUAL_INT(EPI_OK, epi_converter_init(&c, ws, need, &cv));
        epi_rgb_t row[W];
        uint8_t out[W];
        int got = 0;
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                if (y < H / 2) { /* grey ramp, from (0, 0, 0) */
                    const uint8_t v = (uint8_t)(x * 255 / (W - 1));
                    row[x] = (epi_rgb_t){v, v, v};
                } else { /* saturated hues: one channel at 255, one at 0, one sweeping */
                    const uint8_t v = (uint8_t)(x * 255 / (W - 1));
                    const int h = (y - H / 2) % 3;
                    row[x] = h == 0 ? (epi_rgb_t){255, v, 0} : h == 1 ? (epi_rgb_t){0, 255, v} : (epi_rgb_t){v, 0, 255};
                }
            }
            TEST_ASSERT_EQUAL_INT(EPI_OK, epi_converter_push(cv, row));
            while (epi_converter_pop(cv, out) == EPI_OK) {
                got++;
                for (int x = 0; x < W; x++) {
                    size_t i = 0;
                    while (i < p->ink_count && p->inks[i].code != out[x]) {
                        i++;
                    }
                    TEST_ASSERT_TRUE(i < p->ink_count);
                }
            }
        }
        TEST_ASSERT_EQUAL_INT(H, got);
        free(ws);
        assert_flat(p, p->presets[k].intent, (epi_rgb_t){0, 0, 0}, pure_black[0].code);
    }
}

/* What the derivation lowers each hull offset for: q(M·lin, 16), from libm here. */
static int32_t ink_xyz_q16(epi_rgb_t m, int row) {
    static const double M[3][3] = {{0.4124, 0.3576, 0.1805}, {0.2126, 0.7152, 0.0722}, {0.0193, 0.1192, 0.9505}};
    const uint8_t u[3] = {m.r, m.g, m.b};
    double v = 0.0;
    for (int c = 0; c < 3; c++) {
        const double x = u[c] / 255.0;
        v += M[row][c] * (x <= 0.04045 ? x / 12.92 : pow((x + 0.055) / 1.055, 2.4));
    }
    return (int32_t)floor(v * 65536.0 + 0.5);
}

void test_colour_panel_properties(void) {
    const epi_panel_t* p = panel_inks7();
    const epi_colour_tables_t* t = epi_colour_tables(p->tables);

    /* Every ink tests inside the Q16 hull, in the integer arithmetic of S5. */
    TEST_ASSERT_TRUE(t->hull_face_count >= 4);
    for (size_t i = 0; i < p->ink_count; i++) {
        const int32_t xyz[3] = {
            ink_xyz_q16(inks7[i].measured, 0), ink_xyz_q16(inks7[i].measured, 1), ink_xyz_q16(inks7[i].measured, 2)
        };
        for (unsigned f = 0; f < t->hull_face_count; f++) {
            TEST_ASSERT_TRUE(epi_dot3(t->hull_n[f], xyz) + t->hull_d[f] <= 0);
        }
    }
    TEST_ASSERT_EQUAL_INT32_ARRAY(((const int32_t[]){65536, 0, 0}), t->ink_oklab[INKS7_WHITE], 3);
    /* M_adapt's rows sum exactly to W − B. */
    for (int r = 0; r < 3; r++) {
        TEST_ASSERT_EQUAL_INT64(
            (int64_t)t->white_xyz[r] - t->black_xyz[r],
            (int64_t)t->lin_to_panel_adapt[r][0] + t->lin_to_panel_adapt[r][1] + t->lin_to_panel_adapt[r][2]
        );
    }
    for (int hue = 0; hue < 360; hue++) {
        TEST_ASSERT_TRUE(t->cusp_lstar[hue] >= t->lstar_min + EPI_CUSP_MARGIN_Q8);
        TEST_ASSERT_TRUE(t->cusp_lstar[hue] <= t->lstar_max - EPI_CUSP_MARGIN_Q8);
    }

    /* The white ink code passes through a flat-white image, under every preset. */
    const epi_rgb_t white = {255, 255, 255};
    for (size_t i = 0; i < p->preset_count; i++) {
        assert_flat(p, p->presets[i].intent, white, inks7[INKS7_WHITE].code);
    }
}

void test_colour_panel_cube(void) {
    const epi_panel_t* p = panel_cube();
    /* Coplanar inks: every triple of a square face merges into one face. */
    TEST_ASSERT_EQUAL_UINT8(6, epi_colour_tables(p->tables)->hull_face_count);
    assert_flat(p, NULL, (epi_rgb_t){255, 255, 255}, cube8[7].code);
}

/* ---- running a converter ----------------------------------------------------------- */

static void convert_small(const epi_panel_t* p) {
    enum { W = 64, H = 40 };
    const epi_config_t c = {p, NULL, W, H};
    size_t need = epi_converter_workspace_size(&c);
    TEST_ASSERT_NOT_EQUAL(0, need);
    void* ws = xmalloc(need);
    epi_converter_t* cv = NULL;
    TEST_ASSERT_EQUAL_INT(EPI_OK, epi_converter_init(&c, ws, need, &cv));
    epi_rgb_t row[W];
    uint8_t out[W];
    int got = 0;
    unsigned used = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            row[x] = (epi_rgb_t){(uint8_t)(x * 4), (uint8_t)(y * 6), (uint8_t)(255 - x * 3)};
        }
        TEST_ASSERT_EQUAL_INT(EPI_OK, epi_converter_push(cv, row));
        while (epi_converter_pop(cv, out) == EPI_OK) {
            got++;
            for (int x = 0; x < W; x++) {
                size_t i = 0;
                while (i < p->ink_count && p->inks[i].code != out[x]) {
                    i++;
                }
                TEST_ASSERT_TRUE(i < p->ink_count);
                used |= 1u << i;
            }
        }
    }
    TEST_ASSERT_EQUAL_INT(H, got);
    TEST_ASSERT_TRUE(used & (used - 1)); /* more than one ink */
    free(ws);
}

void test_colour_panel_converter(void) {
    static storage_t st;
    convert_small(panel_el133());
    convert_small(panel_inks7());
    convert_small(panel_cube());
    convert_small(build(&st, inks5, 5, 2.0f));
}

/* ---- FNV-1a 64 of the tables, field by field, with no padding --------------------- */

static uint64_t hash_u64(uint64_t h, uint64_t v, unsigned bytes) {
    for (unsigned i = 0; i < bytes; i++) {
        h ^= (v >> (8 * i)) & 0xFFu;
        h *= UINT64_C(0x100000001B3);
    }
    return h;
}

static uint64_t hash_i32s(uint64_t h, const int32_t* v, size_t n) {
    for (size_t i = 0; i < n; i++) {
        h = hash_u64(h, (uint64_t)(uint32_t)v[i], 4);
    }
    return h;
}

static uint64_t hash_tables(const epi_colour_tables_t* t) {
    uint64_t h = UINT64_C(0xCBF29CE484222325);
    h = hash_u64(h, t->base.kind, 1);
    h = hash_u64(h, t->base.ink_count, 1);
    h = hash_u64(h, t->base.kernel_radius, 1);
    for (int i = 0; i < 2 * EPI_MAX_KERNEL_RADIUS + 1; i++) {
        h = hash_u64(h, t->base.kernel[i], 2);
    }
    h = hash_i32s(h, t->white_xyz, 3);
    h = hash_i32s(h, t->black_xyz, 3);
    h = hash_i32s(h, &t->lin_to_panel_adapt[0][0], 9);
    h = hash_i32s(h, &t->lin_to_panel_bpc[0][0], 9);
    h = hash_i32s(h, &t->xyz_to_lin[0][0], 9);
    h = hash_i32s(h, &t->lin_to_lms[0][0], 9);
    h = hash_i32s(h, &t->lms_to_oklab[0][0], 9);
    h = hash_u64(h, t->hull_face_count, 1);
    h = hash_i32s(h, &t->hull_n[0][0], 3 * EPI_MAX_HULL_FACES);
    for (int f = 0; f < EPI_MAX_HULL_FACES; f++) {
        h = hash_u64(h, (uint64_t)t->hull_d[f], 8);
    }
    h = hash_i32s(h, &t->lstar_min, 1);
    h = hash_i32s(h, &t->lstar_max, 1);
    for (int i = 0; i < 360; i++) {
        h = hash_u64(h, t->cusp_lstar[i], 2);
    }
    h = hash_i32s(h, &t->ink_lin[0][0], 3 * EPI_MAX_INKS);
    h = hash_i32s(h, &t->ink_oklab[0][0], 3 * EPI_MAX_INKS);
    h = hash_i32s(h, t->nhat, 3);
    return h;
}

void test_colour_panel_reinit_from_own_inks(void) {
    static storage_t st;
    const epi_panel_t* p = build(&st, inks5, 5, 2.0f);
    const uint64_t before = hash_tables(epi_colour_tables(p->tables));
    const epi_panel_t* again = build(&st, p->inks, p->ink_count, 2.0f);
    TEST_ASSERT_TRUE(again == p);
    TEST_ASSERT_EQUAL_MEMORY(inks5, again->inks, sizeof inks5);
    TEST_ASSERT_TRUE(before == hash_tables(epi_colour_tables(again->tables)));
}

#define FP_EL133 UINT64_C(0x1abc22e704c250d0)
#define FP_INKS7 UINT64_C(0xb8254b34d4080507)
#define FP_CUBE UINT64_C(0xd6424fb09307e364)

void test_colour_panel_fingerprint(void) {
    const uint64_t el133 = hash_tables(epi_colour_tables(panel_el133()->tables));
    const uint64_t i7 = hash_tables(epi_colour_tables(panel_inks7()->tables));
    const uint64_t cube = hash_tables(epi_colour_tables(panel_cube()->tables));
    printf(
        "fingerprint colour tables el133uf1 0x%016" PRIx64 ", inks7 0x%016" PRIx64 ", cube 0x%016" PRIx64 "\n",
        el133,
        i7,
        cube
    );
    TEST_ASSERT_TRUE(el133 == FP_EL133);
    TEST_ASSERT_TRUE(i7 == FP_INKS7);
    TEST_ASSERT_TRUE(cube == FP_CUBE);
}

/* ---- the cusp margin: cusp_move's arithmetic, in exact integers -------------------- */

typedef struct {
    int64_t k;
    uint64_t e1;  /* e1² fits int64 iff e1 ≤ E1_MAX */
    uint64_t kld; /* 2·k·|ld| */
} cusp_arith_t;

#define E1_MAX UINT64_C(3037000499) /* ⌊sqrt(2^63 − 1)⌋ */

static cusp_arith_t cusp_arith(int64_t lmin, int64_t lmax, int64_t lc, int64_t l, int64_t c) {
    const int64_t span = lmax - lmin, ld = l - lc;
    const int64_t k = 2 * (ld > 0 ? lmax - lc : lc - lmin);
    const int64_t ald = ld < 0 ? -ld : ld;
    const uint64_t num = (uint64_t)c * (uint64_t)span, den = (uint64_t)(20 * k);
    cusp_arith_t a = {k, (uint64_t)(k / 2 + ald) + (num + den / 2) / den, 2 * (uint64_t)k * (uint64_t)ald};
    return a;
}

/* The bound in epi_panel_tables.h: widest span, the margin, the largest c. */
void test_colour_panel_cusp_margin(void) {
    const int64_t lmin = 0, lmax = (int64_t)100 << 16;
    const int64_t c = (INT64_C(1) << 29) - 1;
    const int64_t margin = EPI_CUSP_MARGIN_Q8 << 8;
    const int64_t centres[3] = {margin, lmax / 2, lmax - margin};
    for (int i = 0; i < 3; i++) {
        const int64_t lc = centres[i];
        const int64_t ls[5] = {lmin, lc - 1, lc, lc + 1, lmax};
        for (int j = 0; j < 5; j++) {
            const cusp_arith_t a = cusp_arith(lmin, lmax, lc, ls[j], c);
            TEST_ASSERT_TRUE(a.k >= 1 << 16);
            TEST_ASSERT_TRUE(a.e1 <= E1_MAX);
            TEST_ASSERT_TRUE(a.kld < UINT64_C(1) << 63);
        }
    }
    /* Half the margin is not enough: the check is not vacuous. */
    TEST_ASSERT_TRUE(cusp_arith(lmin, lmax, margin / 2, lmin, c).e1 > E1_MAX);
}
