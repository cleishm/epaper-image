/*
 * Tests for the preview reducer and the row packer (src/epi_preview.c):
 * agreement with the converter's outputs, hand-computed reductions, rounding and
 * tie rules, validation and the push/pop protocol.
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L /* fork, waitpid, _exit for the death test */
#endif

#include "epaper_image.h"
#include "epi_srgb.h"
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

#define WHITE 9
#define BLACK 3
#define CYAN 200
#define ROSE 77

/* Only inks matter here, so the panel has no tables. The codes are sparse and
 * unordered, and each channel takes its own path. */
static const epi_ink_t HAND_INKS[] = {
    {WHITE, {250, 251, 252}, {255, 255, 255}},
    {BLACK, {1, 2, 3}, {0, 0, 0}},
    {CYAN, {5, 6, 7}, {0, 128, 255}},
    {ROSE, {8, 9, 10}, {200, 16, 64}},
};
static const epi_panel_t HAND_PANEL = {.name = "hand", .inks = HAND_INKS, .ink_count = 4};

static const epi_panel_t* const BUILT_IN[] = {
    &epi_panel_el133uf1,
    &epi_panel_gdey075t7_mono,
    &epi_panel_gdey075t7_grey4,
};
#define BUILT_IN_COUNT (sizeof BUILT_IN / sizeof BUILT_IN[0])

static void make_cfg(
    epi_preview_config_t* c,
    const epi_panel_t* p,
    uint16_t sw,
    uint16_t sh,
    uint16_t dw,
    uint16_t dh,
    epi_output_t o
) {
    memset(c, 0, sizeof *c);
    c->panel = p;
    c->src_width = sw;
    c->src_height = sh;
    c->dst_width = dw;
    c->dst_height = dh;
    c->output = o;
}

/* Streams codes through a reducer in ws, popping until not OK after every push,
 * and checks that one push finishes at most one row and that every row came out. */
static void reduce_in(const epi_preview_config_t* cfg, const uint8_t* codes, void* ws, size_t size, epi_rgb_t* out) {
    epi_preview_t* p = NULL;
    TEST_ASSERT_EQUAL_INT(EPI_OK, epi_preview_init(cfg, ws, size, &p));
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_size_t(0, (uintptr_t)p % _Alignof(uint64_t)); /* wherever ws is */
    int n = 0;
    for (int y = 0; y < cfg->src_height; y++) {
        TEST_ASSERT_EQUAL_INT(EPI_OK, epi_preview_push(p, codes + (size_t)y * cfg->src_width));
        int popped = 0;
        epi_err_t e;
        while ((e = epi_preview_pop(p, out + (size_t)n * cfg->dst_width)) == EPI_OK) {
            n++;
            popped++;
        }
        TEST_ASSERT_TRUE(popped <= 1);
        TEST_ASSERT_EQUAL_INT(n == cfg->dst_height ? EPI_ERR_FINISHED : EPI_ERR_NOT_READY, e);
    }
    TEST_ASSERT_EQUAL_INT(cfg->dst_height, n);
    TEST_ASSERT_EQUAL_INT(EPI_ERR_FINISHED, epi_preview_pop(p, out));
    TEST_ASSERT_EQUAL_INT(EPI_ERR_FINISHED, epi_preview_push(p, codes));
}

static void reduce(const epi_preview_config_t* cfg, const uint8_t* codes, epi_rgb_t* out) {
    const size_t size = epi_preview_workspace_size(cfg);
    TEST_ASSERT_NOT_EQUAL(0, size);
    void* ws = xmalloc(size);
    reduce_in(cfg, codes, ws, size, out);
    free(ws);
}

/* Reduces on the hand panel and compares with the expected pixels. */
static void check_hand(
    const uint8_t* codes,
    uint16_t sw,
    uint16_t sh,
    uint16_t dw,
    uint16_t dh,
    const uint8_t* want /* dw*dh*3 */
) {
    epi_preview_config_t c;
    epi_rgb_t got[16];
    TEST_ASSERT_TRUE((size_t)dw * dh <= sizeof got / sizeof got[0]);
    make_cfg(&c, &HAND_PANEL, sw, sh, dw, dh, EPI_OUT_PREVIEW_MEASURED);
    reduce(&c, codes, got);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, (const uint8_t*)got, (size_t)dw * dh * 3);
}

/* A grey panel with free codes: lightest last, but not the largest code. */
typedef union {
    long long ll;
    long double ld;
    void* p;
    unsigned char bytes[8192];
} storage_t;
static storage_t grey_storage;

static const epi_panel_t* free_code_grey(void) {
    static const epi_ink_t levels[] = {
        {3, {0, 0, 0}, {40, 40, 40}},
        {0, {90, 90, 90}, {90, 90, 90}},
        {2, {170, 170, 170}, {150, 150, 150}},
        {1, {255, 255, 255}, {220, 220, 220}},
    };
    epi_grey_panel_config_t cfg = {"grey", 64, 48, levels, 4, 1.0f};
    size_t size = epi_grey_panel_size(&cfg);
    TEST_ASSERT_TRUE(size != 0 && size <= sizeof grey_storage.bytes);
    const epi_panel_t* p = NULL;
    TEST_ASSERT_EQUAL_INT(EPI_OK, epi_grey_panel_init(&cfg, grey_storage.bytes, size, &p));
    return p;
}

/* ---- packer ---------------------------------------------------------------- */

