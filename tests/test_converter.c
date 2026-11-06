/*
 * Tests for the streaming converter (src/epi_converter.c): validation,
 * workspace, push/pop lag and flush, what its codes pack and preview as,
 * streaming == whole image, and regression goldens.
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L /* fork, waitpid, _exit for the death test */
#endif

#include "epaper_image.h"
#include "epi_dither.h"
#include "epi_intent.h"
#include "epi_panel_tables.h"
#include "test_util.h"
#include "unity.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(NDEBUG) && (defined(__unix__) || defined(__APPLE__)) && !defined(__EMSCRIPTEN__)
#define HAVE_DEATH_TEST 1
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

static const epi_panel_t* panel(void) {
    return &epi_panel_el133uf1;
}

static int radius(void) {
    return panel()->tables->kernel_radius;
}

/* ---- deterministic inputs ------------------------------------------------ */

/* White page with a mid-grey bar across rows h/3 .. h/2. */
static void fill_page(epi_rgb_t* img, int w, int h) {
    for (int y = 0; y < h; y++) {
        uint8_t v = (y >= h / 3 && y < h / 2) ? 128 : 255;
        for (int x = 0; x < w; x++) {
            epi_rgb_t* p = &img[(size_t)y * w + x];
            p->r = p->g = p->b = v;
        }
    }
}

/* Whole-image reference: the stages over full buffers, edge rows
 * replicated by clamping. idx_out is h × w palette indices. */
static void reference_indices(const epi_intent_t* intent, const epi_rgb_t* img, int w, int h, uint8_t* idx_out) {
    const epi_panel_tables_t* t = panel()->tables;
    const int r = t->kernel_radius;
    epi_intent_ctx_t* ctx = xmalloc(sizeof *ctx);
    TEST_ASSERT_EQUAL_INT(EPI_OK, epi_intent_init(ctx, t, intent));
    size_t n = (size_t)w * h;
    int16_t* ls = xmalloc((size_t)w * sizeof *ls);
    int16_t* hb = xmalloc(n * sizeof *hb);
    int16_t* base = xmalloc((size_t)w * sizeof *base);
    int32_t (*target)[3] = xmalloc((size_t)w * sizeof *target);
    const int16_t* rows[2 * EPI_MAX_KERNEL_RADIUS + 1];
    void* err = xmalloc(epi_dither_err_size(t, (uint16_t)w));
    epi_dither_ctx_t d;
    epi_dither_init(&d, t, (uint16_t)w, err);
    for (int y = 0; y < h; y++) {
        epi_intent_lstar_row(img + (size_t)y * w, ls, (uint16_t)w);
        epi_blur_h(t, ls, hb + (size_t)y * w, (uint16_t)w);
    }
    for (int y = 0; y < h; y++) {
        clamp_rows(hb, w, h, y, r, rows);
        epi_blur_v(t, rows, base, (uint16_t)w);
        epi_intent_row(ctx, img + (size_t)y * w, base, target, (uint16_t)w);
        epi_dither_row(&d, (const int32_t (*)[3])target, idx_out + (size_t)y * w);
    }
    free(err);
    free(target);
    free(base);
    free(hb);
    free(ls);
    free(ctx);
}

/* The converter's codes equal the whole-image reference. */
static void check_streaming_equals_whole(const epi_intent_t* intent, const epi_rgb_t* img, int w, int h) {
    epi_config_t cfg = config(panel(), (uint16_t)w, (uint16_t)h);
    cfg.intent = intent;
    size_t n = (size_t)w * h;
    uint8_t* codes = xmalloc(n);
    uint8_t* ref = xmalloc(n);
    int first = -1;
    TEST_ASSERT_EQUAL_INT(h, run_converter(&cfg, img, codes, &first));
    int r = radius();
    TEST_ASSERT_EQUAL_INT(r < h - 1 ? r : h - 1, first);
    reference_indices(intent != NULL ? intent : panel()->presets[0].intent, img, w, h, ref);
    for (size_t i = 0; i < n; i++) {
        int idx = code_to_index(panel(), codes[i]);
        if (idx != ref[i]) {
            char msg[96];
            snprintf(
                msg,
                sizeof msg,
                "%dx%d: pixel %zu code %u, reference index %u",
                w,
                h,
                i,
                (unsigned)codes[i],
                (unsigned)ref[i]
            );
            TEST_FAIL_MESSAGE(msg);
        }
    }
    free(ref);
    free(codes);
}