static void check_pack_panel(const epi_panel_t* p) {
    static const epi_output_t formats[] = {EPI_OUT_PACKED4, EPI_OUT_PACKED2, EPI_OUT_PACKED1};
    static const uint16_t widths[] = {1, 2, 3, 4, 5, 7, 8, 9, 13, 16, 33};
    const uint16_t h = 7;
    for (size_t f = 0; f < sizeof formats / sizeof formats[0]; f++) {
        for (size_t wi = 0; wi < sizeof widths / sizeof widths[0]; wi++) {
            const uint16_t w = widths[wi];
            const size_t rs = epi_output_row_size(formats[f], w);
            uint8_t* got = xmalloc(rs);
            epi_config_t packed_cfg = config(p, w, h, formats[f]);
            epi_config_t codes_cfg = config(p, w, h, EPI_OUT_CODES8);
            epi_rgb_t* img = xmalloc((size_t)w * h * sizeof *img);
            uint8_t* codes = xmalloc((size_t)w * h);
            fill_noise(img, (size_t)w * h, 4242u + w);
            TEST_ASSERT_EQUAL_INT(h, run_converter(&codes_cfg, img, codes, NULL));
            if (epi_converter_workspace_size(&packed_cfg) == 0) {
                /* The panel's codes don't fit: rejected like the converter, even for
                 * a row whose codes would. */
                memset(codes, 0, w);
                TEST_ASSERT_EQUAL_INT(EPI_ERR_INVALID_ARG, epi_pack_row(p, formats[f], codes, w, got));
            } else {
                uint8_t* want = xmalloc(rs * h);
                TEST_ASSERT_EQUAL_INT(h, run_converter(&packed_cfg, img, want, NULL));
                for (uint16_t y = 0; y < h; y++) {
                    TEST_ASSERT_EQUAL_INT(EPI_OK, epi_pack_row(p, formats[f], codes + (size_t)y * w, w, got));
                    TEST_ASSERT_EQUAL_MEMORY(want + (size_t)y * rs, got, rs);
                }
                free(want);
            }
            free(codes);
            free(img);
            free(got);
        }
    }
}

void test_preview_pack_matches_converter(void) {
    for (size_t i = 0; i < BUILT_IN_COUNT; i++) {
        check_pack_panel(BUILT_IN[i]);
    }
    check_pack_panel(free_code_grey());
}

void test_preview_pack_bytes(void) {
    uint8_t out[2];
    /* EL133UF1's white is code 1, not its last ink or largest code. */
    const uint8_t colour[] = {1, 2, 3};
    TEST_ASSERT_EQUAL_INT(EPI_OK, epi_pack_row(&epi_panel_el133uf1, EPI_OUT_PACKED4, colour, 3, out));
    TEST_ASSERT_EQUAL_UINT8(0x12, out[0]);
    TEST_ASSERT_EQUAL_UINT8(0x31, out[1]);

    const uint8_t grey[] = {0, 1, 2, 3, 2};
    TEST_ASSERT_EQUAL_INT(EPI_OK, epi_pack_row(&epi_panel_gdey075t7_grey4, EPI_OUT_PACKED2, grey, 5, out));
    TEST_ASSERT_EQUAL_UINT8(0x1b, out[0]);
    TEST_ASSERT_EQUAL_UINT8(0xbf, out[1]); /* 2, then three of 3 */

    const uint8_t mono[] = {1, 0, 0};
    TEST_ASSERT_EQUAL_INT(EPI_OK, epi_pack_row(&epi_panel_gdey075t7_mono, EPI_OUT_PACKED1, mono, 3, out));
    TEST_ASSERT_EQUAL_UINT8(0x9f, out[0]); /* 100, then five of 1 */

    /* Lightest last with code 1, so the pad is not the top code. */
    const uint8_t freeform[] = {3};
    TEST_ASSERT_EQUAL_INT(EPI_OK, epi_pack_row(free_code_grey(), EPI_OUT_PACKED2, freeform, 1, out));
    TEST_ASSERT_EQUAL_UINT8(0xd5, out[0]);

    /* Equally light inks: the pad is the first of them. */
    static const epi_ink_t tied[] = {
        {0, {0, 0, 0}, {0, 0, 0}}, {2, {0, 0, 0}, {99, 99, 99}}, {1, {0, 0, 0}, {99, 99, 99}}
    };
    const epi_panel_t tied_panel = {.name = "tied", .inks = tied, .ink_count = 3};
    const uint8_t zero[] = {0};
    TEST_ASSERT_EQUAL_INT(EPI_OK, epi_pack_row(&tied_panel, EPI_OUT_PACKED4, zero, 1, out));
    TEST_ASSERT_EQUAL_UINT8(0x02, out[0]);
}