/* ---- tests ----------------------------------------------------------------- */

void test_converter_validation(void) {
    epi_config_t cfg = config(panel(), 16, 8);
    size_t size = epi_converter_workspace_size(&cfg);
    TEST_ASSERT_NOT_EQUAL(0, size);
    void* ws = xmalloc(size);
    epi_converter_t* c = (epi_converter_t*)ws;

    TEST_ASSERT_EQUAL_size_t(0, epi_converter_workspace_size(NULL));
    TEST_ASSERT_EQUAL_INT(EPI_ERR_INVALID_ARG, epi_converter_init(NULL, ws, size, &c));
    TEST_ASSERT_NULL(c);
    TEST_ASSERT_EQUAL_INT(EPI_ERR_INVALID_ARG, epi_converter_init(&cfg, NULL, size, &c));
    TEST_ASSERT_EQUAL_INT(EPI_ERR_INVALID_ARG, epi_converter_init(&cfg, ws, size, NULL));

    /* Each structurally invalid config: size 0 and INVALID_ARG. */
    epi_panel_t no_tables = *panel();
    no_tables.tables = NULL;
    epi_panel_t no_inks = *panel();
    no_inks.inks = NULL;
    epi_panel_t ink_mismatch = *panel();
    ink_mismatch.ink_count = 5;
    epi_panel_t too_many = *panel();
    too_many.ink_count = EPI_MAX_INKS + 1;
    epi_panel_t no_presets = *panel();
    no_presets.presets = NULL;
    no_presets.preset_count = 0;

    epi_config_t bad[8];
    int nbad = 0;
    bad[nbad] = cfg, bad[nbad++].width = 0;
    bad[nbad] = cfg, bad[nbad++].height = 0;
    bad[nbad] = cfg, bad[nbad++].panel = NULL;
    bad[nbad] = cfg, bad[nbad++].panel = &no_tables;
    bad[nbad] = cfg, bad[nbad++].panel = &no_inks;
    bad[nbad] = cfg, bad[nbad++].panel = &ink_mismatch;
    bad[nbad] = cfg, bad[nbad++].panel = &too_many;
    bad[nbad] = cfg, bad[nbad++].panel = &no_presets; /* intent NULL needs presets[0] */
    for (int i = 0; i < nbad; i++) {
        TEST_ASSERT_EQUAL_size_t_MESSAGE(0, epi_converter_workspace_size(&bad[i]), "size");
        c = (epi_converter_t*)ws;
        TEST_ASSERT_EQUAL_INT_MESSAGE(EPI_ERR_INVALID_ARG, epi_converter_init(&bad[i], ws, size, &c), "init");
        TEST_ASSERT_NULL(c);
    }

    /* An explicit intent with no presets is fine. */
    epi_config_t ok = cfg;
    ok.panel = &no_presets;
    ok.intent = panel()->presets[0].intent;
    TEST_ASSERT_EQUAL_INT(EPI_OK, epi_converter_init(&ok, ws, size, &c));

    /* Intent ranges: checked by init. */
    const float bad_vals[][5] = {
        {-0.1f, .75f, .5f, 1, 0},
        {1.1f, .75f, .5f, 1, 0},
        {.5f, 1.5f, .5f, 1, 0},
        {.5f, .75f, -1, 1, 0},
        {.5f, .75f, .5f, 3.5f, 0},
        {.5f, .75f, .5f, 1, 4},
    };
    for (size_t i = 0; i < sizeof bad_vals / sizeof bad_vals[0]; i++) {
        epi_intent_t in = {bad_vals[i][0], bad_vals[i][1], bad_vals[i][2], bad_vals[i][3], bad_vals[i][4], 0.0f};
        epi_config_t bi = cfg;
        bi.intent = &in;
        TEST_ASSERT_EQUAL_INT(EPI_ERR_INVALID_ARG, epi_converter_init(&bi, ws, size, &c));
        TEST_ASSERT_NULL(c);
    }

    /* Any code is fine: only epi_pack_row limits their width. */
    epi_ink_t inks[EPI_MAX_INKS];
    memcpy(inks, panel()->inks, panel()->ink_count * sizeof inks[0]);
    inks[3].code = 16;
    epi_panel_t wide_codes = *panel();
    wide_codes.inks = inks;
    epi_config_t pk = cfg;
    pk.panel = &wide_codes;
    TEST_ASSERT_NOT_EQUAL(0, epi_converter_workspace_size(&pk));
    TEST_ASSERT_EQUAL_INT(EPI_OK, epi_converter_init(&pk, ws, size, &c));

    /* NULL converter / row: invalid argument. */
    const epi_rgb_t px = {0, 0, 0};
    TEST_ASSERT_EQUAL_INT(EPI_ERR_INVALID_ARG, epi_converter_push(NULL, NULL));
    TEST_ASSERT_EQUAL_INT(EPI_ERR_INVALID_ARG, epi_converter_push(NULL, &px));
    TEST_ASSERT_EQUAL_INT(EPI_ERR_INVALID_ARG, epi_converter_pop(NULL, ws));
    TEST_ASSERT_EQUAL_INT(EPI_OK, epi_converter_init(&cfg, ws, size, &c));
    TEST_ASSERT_EQUAL_INT(EPI_ERR_INVALID_ARG, epi_converter_push(c, NULL));
    TEST_ASSERT_EQUAL_INT(EPI_ERR_INVALID_ARG, epi_converter_pop(c, NULL));
    free(ws);
}