void test_preview_pack_validation(void) {
    const epi_panel_t* mono = &epi_panel_gdey075t7_mono;
    const uint8_t codes[] = {0, 1, 0, 1};
    uint8_t out[4];
    memset(out, 0x5a, sizeof out);

    TEST_ASSERT_EQUAL_INT(EPI_OK, epi_pack_row(mono, EPI_OUT_PACKED1, codes, 4, out));
    memset(out, 0x5a, sizeof out);

    TEST_ASSERT_EQUAL_INT(EPI_ERR_INVALID_ARG, epi_pack_row(NULL, EPI_OUT_PACKED1, codes, 4, out));
    TEST_ASSERT_EQUAL_INT(EPI_ERR_INVALID_ARG, epi_pack_row(mono, EPI_OUT_PACKED1, NULL, 4, out));
    TEST_ASSERT_EQUAL_INT(EPI_ERR_INVALID_ARG, epi_pack_row(mono, EPI_OUT_PACKED1, codes, 4, NULL));
    TEST_ASSERT_EQUAL_INT(EPI_ERR_INVALID_ARG, epi_pack_row(mono, EPI_OUT_PACKED1, codes, 0, out));

    const epi_panel_t no_inks = {.name = "none", .inks = NULL, .ink_count = 0};
    const epi_panel_t zero_inks = {.name = "none", .inks = HAND_INKS, .ink_count = 0};
    const epi_panel_t null_inks = {.name = "none", .inks = NULL, .ink_count = 4};
    TEST_ASSERT_EQUAL_INT(EPI_ERR_INVALID_ARG, epi_pack_row(&no_inks, EPI_OUT_PACKED1, codes, 4, out));
    TEST_ASSERT_EQUAL_INT(EPI_ERR_INVALID_ARG, epi_pack_row(&zero_inks, EPI_OUT_PACKED1, codes, 4, out));
    TEST_ASSERT_EQUAL_INT(EPI_ERR_INVALID_ARG, epi_pack_row(&null_inks, EPI_OUT_PACKED1, codes, 4, out));

    static const epi_output_t not_packed[] = {
        EPI_OUT_CODES8, EPI_OUT_PREVIEW_MEASURED, EPI_OUT_PREVIEW_NOMINAL, (epi_output_t)99
    };
    for (size_t i = 0; i < sizeof not_packed / sizeof not_packed[0]; i++) {
        TEST_ASSERT_EQUAL_INT(EPI_ERR_INVALID_ARG, epi_pack_row(mono, not_packed[i], codes, 4, out));
    }

    /* A code too wide for the format, in any position, and in the panel's inks. */
    const uint8_t wide_first[] = {2, 1, 0, 1}, wide_last[] = {0, 1, 0, 2};
    TEST_ASSERT_EQUAL_INT(EPI_ERR_INVALID_ARG, epi_pack_row(mono, EPI_OUT_PACKED1, wide_first, 4, out));
    TEST_ASSERT_EQUAL_INT(EPI_ERR_INVALID_ARG, epi_pack_row(mono, EPI_OUT_PACKED1, wide_last, 4, out));
    const uint8_t wide4[] = {16};
    TEST_ASSERT_EQUAL_INT(EPI_ERR_INVALID_ARG, epi_pack_row(mono, EPI_OUT_PACKED4, wide4, 1, out));
    const uint8_t wide2[] = {4};
    TEST_ASSERT_EQUAL_INT(EPI_ERR_INVALID_ARG, epi_pack_row(mono, EPI_OUT_PACKED2, wide2, 1, out));
    TEST_ASSERT_EQUAL_INT(EPI_ERR_INVALID_ARG, epi_pack_row(&epi_panel_el133uf1, EPI_OUT_PACKED2, codes, 4, out));
    TEST_ASSERT_EQUAL_INT(
        EPI_ERR_INVALID_ARG, epi_pack_row(&epi_panel_gdey075t7_grey4, EPI_OUT_PACKED1, codes, 4, out)
    );

    /* Nothing was written. */
    for (size_t i = 0; i < sizeof out; i++) {
        TEST_ASSERT_EQUAL_UINT8(0x5a, out[i]);
    }
}

/* ---- reducer: agreement with the converter --------------------------------- */

static void check_identity(const epi_panel_t* p, const epi_rgb_t* img, uint16_t w, uint16_t h) {
    static const epi_output_t previews[] = {EPI_OUT_PREVIEW_MEASURED, EPI_OUT_PREVIEW_NOMINAL};
    const size_t n = (size_t)w * h;
    uint8_t* codes = xmalloc(n);
    epi_rgb_t* want = xmalloc(n * sizeof *want);
    epi_rgb_t* got = xmalloc(n * sizeof *got);
    epi_config_t codes_cfg = config(p, w, h, EPI_OUT_CODES8);
    TEST_ASSERT_EQUAL_INT(h, run_converter(&codes_cfg, img, codes, NULL));
    for (size_t k = 0; k < 2; k++) {
        epi_config_t preview_cfg = config(p, w, h, previews[k]);
        TEST_ASSERT_EQUAL_INT(h, run_converter(&preview_cfg, img, (uint8_t*)want, NULL));
        epi_preview_config_t c;
        make_cfg(&c, p, w, h, w, h, previews[k]);
        reduce(&c, codes, got);
        TEST_ASSERT_EQUAL_MEMORY(want, got, n * sizeof *want);
    }
    free(got);
    free(want);
    free(codes);
}

void test_preview_identity_matches_converter(void) {
    const uint16_t w = 41, h = 29;
    epi_rgb_t* noise = xmalloc((size_t)w * h * sizeof *noise);
    epi_rgb_t* ramp = xmalloc((size_t)w * h * sizeof *ramp);
    fill_noise(noise, (size_t)w * h, 99);
    fill_hue_ramp(ramp, w, h);
    for (size_t i = 0; i < BUILT_IN_COUNT; i++) {
        check_identity(BUILT_IN[i], noise, w, h);
        check_identity(BUILT_IN[i], ramp, w, h);
    }
    check_identity(free_code_grey(), noise, w, h);
    free(ramp);
    free(noise);
}

/* ---- reducer: exact values ------------------------------------------------- */

static void check_single_ink(const epi_panel_t* p) {
    static const uint16_t sizes[][4] = {{7, 5, 3, 2}, {5, 3, 3, 2}, {16, 9, 9, 7}, {3, 3, 2, 1}, {4, 7, 1, 1}};
    for (size_t k = 0; k < p->ink_count; k++) {
        for (size_t s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
            const uint16_t sw = sizes[s][0], sh = sizes[s][1], dw = sizes[s][2], dh = sizes[s][3];
            uint8_t codes[16 * 9];
            epi_rgb_t got[9 * 7];
            memset(codes, p->inks[k].code, sizeof codes);
            epi_preview_config_t c;
            make_cfg(&c, p, sw, sh, dw, dh, EPI_OUT_PREVIEW_MEASURED);
            reduce(&c, codes, got);
            for (int i = 0; i < dw * dh; i++) {
                TEST_ASSERT_EQUAL_RGB(p->inks[k].measured, got[i]);
            }
            make_cfg(&c, p, sw, sh, dw, dh, EPI_OUT_PREVIEW_NOMINAL);
            reduce(&c, codes, got);
            for (int i = 0; i < dw * dh; i++) {
                TEST_ASSERT_EQUAL_RGB(p->inks[k].nominal, got[i]);
            }
        }
    }
}

void test_preview_single_ink(void) {
    for (size_t i = 0; i < BUILT_IN_COUNT; i++) {
        check_single_ink(BUILT_IN[i]);
    }
    check_single_ink(&HAND_PANEL);
}

/* The largest sides, on each axis: the edge positions reach 65535 × dst. */
void test_preview_largest_sides(void) {
    const uint16_t big = 65535;
    uint8_t* row = xmalloc(big);
    epi_rgb_t got[61];
    memset(row, CYAN, big);
    const epi_rgb_t cyan = {0, 128, 255};

    epi_preview_config_t c;
    make_cfg(&c, &HAND_PANEL, big, 1, 61, 1, EPI_OUT_PREVIEW_MEASURED);
    reduce(&c, row, got);
    for (int i = 0; i < 61; i++) {
        TEST_ASSERT_EQUAL_RGB(cyan, got[i]);
    }
    free(row);

    make_cfg(&c, &HAND_PANEL, 1, big, 1, 61, EPI_OUT_PREVIEW_MEASURED);
    size_t size = epi_preview_workspace_size(&c);
    void* ws = xmalloc(size);
    epi_preview_t* p = NULL;
    TEST_ASSERT_EQUAL_INT(EPI_OK, epi_preview_init(&c, ws, size, &p));
    const uint8_t code = CYAN;
    int n = 0;
    for (uint32_t y = 0; y < big; y++) {
        TEST_ASSERT_EQUAL_INT(EPI_OK, epi_preview_push(p, &code));
        while (epi_preview_pop(p, got) == EPI_OK) {
            TEST_ASSERT_EQUAL_RGB(cyan, got[0]);
            n++;
        }
    }
    TEST_ASSERT_EQUAL_INT(61, n);
    free(ws);
}

void test_preview_checkerboard(void) {
    static const epi_ink_t levels[] = {{0, {0, 0, 0}, {0, 0, 0}}, {1, {255, 255, 255}, {255, 255, 255}}};
    epi_grey_panel_config_t g = {"check", 16, 16, levels, 2, 1.0f};
    size_t gsize = epi_grey_panel_size(&g);
    TEST_ASSERT_TRUE(gsize != 0 && gsize <= sizeof grey_storage.bytes);
    const epi_panel_t* p = NULL;
    TEST_ASSERT_EQUAL_INT(EPI_OK, epi_grey_panel_init(&g, grey_storage.bytes, gsize, &p));

    enum { W = 8, H = 6 };
    uint8_t codes[W * H];
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            codes[y * W + x] = (uint8_t)((x ^ y) & 1);
        }
    }
    epi_rgb_t got[W / 2 * H / 2];
    const epi_rgb_t want = {188, 188, 188}; /* linear 32768; averaging sRGB values would give 128 */
    epi_preview_config_t c;
    make_cfg(&c, p, W, H, W / 2, H / 2, EPI_OUT_PREVIEW_MEASURED);
    reduce(&c, codes, got);
    for (int i = 0; i < W / 2 * H / 2; i++) {
        TEST_ASSERT_EQUAL_RGB(want, got[i]);
    }
    make_cfg(&c, p, W, H, W / 2, H / 2, EPI_OUT_PREVIEW_NOMINAL);
    reduce(&c, codes, got);
    for (int i = 0; i < W / 2 * H / 2; i++) {
        TEST_ASSERT_EQUAL_RGB(want, got[i]);
    }
}

/*
 * Expected values worked out separately, with exact fractions, interval overlaps
 * of whole unit cells and a linear scan of the sRGB table.
 *
 * Linear values (Q16): 255 -> 65536, 200 -> 37852, 128 -> 14147, 64 -> 3360,
 * 16 -> 340, 0 -> 0. In the table, 213 is the nearest to 43691, 106 to 9431,
 * and so on.
 */
void test_preview_hand_computed(void) {
    const uint8_t W = WHITE, K = BLACK, C = CYAN, R = ROSE;

    /* 3 -> 2. Weights a:2 b:1 | b:1 c:2, over 3:
     *   (2·65536 + 1·0) / 3      = 43690.67 -> 43691 -> 213, on each channel
     *   r 0 -> 0; g 2·14147/3 = 9431.33 -> 9431 -> 106; b 2·65536/3 -> 213 */
    const uint8_t a3[] = {W, K, C};
    const uint8_t a3want[] = {213, 213, 213, 0, 106, 213};
    check_hand(a3, 3, 1, 2, 1, a3want);
    check_hand(a3, 1, 3, 1, 2, a3want);

    /* 5 -> 3. Weights a:3 b:2 | b:1 c:3 d:1 | d:2 e:3, over 5:
     *   pixel 0: 3·65536/5 = 39321.6 -> 39322 -> 203
     *   pixel 1: r (0 + 0 + 65536)/5 = 13107.2 -> 124
     *            g (0 + 3·14147 + 65536)/5 = 21595.4 -> 155
     *            b (0 + 3·65536 + 65536)/5 = 52428.8 -> 231
     *   pixel 2: r (2·65536 + 3·37852)/5 = 48925.6 -> 224
     *            g (2·65536 + 3·340)/5   = 26418.4 -> 170
     *            b (2·65536 + 3·3360)/5  = 28230.4 -> 175 */
    const uint8_t a5[] = {W, K, C, W, R};
    const uint8_t a5want[] = {203, 203, 203, 124, 155, 231, 224, 170, 175};
    check_hand(a5, 5, 1, 3, 1, a5want);
    check_hand(a5, 1, 5, 1, 3, a5want);

    /* 5x3 -> 3x2: x weights as above, y weights 2:1 | 1:2, over 15. */
    const uint8_t b[] = {W, K, C, R, W, K, W, R, C, K, C, C, W, K, R};
    const uint8_t bwant[] = {193, 193, 193, 139, 114, 196, 196, 175, 195, 102, 144, 231, 201, 189, 215, 132, 49, 109};
    check_hand(b, 5, 3, 3, 2, bwant);

    /* 7x5 -> 3x4, over 35: both axes split pixels, unevenly. */
    const uint8_t d[] = {
        W, K, C, R, W, K, C, K, W, R, C, K, R, W, C, C, W, K, R, W, K, R, K, K, W, C, C, R, W, W, R, R, K, K, C,
    };
    const uint8_t dwant[] = {
        178, 179, 195, 181, 149, 198, 137, 143, 195, 162, 167, 212, 144, 115, 167, 205, 175, 178,
        125, 102, 172, 177, 171, 195, 160, 136, 193, 231, 216, 217, 173, 89,  115, 63,  89,  181,
    };
    check_hand(d, 7, 5, 3, 4, dwant);
}