/* Corrupt tables are refused up front: size 0 and INVALID_ARG. */
static void check_tables(const epi_panel_t* base, const epi_panel_tables_t* t, bool valid, const char* what) {
    epi_panel_t p = *base;
    p.tables = t;
    epi_config_t cfg = config(&p, 16, 8);
    const size_t cap = 64 * 1024;
    void* ws = xmalloc(cap);
    epi_converter_t* c = (epi_converter_t*)ws;
    const size_t size = epi_converter_workspace_size(&cfg);
    const epi_err_t e = epi_converter_init(&cfg, ws, cap, &c);
    if (valid) {
        TEST_ASSERT_TRUE_MESSAGE(size > 0 && size <= cap, what);
        TEST_ASSERT_EQUAL_INT_MESSAGE(EPI_OK, e, what);
        TEST_ASSERT_NOT_NULL_MESSAGE(c, what);
    } else {
        TEST_ASSERT_EQUAL_size_t_MESSAGE(0, size, what);
        TEST_ASSERT_EQUAL_INT_MESSAGE(EPI_ERR_INVALID_ARG, e, what);
        TEST_ASSERT_NULL_MESSAGE(c, what);
    }
    free(ws);
}

#define COLOUR_CASE(from, valid, change)                                                                               \
    do {                                                                                                               \
        t = *epi_colour_tables((from)->tables);                                                                        \
        change;                                                                                                        \
        check_tables((from), &t.base, (valid), #change);                                                               \
    } while (0)

#define GREY_CASE(from, valid, change)                                                                                 \
    do {                                                                                                               \
        g = *epi_grey_tables((from)->tables);                                                                          \
        change;                                                                                                        \
        check_tables((from), &g.base, (valid), #change);                                                               \
    } while (0)

void test_converter_corrupt_tables(void) {
    static epi_colour_tables_t t;
    static epi_grey_tables_t g;
    const epi_panel_t* col = &epi_panel_el133uf1;
    COLOUR_CASE(col, true, (void)t);
    COLOUR_CASE(col, false, t.base.kind = 2);
    COLOUR_CASE(col, false, t.base.kind = 255);
    COLOUR_CASE(col, false, t.base.kernel_radius = EPI_MAX_KERNEL_RADIUS + 1);
    COLOUR_CASE(col, true, t.hull_face_count = EPI_MAX_HULL_FACES);
    COLOUR_CASE(col, false, t.hull_face_count = EPI_MAX_HULL_FACES + 1);
    COLOUR_CASE(col, true, t.lstar_min = 0);
    COLOUR_CASE(col, false, t.lstar_min = -1);
    COLOUR_CASE(col, false, t.lstar_min = t.lstar_max);
    COLOUR_CASE(col, false, t.lstar_min = t.lstar_max + 1);
    COLOUR_CASE(col, true, t.lstar_max = 100 * 256);
    COLOUR_CASE(col, false, t.lstar_max = 100 * 256 + 1);
    COLOUR_CASE(col, true, t.black_xyz[1] = 0);
    COLOUR_CASE(col, false, t.black_xyz[1] = -1);
    COLOUR_CASE(col, false, t.black_xyz[1] = t.white_xyz[1]);
    for (int i = 0; i < 3; i++) {
        COLOUR_CASE(col, false, t.white_xyz[i] = 512);
        COLOUR_CASE(col, false, t.white_xyz[i] = 0);
        COLOUR_CASE(col, false, t.white_xyz[i] = -5);
    }
    COLOUR_CASE(col, true, t.white_xyz[0] = 513);
    /* The midtone knee, on an otherwise sound neutral interval. */
    COLOUR_CASE(col, true, t.lstar_max = 100 * 256; t.lstar_min = 65 * 256);
    COLOUR_CASE(col, false, t.lstar_max = 100 * 256; t.lstar_min = 67 * 256);

    const epi_panel_t* grey = &epi_panel_gdey075t7_mono;
    GREY_CASE(grey, true, (void)g);
    GREY_CASE(grey, false, g.base.kind = 2);
    GREY_CASE(grey, false, g.level_y[0] = -1);
    GREY_CASE(grey, false, g.level_y[1] = 65535);
    GREY_CASE(grey, false, g.level_y[1] = 65537);
    GREY_CASE(grey, false, g.level_y[0] = g.level_y[1]);
    GREY_CASE(grey, false, g.level_thr[0] = g.level_y[0] - 1);
    GREY_CASE(grey, false, g.level_thr[0] = g.level_y[1]);
    GREY_CASE(grey, true, g.level_y[0] = 0; g.level_thr[0] = 0);
    GREY_CASE(grey, true, g.level_y[0] = 20000; g.level_thr[0] = 20000);
    GREY_CASE(grey, false, g.level_y[0] = 42000; g.level_thr[0] = 42000); /* the knee */
    /* ink_count on the panel too, or the mismatch check would hide the tables'. */
    epi_panel_t one = *grey;
    one.ink_count = 1;
    GREY_CASE(&one, false, g.base.ink_count = 1);
    epi_panel_t many = *grey;
    many.ink_count = EPI_MAX_INKS + 1;
    GREY_CASE(&many, false, g.base.ink_count = EPI_MAX_INKS + 1);
}

void test_converter_workspace(void) {
    epi_config_t cfg = config(panel(), 1, 10);
    size_t prev = 0;
    for (unsigned w = 1; w <= 2048; w += (w < 64 ? 1 : 61)) {
        cfg.width = (uint16_t)w;
        size_t s = epi_converter_workspace_size(&cfg);
        TEST_ASSERT_TRUE(s > prev);
        prev = s;
        cfg.height = 1600; /* height does not matter */
        TEST_ASSERT_EQUAL_size_t(s, epi_converter_workspace_size(&cfg));
        cfg.height = 10;
    }
    cfg.width = 65535;
    TEST_ASSERT_TRUE(epi_converter_workspace_size(&cfg) > prev);

    /* Too small by one byte fails at any alignment; exact size works at any alignment. */
    cfg = config(panel(), 37, 5);
    size_t size = epi_converter_workspace_size(&cfg);
    uint8_t* buf = xmalloc(size + 64);
    epi_rgb_t img[37 * 5];
    fill_noise(img, 37 * 5, 7);
    uint8_t ref[37 * 5], out[37 * 5];
    TEST_ASSERT_EQUAL_INT(5, run_converter(&cfg, img, ref, NULL));
    for (int off = 0; off < 32; off++) {
        epi_converter_t* c = (epi_converter_t*)buf;
        TEST_ASSERT_EQUAL_INT(EPI_ERR_WORKSPACE_TOO_SMALL, epi_converter_init(&cfg, buf + off, size - 1, &c));
        TEST_ASSERT_NULL(c);
        TEST_ASSERT_EQUAL_INT(EPI_ERR_WORKSPACE_TOO_SMALL, epi_converter_init(&cfg, buf + off, 0, &c));
        TEST_ASSERT_EQUAL_INT(5, run_converter_in(&cfg, img, buf + off, size, out, NULL));
        TEST_ASSERT_EQUAL_MEMORY(ref, out, sizeof ref);
    }
    free(buf);
}

/* Lag, flush and counts for heights around the radius, and tiny images,
 * each equal to the whole-image reference. */
void test_converter_push_pop_counts(void) {
    const int r = radius();
    const int heights[] = {1, 2, r - 1, r, r + 1, r + 2, 2 * r, 2 * r + 1, 2 * r + 2, 3 * r + 5};
    const int widths[] = {1, 2, 7};
    for (size_t hi = 0; hi < sizeof heights / sizeof heights[0]; hi++) {
        for (size_t wi = 0; wi < sizeof widths / sizeof widths[0]; wi++) {
            int w = widths[wi], h = heights[hi];
            epi_rgb_t* img = xmalloc((size_t)w * h * sizeof *img);
            fill_noise(img, (size_t)w * h, (uint32_t)(h * 131 + w));
            check_streaming_equals_whole(NULL, img, w, h);
            free(img);
        }
    }
    /* W × 1 and 1 × H at larger sizes. */
    epi_rgb_t* img = xmalloc(300 * sizeof *img);
    fill_noise(img, 300, 99);
    check_streaming_equals_whole(NULL, img, 300, 1);
    check_streaming_equals_whole(NULL, img, 1, 300);
    free(img);
}

/* Streaming equals whole image on a larger input, for every preset. */
void test_converter_streaming_equals_whole(void) {
    const int w = 61, h = 97;
    epi_rgb_t* img = xmalloc((size_t)w * h * sizeof *img);
    fill_hue_ramp(img, w, h);
    for (size_t p = 0; p < panel()->preset_count; p++) {
        check_streaming_equals_whole(panel()->presets[p].intent, img, w, h);
    }
    fill_noise(img, (size_t)w * h, 12345);
    check_streaming_equals_whole(panel()->presets[0].intent, img, w, h);
    free(img);
}

void test_converter_null_intent_is_default(void) {
    const int w = 33, h = 41;
    epi_rgb_t* img = xmalloc((size_t)w * h * sizeof *img);
    fill_gradient(img, w, h);
    uint8_t* a = xmalloc((size_t)w * h);
    uint8_t* b = xmalloc((size_t)w * h);
    epi_config_t cfg = config(panel(), (uint16_t)w, (uint16_t)h);
    run_converter(&cfg, img, a, NULL);
    epi_intent_t def = *panel()->presets[0].intent; /* a copy: equal by value, not pointer */
    cfg.intent = &def;
    run_converter(&cfg, img, b, NULL);
    TEST_ASSERT_EQUAL_MEMORY(a, b, (size_t)w * h);
    cfg.intent = panel()->presets[1].intent; /* Soft differs */
    run_converter(&cfg, img, b, NULL);
    TEST_ASSERT_TRUE(memcmp(a, b, (size_t)w * h) != 0);
    free(b);
    free(a);
    free(img);
}

/* Each packed format and preview of the codes against the same input, at odd
 * widths. */
void test_converter_formats(void) {
    const epi_panel_t* p = panel();
    /* The lightest EL133UF1 ink is white (index 1, code 1). */
    const uint8_t pad_el = p->inks[1].code;
    TEST_ASSERT_EQUAL_UINT8(1, pad_el);

    /* A copy of the panel with 1-bit codes: inks 0, 1, 2.. → 0, 1, 0, 1.. */
    epi_ink_t inks1[EPI_MAX_INKS];
    memcpy(inks1, p->inks, p->ink_count * sizeof inks1[0]);
    for (size_t i = 0; i < p->ink_count; i++) {
        inks1[i].code = (uint8_t)(i & 1);
    }
    epi_panel_t p1 = *p;
    p1.inks = inks1;
    /* And 2-bit codes: 3, 2, 1, 0, 3, 2 (lightest ink white → pad 2). */
    epi_ink_t inks2[EPI_MAX_INKS];
    memcpy(inks2, p->inks, p->ink_count * sizeof inks2[0]);
    for (size_t i = 0; i < p->ink_count; i++) {
        inks2[i].code = (uint8_t)(3 - (i & 3));
    }
    epi_panel_t p2 = *p;
    p2.inks = inks2;

    const int widths[] = {1, 2, 3, 5, 7, 8, 9, 13, 16, 17};
    const int h = 23;
    for (size_t wi = 0; wi < sizeof widths / sizeof widths[0]; wi++) {
        const int w = widths[wi];
        epi_rgb_t* img = xmalloc((size_t)w * h * sizeof *img);
        fill_noise(img, (size_t)w * h, (uint32_t)(w * 7 + 1));
        uint8_t* codes = xmalloc((size_t)w * h);
        uint8_t* out = xmalloc((size_t)w * h * 3);
        uint8_t* panel_codes = xmalloc((size_t)w * h);
        epi_config_t cfg = config(panel(), (uint16_t)w, (uint16_t)h);
        run_converter(&cfg, img, codes, NULL);
        uint8_t idx[32 * 23];
        for (int i = 0; i < w * h; i++) {
            int k = code_to_index(panel(), codes[i]);
            TEST_ASSERT_TRUE(k >= 0);
            idx[i] = (uint8_t)k;
        }

        struct {
            epi_pack_format_t format;
            const epi_panel_t* panel;
            unsigned bits;
            uint8_t pad;
        } packs[] = {
            {EPI_PACK_4BPP, p, 4, pad_el},
            {EPI_PACK_2BPP, &p2, 2, inks2[1].code},
            {EPI_PACK_1BPP, &p1, 1, inks1[1].code},
        };
        for (size_t k = 0; k < 3; k++) {
            cfg = config(packs[k].panel, (uint16_t)w, (uint16_t)h);
            const size_t rs = epi_pack_row_size(packs[k].format, (uint16_t)w);
            run_converter(&cfg, img, panel_codes, NULL);
            const unsigned per = 8 / packs[k].bits;
            for (int y = 0; y < h; y++) {
                TEST_ASSERT_EQUAL_INT(
                    EPI_OK, epi_pack_row(packs[k].panel, packs[k].format, panel_codes + (size_t)y * w, (uint16_t)w, out)
                );
                for (size_t b = 0; b < rs; b++) {
                    unsigned v = 0;
                    for (unsigned j = 0; j < per; j++) {
                        size_t x = b * per + j;
                        unsigned code = x < (size_t)w ? packs[k].panel->inks[idx[y * w + x]].code : packs[k].pad;
                        v = (v << packs[k].bits) | code;
                    }
                    TEST_ASSERT_EQUAL_HEX8(v, out[b]);
                }
            }
        }

        for (int pv = 0; pv < 2; pv++) {
            preview_same_size(p, pv != 0, codes, w, h, (epi_rgb_t*)(void*)out);
            for (int i = 0; i < w * h; i++) {
                epi_rgb_t want = pv ? p->inks[idx[i]].nominal : p->inks[idx[i]].measured;
                epi_rgb_t got = {out[3 * i], out[3 * i + 1], out[3 * i + 2]};
                TEST_ASSERT_EQUAL_RGB(want, got);
            }
        }
        free(panel_codes);
        free(out);
        free(codes);
        free(img);
    }
}

/* Init on dirty memory, and re-init of a used workspace, give identical output. */
void test_converter_reinit_dirty(void) {
    const int w = 29, h = 45;
    epi_rgb_t* img = xmalloc((size_t)w * h * sizeof *img);
    fill_noise(img, (size_t)w * h, 4242);
    epi_config_t cfg = config(panel(), (uint16_t)w, (uint16_t)h);
    size_t size = epi_converter_workspace_size(&cfg);
    uint8_t* ws = xmalloc(size);
    uint8_t* a = xmalloc((size_t)w * h);
    uint8_t* b = xmalloc((size_t)w * h);
    check_dirty_workspace(&cfg, img, ws, size, a, b);
    /* Re-init without clearing, mid-image and after a full run. */
    epi_converter_t* c = NULL;
    TEST_ASSERT_EQUAL_INT(EPI_OK, epi_converter_init(&cfg, ws, size, &c));
    for (int y = 0; y < 30; y++) {
        epi_converter_push(c, img + (size_t)y * w);
        while (epi_converter_pop(c, b) == EPI_OK) {
        }
    }
    run_converter_in(&cfg, img, ws, size, b, NULL);
    TEST_ASSERT_EQUAL_MEMORY(a, b, (size_t)w * h);
    run_converter_in(&cfg, img, ws, size, b, NULL);
    TEST_ASSERT_EQUAL_MEMORY(a, b, (size_t)w * h);
    free(b);
    free(a);
    free(ws);
    free(img);
}

/* Pushing without draining: the converter holds r + 1 undrained rows; the
 * next push asserts (debug) or is rejected with EPI_ERR_NOT_DRAINED without
 * touching memory (release). */
static void violate_drain(epi_converter_t* c, const epi_rgb_t* img, int w, int pushes) {
    for (int y = 0; y < pushes; y++) {
        epi_converter_push(c, img + (size_t)y * w);
    }
}

void test_converter_drain_violation(void) {
    const int w = 11, h = 80, r = radius();
    epi_rgb_t* img = xmalloc((size_t)w * h * sizeof *img);
    fill_noise(img, (size_t)w * h, 777);
    epi_config_t cfg = config(panel(), (uint16_t)w, (uint16_t)h);
    size_t size = epi_converter_workspace_size(&cfg);
    const size_t guard = 256;
    uint8_t* buf = xmalloc(size + 2 * guard);
    uint8_t* ws = buf + guard;
    epi_converter_t* c = NULL;

    /* r + 1 undrained rows are within the contract's capacity. */
    memset(buf, 0xc3, size + 2 * guard);
    TEST_ASSERT_EQUAL_INT(EPI_OK, epi_converter_init(&cfg, ws, size, &c));
    violate_drain(c, img, w, r + 1);
    uint8_t row[11];
    TEST_ASSERT_EQUAL_INT(EPI_OK, epi_converter_pop(c, row));
    TEST_ASSERT_EQUAL_INT(EPI_ERR_NOT_READY, epi_converter_pop(c, row));

#if defined(NDEBUG)
    /* Release: the extra pushes are rejected with EPI_ERR_NOT_DRAINED and nothing
     * outside the workspace is written. */
    TEST_ASSERT_EQUAL_INT(EPI_OK, epi_converter_init(&cfg, ws, size, &c));
    violate_drain(c, img, w, h);
    TEST_ASSERT_EQUAL_INT(EPI_ERR_NOT_DRAINED, epi_converter_push(c, img)); /* rejected: still undrained */
    for (size_t i = 0; i < guard; i++) {
        TEST_ASSERT_EQUAL_UINT8(0xc3, buf[i]);
        TEST_ASSERT_EQUAL_UINT8(0xc3, buf[guard + size + i]);
    }
    /* Exactly the first r + 1 rows were accepted: the first output row
     * matches a normal run, and no other row is ready. */
    uint8_t* ref = xmalloc((size_t)w * h);
    TEST_ASSERT_EQUAL_INT(EPI_OK, epi_converter_pop(c, row));
    TEST_ASSERT_EQUAL_INT(EPI_ERR_NOT_READY, epi_converter_pop(c, row + 0));
    run_converter(&cfg, img, ref, NULL);
    TEST_ASSERT_EQUAL_INT(EPI_OK, epi_converter_init(&cfg, ws, size, &c));
    violate_drain(c, img, w, h);
    TEST_ASSERT_EQUAL_INT(EPI_OK, epi_converter_pop(c, row));
    TEST_ASSERT_EQUAL_MEMORY(ref, row, (size_t)w);
    free(ref);
#elif defined(HAVE_DEATH_TEST)
    /* Debug: the extra push asserts. Run it in a child process. */
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    TEST_ASSERT_TRUE(pid >= 0);
    if (pid == 0) {
        if (freopen("/dev/null", "w", stderr) == NULL) {
            _exit(3);
        }
        signal(SIGABRT, SIG_DFL);
        if (epi_converter_init(&cfg, ws, size, &c) != EPI_OK) { /* no Unity asserts in the child */
            _exit(3);
        }
        violate_drain(c, img, w, r + 2);
        _exit(0); /* no assert: failure */
    }
    int status = 0;
    TEST_ASSERT_EQUAL_INT(pid, waitpid(pid, &status, 0));
    TEST_ASSERT_TRUE_MESSAGE(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT, "push past the rings did not assert");
#else
    TEST_IGNORE_MESSAGE("debug build without fork: the contract assert is not testable here");
#endif
    free(buf);
    free(img);
}

/*
 * Regression goldens: FNV-1a 64 of the ink
 * codes for each input × preset, frozen from the verified implementation
 * (streaming == whole image, and the end-to-end checks). Update only on an
 * intentional change to the output: run the test, and copy the "got" values
 * it prints on failure into this table (one row per input, presets in the
 * panel's order: Default, Soft, Deep, Vivid).
 */
#define GOLDEN_W 157
#define GOLDEN_H 119
static const uint64_t goldens[4][4] = {
    /* gradient */
    {UINT64_C(0x341168798e0be9a4),
     UINT64_C(0x8d562bd71a500f8f),
     UINT64_C(0x5617dc72e6528cef),
     UINT64_C(0xbcf3e29fe1c1f4bc)},
    /* hue ramp */
    {UINT64_C(0x3eee4082e42ab796),
     UINT64_C(0xcf1fa89700f9cf83),
     UINT64_C(0xd27727c31c7f5e40),
     UINT64_C(0xc8caaa06fabd37ce)},
    /* noise */
    {UINT64_C(0x90d8e29535660eb3),
     UINT64_C(0xbd3671fe036900ae),
     UINT64_C(0x0f470c3ecb523c51),
     UINT64_C(0xa183ba3f93c94efe)},
    /* page (neutral, so Vivid = Default) */
    {UINT64_C(0x9cb2c9a745621a27),
     UINT64_C(0x4158ddccd2aa139c),
     UINT64_C(0xa564c04e88085f3a),
     UINT64_C(0x9cb2c9a745621a27)},
};

/* The same with the panel's neutral cast cleared (has_neutral 0): the output of a
 * panel without one, which the stage must leave bit for bit. */
static const uint64_t goldens_without_neutral[4][4] = {
    /* gradient */
    {UINT64_C(0x1e1ee32fa0c19a3b),
     UINT64_C(0x7e3e724b8020f34e),
     UINT64_C(0x00f153a7ac7c1952),
     UINT64_C(0xd173cdf52bd6a2d8)},
    /* hue ramp */
    {UINT64_C(0xeb0ce46a46d233c0),
     UINT64_C(0xabef27f76593e7df),
     UINT64_C(0xbd9cfc7c45f422e5),
     UINT64_C(0x70219770a57456e1)},
    /* noise */
    {UINT64_C(0x3933c1b51b2ae1ad),
     UINT64_C(0x3f081fc88f92034f),
     UINT64_C(0xf88449c5d3f511ab),
     UINT64_C(0x202c6f391544084d)},
    /* page */
    {UINT64_C(0xcae2b79a1fd803c9),
     UINT64_C(0xb1dba564c2b19f18),
     UINT64_C(0xf923f6b29d1bc6a8),
     UINT64_C(0xcae2b79a1fd803c9)},
};

static void check_goldens(const epi_panel_t* pn, const uint64_t want[4][4]) {
    static const char* names[4] = {"gradient", "hue ramp", "noise", "page"};
    const size_t n = (size_t)GOLDEN_W * GOLDEN_H;
    epi_rgb_t* img = xmalloc(n * sizeof *img);
    uint8_t* out = xmalloc(n);
    TEST_ASSERT_EQUAL_size_t(4, pn->preset_count);
    char report[1024];
    size_t len = 0;
    int bad = 0;
    for (int i = 0; i < 4; i++) {
        switch (i) {
        case 0:
            fill_gradient(img, GOLDEN_W, GOLDEN_H);
            break;
        case 1:
            fill_hue_ramp(img, GOLDEN_W, GOLDEN_H);
            break;
        case 2:
            fill_noise(img, n, 0x9e3779b9u);
            break;
        default:
            fill_page(img, GOLDEN_W, GOLDEN_H);
            break;
        }
        len += (size_t)snprintf(report + len, sizeof report - len, "\n    /* %s */ {", names[i]);
        for (int p = 0; p < 4; p++) {
            epi_config_t cfg = config(pn, GOLDEN_W, GOLDEN_H);
            cfg.intent = pn->presets[p].intent;
            TEST_ASSERT_EQUAL_INT(GOLDEN_H, run_converter(&cfg, img, out, NULL));
            uint64_t hsh = fnv1a64(out, n);
            bad |= hsh != want[i][p];
            /* Through the packer, the same codes reach the panel. */
            TEST_ASSERT_TRUE(packed_codes_hash(pn, EPI_PACK_4BPP, 4, out, GOLDEN_W, GOLDEN_H) == hsh);
            len += (size_t)snprintf(
                report + len, sizeof report - len, "UINT64_C(0x%016llx)%s", (unsigned long long)hsh, p < 3 ? ", " : "},"
            );
        }
    }
    free(out);
    free(img);
    if (bad) {
        printf("golden hashes, got:%s\n", report);
        TEST_FAIL_MESSAGE("converter output changed (see the golden hashes printed above)");
    }
}

void test_converter_goldens(void) {
    check_goldens(panel(), goldens);
}

void test_converter_goldens_without_neutral(void) {
    epi_colour_tables_t off = *epi_colour_tables(panel()->tables);
    TEST_ASSERT_EQUAL_UINT8(1, off.has_neutral);
    off.has_neutral = 0;
    epi_panel_t p = *panel();
    p.tables = &off.base;
    check_goldens(&p, goldens_without_neutral);
}