/* A direct evaluation of the definition, for a spread of ratios: every source
 * pixel against every preview pixel, and a linear scan of the table. */
static uint32_t overlap(uint32_t i, uint32_t dst, uint32_t j, uint32_t src) {
    const uint32_t lo = i * dst > j * src ? i * dst : j * src;
    const uint32_t hi = (i + 1) * dst < (j + 1) * src ? (i + 1) * dst : (j + 1) * src;
    return hi > lo ? hi - lo : 0;
}

static uint8_t scan_nearest(uint32_t lin) {
    unsigned best = 0;
    for (unsigned u = 1; u < 256; u++) {
        const uint32_t d = epi_srgb_to_lin_q16[u] > lin ? epi_srgb_to_lin_q16[u] - lin : lin - epi_srgb_to_lin_q16[u];
        const uint32_t b =
            epi_srgb_to_lin_q16[best] > lin ? epi_srgb_to_lin_q16[best] - lin : lin - epi_srgb_to_lin_q16[best];
        if (d < b) {
            best = u;
        }
    }
    return (uint8_t)best;
}

void test_preview_reference(void) {
    static const uint16_t sizes[][4] = {
        {16, 9, 9, 7},
        {23, 17, 7, 16},
        {50, 40, 37, 3},
        {64, 3, 1, 3},
        {3, 64, 3, 1},
        {31, 31, 30, 30},
        {12, 12, 4, 6},
        {37, 11, 37, 11},
    };
    uint32_t seed = 2024;
    for (size_t s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
        const uint32_t sw = sizes[s][0], sh = sizes[s][1], dw = sizes[s][2], dh = sizes[s][3];
        uint8_t* codes = xmalloc((size_t)sw * sh);
        epi_rgb_t* got = xmalloc((size_t)dw * dh * sizeof *got);
        for (size_t i = 0; i < (size_t)sw * sh; i++) {
            codes[i] = HAND_INKS[xorshift32(&seed) % HAND_PANEL.ink_count].code;
        }
        epi_preview_config_t c;
        make_cfg(&c, &HAND_PANEL, (uint16_t)sw, (uint16_t)sh, (uint16_t)dw, (uint16_t)dh, EPI_OUT_PREVIEW_MEASURED);
        reduce(&c, codes, got);
        const uint64_t total = (uint64_t)sw * sh;
        for (uint32_t j = 0; j < dh; j++) {
            for (uint32_t i = 0; i < dw; i++) {
                uint64_t sum[3] = {0, 0, 0};
                for (uint32_t y = 0; y < sh; y++) {
                    for (uint32_t x = 0; x < sw; x++) {
                        const uint64_t w = (uint64_t)overlap(x, dw, i, sw) * overlap(y, dh, j, sh);
                        const uint8_t code = codes[(size_t)y * sw + x];
                        for (size_t k = 0; k < HAND_PANEL.ink_count; k++) {
                            if (HAND_INKS[k].code == code) {
                                sum[0] += w * epi_srgb_to_lin_q16[HAND_INKS[k].measured.r];
                                sum[1] += w * epi_srgb_to_lin_q16[HAND_INKS[k].measured.g];
                                sum[2] += w * epi_srgb_to_lin_q16[HAND_INKS[k].measured.b];
                            }
                        }
                    }
                }
                const epi_rgb_t want = {
                    scan_nearest((uint32_t)((sum[0] + total / 2) / total)),
                    scan_nearest((uint32_t)((sum[1] + total / 2) / total)),
                    scan_nearest((uint32_t)((sum[2] + total / 2) / total)),
                };
                TEST_ASSERT_EQUAL_RGB(want, got[(size_t)j * dw + i]);
            }
        }
        free(got);
        free(codes);
    }
}

/* One preview pixel from the codes, on a panel of grey inks at the given sRGB8
 * values (code i has value v[i]). */
static uint8_t mix(const uint8_t* v, size_t nv, const uint8_t* codes, uint16_t n) {
    epi_ink_t inks[4];
    TEST_ASSERT_TRUE(nv <= sizeof inks / sizeof inks[0]);
    for (size_t i = 0; i < nv; i++) {
        inks[i].code = (uint8_t)i;
        inks[i].measured = inks[i].nominal = (epi_rgb_t){v[i], v[i], v[i]};
    }
    const epi_panel_t p = {.name = "mix", .inks = inks, .ink_count = nv};
    epi_preview_config_t c;
    make_cfg(&c, &p, n, 1, 1, 1, EPI_OUT_PREVIEW_MEASURED);
    epi_rgb_t got;
    reduce(&c, codes, &got);
    TEST_ASSERT_EQUAL_UINT8(got.r, got.g);
    TEST_ASSERT_EQUAL_UINT8(got.r, got.b);
    return got.r;
}

void test_preview_rounding_and_ties(void) {
    const uint8_t half[] = {0, 1}, one_third[] = {0, 0, 1}, two_thirds[] = {0, 1, 1};

    /* A mean exactly midway between two table values takes the lower. */
    const uint8_t v01[] = {0, 1}; /* linear 0 and 20: mean 10 */
    TEST_ASSERT_EQUAL_UINT8(0, mix(v01, 2, half, 2));
    const uint8_t v253[] = {253, 254}; /* linear 64373 and 64953: mean 64663 */
    TEST_ASSERT_EQUAL_UINT8(253, mix(v253, 2, half, 2));

    /* Either side of the midpoint goes to the nearer value. */
    TEST_ASSERT_EQUAL_UINT8(0, mix(v01, 2, one_third, 3));  /* 20/3 = 6.67 -> 7 */
    TEST_ASSERT_EQUAL_UINT8(1, mix(v01, 2, two_thirds, 3)); /* 40/3 = 13.33 -> 13 */

    /* A mean of x.5 rounds up. Linear 64953 and 65536 have mean 65244.5: rounded up
     * to 65245 it is 291 from 255 and 292 from 254; truncated it would pick 254. */
    const uint8_t v254[] = {254, 255};
    TEST_ASSERT_EQUAL_UINT8(255, mix(v254, 2, half, 2));
}

/* ---- reducer: validation and protocol -------------------------------------- */

void test_preview_validation(void) {
    epi_preview_config_t ok;
    make_cfg(&ok, &HAND_PANEL, 8, 6, 3, 2, EPI_OUT_PREVIEW_MEASURED);
    const size_t size = epi_preview_workspace_size(&ok);
    TEST_ASSERT_NOT_EQUAL(0, size);
    uint8_t* ws = xmalloc(size);
    epi_preview_t* p = (epi_preview_t*)(void*)ws; /* a non-NULL sentinel */

    const epi_panel_t no_inks = {.name = "none", .inks = NULL, .ink_count = 0};
    const epi_panel_t zero_inks = {.name = "none", .inks = HAND_INKS, .ink_count = 0};
    const epi_panel_t null_inks = {.name = "none", .inks = NULL, .ink_count = 4};

    epi_preview_config_t bad[16];
    size_t n = 0;
    make_cfg(&bad[n++], NULL, 8, 6, 3, 2, EPI_OUT_PREVIEW_MEASURED);
    make_cfg(&bad[n++], &no_inks, 8, 6, 3, 2, EPI_OUT_PREVIEW_MEASURED);
    make_cfg(&bad[n++], &zero_inks, 8, 6, 3, 2, EPI_OUT_PREVIEW_MEASURED);
    make_cfg(&bad[n++], &null_inks, 8, 6, 3, 2, EPI_OUT_PREVIEW_MEASURED);
    make_cfg(&bad[n++], &HAND_PANEL, 0, 6, 3, 2, EPI_OUT_PREVIEW_MEASURED);
    make_cfg(&bad[n++], &HAND_PANEL, 8, 0, 3, 2, EPI_OUT_PREVIEW_MEASURED);
    make_cfg(&bad[n++], &HAND_PANEL, 8, 6, 0, 2, EPI_OUT_PREVIEW_MEASURED);
    make_cfg(&bad[n++], &HAND_PANEL, 8, 6, 3, 0, EPI_OUT_PREVIEW_MEASURED);
    make_cfg(&bad[n++], &HAND_PANEL, 8, 6, 9, 2, EPI_OUT_PREVIEW_MEASURED);
    make_cfg(&bad[n++], &HAND_PANEL, 8, 6, 3, 7, EPI_OUT_PREVIEW_MEASURED);
    make_cfg(&bad[n++], &HAND_PANEL, 8, 6, 3, 2, EPI_OUT_CODES8);
    make_cfg(&bad[n++], &HAND_PANEL, 8, 6, 3, 2, EPI_OUT_PACKED4);
    make_cfg(&bad[n++], &HAND_PANEL, 8, 6, 3, 2, EPI_OUT_PACKED2);
    make_cfg(&bad[n++], &HAND_PANEL, 8, 6, 3, 2, EPI_OUT_PACKED1);
    make_cfg(&bad[n++], &HAND_PANEL, 8, 6, 3, 2, (epi_output_t)99);
    TEST_ASSERT_TRUE(n <= sizeof bad / sizeof bad[0]);
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL_size_t(0, epi_preview_workspace_size(&bad[i]));
        p = (epi_preview_t*)(void*)ws;
        TEST_ASSERT_EQUAL_INT(EPI_ERR_INVALID_ARG, epi_preview_init(&bad[i], ws, size, &p));
        TEST_ASSERT_NULL(p);
    }

    TEST_ASSERT_EQUAL_size_t(0, epi_preview_workspace_size(NULL));
    p = (epi_preview_t*)(void*)ws;
    TEST_ASSERT_EQUAL_INT(EPI_ERR_INVALID_ARG, epi_preview_init(NULL, ws, size, &p));
    TEST_ASSERT_NULL(p);
    p = (epi_preview_t*)(void*)ws;
    TEST_ASSERT_EQUAL_INT(EPI_ERR_INVALID_ARG, epi_preview_init(&ok, NULL, size, &p));
    TEST_ASSERT_NULL(p);
    TEST_ASSERT_EQUAL_INT(EPI_ERR_INVALID_ARG, epi_preview_init(&ok, ws, size, NULL));

    /* The workspace size is exact. */
    p = (epi_preview_t*)(void*)ws;
    TEST_ASSERT_EQUAL_INT(EPI_ERR_WORKSPACE_TOO_SMALL, epi_preview_init(&ok, ws, size - 1, &p));
    TEST_ASSERT_NULL(p);
    TEST_ASSERT_EQUAL_INT(EPI_ERR_WORKSPACE_TOO_SMALL, epi_preview_init(&ok, ws, 0, &p));
    TEST_ASSERT_EQUAL_INT(EPI_OK, epi_preview_init(&ok, ws, size, &p));
    TEST_ASSERT_NOT_NULL(p);

    /* NULL arguments. */
    uint8_t codes[8];
    epi_rgb_t row[3];
    memset(codes, WHITE, sizeof codes);
    TEST_ASSERT_EQUAL_INT(EPI_ERR_INVALID_ARG, epi_preview_push(NULL, codes));
    TEST_ASSERT_EQUAL_INT(EPI_ERR_INVALID_ARG, epi_preview_push(p, NULL));
    TEST_ASSERT_EQUAL_INT(EPI_ERR_INVALID_ARG, epi_preview_pop(NULL, row));
    TEST_ASSERT_EQUAL_INT(EPI_ERR_INVALID_ARG, epi_preview_pop(p, NULL));

    /* The extremes of the accepted range. */
    epi_preview_config_t edge;
    make_cfg(&edge, &HAND_PANEL, 8, 6, 8, 6, EPI_OUT_PREVIEW_NOMINAL);
    TEST_ASSERT_NOT_EQUAL(0, epi_preview_workspace_size(&edge));
    make_cfg(&edge, &HAND_PANEL, 8, 6, 1, 1, EPI_OUT_PREVIEW_NOMINAL);
    TEST_ASSERT_NOT_EQUAL(0, epi_preview_workspace_size(&edge));
    make_cfg(&edge, &HAND_PANEL, 8, 6, 8, 1, EPI_OUT_PREVIEW_MEASURED); /* aspect ratio is free */
    TEST_ASSERT_NOT_EQUAL(0, epi_preview_workspace_size(&edge));
    make_cfg(&edge, &HAND_PANEL, 65535, 65535, 65535, 65535, EPI_OUT_PREVIEW_MEASURED);
    TEST_ASSERT_NOT_EQUAL(0, epi_preview_workspace_size(&edge));

    free(ws);
}

/* A bad row leaves the workspace byte-identical, wherever the bad code is and
 * wherever the row falls relative to the preview's rows. */
void test_preview_invalid_code(void) {
    enum { SW = 7, SH = 5, DW = 3, DH = 2 };
    uint8_t codes[SW * SH];
    uint32_t seed = 31;
    for (size_t i = 0; i < sizeof codes; i++) {
        codes[i] = HAND_INKS[xorshift32(&seed) % HAND_PANEL.ink_count].code;
    }
    epi_preview_config_t c;
    make_cfg(&c, &HAND_PANEL, SW, SH, DW, DH, EPI_OUT_PREVIEW_MEASURED);
    epi_rgb_t want[DW * DH], got[DW * DH];
    reduce(&c, codes, want);

    const size_t size = epi_preview_workspace_size(&c);
    uint8_t* ws = xmalloc(size);
    uint8_t* snapshot = xmalloc(size);
    memset(ws, 0, size);
    epi_preview_t* p = NULL;
    TEST_ASSERT_EQUAL_INT(EPI_OK, epi_preview_init(&c, ws, size, &p));
    int n = 0;
    for (int y = 0; y < SH; y++) {
        /* 0 and 255 are not this panel's codes; nor is 4, which falls between them. */
        const uint8_t bad_codes[] = {0, 255, 4};
        for (int pos = 0; pos < SW; pos++) {
            uint8_t bad[SW];
            memcpy(bad, codes + y * SW, SW);
            bad[pos] = bad_codes[(size_t)pos % sizeof bad_codes];
            memcpy(snapshot, ws, size);
            TEST_ASSERT_EQUAL_INT(EPI_ERR_INVALID_ARG, epi_preview_push(p, bad));
            TEST_ASSERT_EQUAL_MEMORY(snapshot, ws, size);
        }
        TEST_ASSERT_EQUAL_INT(EPI_OK, epi_preview_push(p, codes + y * SW));
        while (epi_preview_pop(p, got + (size_t)n * DW) == EPI_OK) {
            n++;
        }
    }
    TEST_ASSERT_EQUAL_INT(DH, n);
    TEST_ASSERT_EQUAL_MEMORY(want, got, sizeof want);
    free(snapshot);
    free(ws);
}

void test_preview_protocol(void) {
    /* 5 -> 2: the rows finish at the third and fifth pushes. */
    enum { W = 5, H = 5, DW = 2, DH = 2 };
    uint8_t codes[W];
    memset(codes, WHITE, sizeof codes);
    epi_preview_config_t c;
    make_cfg(&c, &HAND_PANEL, W, H, DW, DH, EPI_OUT_PREVIEW_MEASURED);
    const size_t size = epi_preview_workspace_size(&c);
    void* ws = xmalloc(size);
    epi_preview_t* p = NULL;
    TEST_ASSERT_EQUAL_INT(EPI_OK, epi_preview_init(&c, ws, size, &p));
    epi_rgb_t row[DW];
    static const int ready[H] = {0, 0, 1, 0, 1};
    TEST_ASSERT_EQUAL_INT(EPI_ERR_NOT_READY, epi_preview_pop(p, row)); /* before any push */
    int n = 0;
    for (int y = 0; y < H; y++) {
        TEST_ASSERT_EQUAL_INT(EPI_OK, epi_preview_push(p, codes));
        if (ready[y]) {
            TEST_ASSERT_EQUAL_INT(EPI_OK, epi_preview_pop(p, row));
            n++;
        }
        if (n < DH) {
            TEST_ASSERT_EQUAL_INT(EPI_ERR_NOT_READY, epi_preview_pop(p, row));
        }
    }
    TEST_ASSERT_EQUAL_INT(EPI_ERR_FINISHED, epi_preview_pop(p, row));
    TEST_ASSERT_EQUAL_INT(EPI_ERR_FINISHED, epi_preview_push(p, codes));
    TEST_ASSERT_EQUAL_INT(EPI_ERR_FINISHED, epi_preview_pop(p, row));

    /* Past the last row with the last preview row undrained: finished, without
     * asserting, and the row is still there to pop. */
    TEST_ASSERT_EQUAL_INT(EPI_OK, epi_preview_init(&c, ws, size, &p));
    for (int y = 0; y < H; y++) {
        TEST_ASSERT_EQUAL_INT(EPI_OK, epi_preview_push(p, codes));
        if (y < H - 1) {
            while (epi_preview_pop(p, row) == EPI_OK) {
            }
        }
    }
    TEST_ASSERT_EQUAL_INT(EPI_ERR_FINISHED, epi_preview_push(p, codes));
    TEST_ASSERT_EQUAL_INT(EPI_OK, epi_preview_pop(p, row));
    TEST_ASSERT_EQUAL_INT(EPI_ERR_FINISHED, epi_preview_pop(p, row));
    free(ws);
}

void test_preview_drain_violation(void) {
    /* Each push finishes a row. */
    enum { W = 3, H = 3 };
    const uint8_t a[W] = {WHITE, BLACK, CYAN}, b[W] = {ROSE, ROSE, BLACK};
    epi_preview_config_t c;
    make_cfg(&c, &HAND_PANEL, W, H, W, H, EPI_OUT_PREVIEW_MEASURED);
    const size_t size = epi_preview_workspace_size(&c);
    uint8_t* ws = xmalloc(size);
    epi_preview_t* p = NULL;
#if defined(NDEBUG)
    /* Release: rejected, nothing changes, and the first row is still good. */
    uint8_t* snapshot = xmalloc(size);
    memset(ws, 0, size);
    TEST_ASSERT_EQUAL_INT(EPI_OK, epi_preview_init(&c, ws, size, &p));
    TEST_ASSERT_EQUAL_INT(EPI_OK, epi_preview_push(p, a));
    memcpy(snapshot, ws, size);
    TEST_ASSERT_EQUAL_INT(EPI_ERR_NOT_DRAINED, epi_preview_push(p, b));
    TEST_ASSERT_EQUAL_MEMORY(snapshot, ws, size);
    epi_rgb_t row[W];
    const epi_rgb_t white = {255, 255, 255}, black = {0, 0, 0}, cyan = {0, 128, 255};
    TEST_ASSERT_EQUAL_INT(EPI_OK, epi_preview_pop(p, row));
    TEST_ASSERT_EQUAL_RGB(white, row[0]);
    TEST_ASSERT_EQUAL_RGB(black, row[1]);
    TEST_ASSERT_EQUAL_RGB(cyan, row[2]);
    /* Drained, the rejected row is accepted. */
    TEST_ASSERT_EQUAL_INT(EPI_OK, epi_preview_push(p, b));
    free(snapshot);
#elif defined(HAVE_DEATH_TEST)
    /* Debug: the push asserts. Run it in a child process. */
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    TEST_ASSERT_TRUE(pid >= 0);
    if (pid == 0) {
        if (freopen("/dev/null", "w", stderr) == NULL) {
            _exit(3);
        }
        signal(SIGABRT, SIG_DFL);
        if (epi_preview_init(&c, ws, size, &p) != EPI_OK) { /* no Unity asserts in the child */
            _exit(3);
        }
        epi_preview_push(p, a);
        epi_preview_push(p, b);
        _exit(0); /* no assert: failure */
    }
    int status = 0;
    TEST_ASSERT_EQUAL_INT(pid, waitpid(pid, &status, 0));
    TEST_ASSERT_TRUE_MESSAGE(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT, "undrained push did not assert");
#else
    (void)a;
    (void)b;
    (void)p;
    TEST_IGNORE_MESSAGE("debug build without fork: the contract assert is not testable here");
#endif
    free(ws);
}

/* Any start address and any prior contents give the same output, within the
 * reported size. The widths cross the blocks' rounding, so that the size's last
 * byte is in use at some offset. */
void test_preview_workspace_alignment(void) {
    enum { SW = 11, SH = 7, MAX_DW = 8, DH = 3 };
    uint8_t codes[SW * SH];
    uint32_t seed = 555;
    for (size_t i = 0; i < sizeof codes; i++) {
        codes[i] = HAND_INKS[xorshift32(&seed) % HAND_PANEL.ink_count].code;
    }
    for (uint16_t dw = 1; dw <= MAX_DW; dw++) {
        epi_preview_config_t c;
        make_cfg(&c, &HAND_PANEL, SW, SH, dw, DH, EPI_OUT_PREVIEW_MEASURED);
        epi_rgb_t want[MAX_DW * DH], got[MAX_DW * DH];
        reduce(&c, codes, want);

        const size_t size = epi_preview_workspace_size(&c);
        const size_t guard = 64;
        uint8_t* buf = xmalloc(guard + 32 + size + guard);
        static const uint8_t fills[] = {0xff, 0xa5};
        for (size_t offset = 0; offset < 32; offset++) {
            const uint8_t fill = fills[offset % sizeof fills];
            memset(buf, fill, guard + 32 + size + guard);
            reduce_in(&c, codes, buf + guard + offset, size, got);
            TEST_ASSERT_EQUAL_MEMORY(want, got, (size_t)dw * DH * sizeof want[0]);
            for (size_t i = 0; i < guard + offset; i++) {
                TEST_ASSERT_EQUAL_UINT8(fill, buf[i]);
            }
            for (size_t i = guard + offset + size; i < guard + 32 + size + guard; i++) {
                TEST_ASSERT_EQUAL_UINT8(fill, buf[i]);
            }
        }
        free(buf);
    }
}

void test_preview_one_by_one(void) {
    /* From 1x1, and from a block with a known mean. */
    const uint8_t one = ROSE;
    const uint8_t one_want[] = {200, 16, 64};
    check_hand(&one, 1, 1, 1, 1, one_want);

    const uint8_t two[] = {WHITE, BLACK, BLACK, WHITE};
    const uint8_t two_want[] = {188, 188, 188};
    check_hand(two, 2, 2, 1, 1, two_want);

    /* 3x3: five whites of nine is 36408.9 -> 36409, between 36177 (196) and 36591 (197). */
    const uint8_t three[] = {WHITE, BLACK, WHITE, BLACK, WHITE, BLACK, WHITE, BLACK, WHITE};
    const uint8_t three_want[] = {197, 197, 197};
    check_hand(three, 3, 3, 1, 1, three_want);
}
