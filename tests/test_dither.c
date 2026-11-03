/*
 * Tests for the ditherer, stage S7 (src/epi_dither.c): structural checks.
 */
#include "epi_dither.h"
#include "epi_math.h"
#include "unity.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MAXW 64
#define MAXH 8

static const epi_colour_tables_t* tables(void) {
    return epi_colour_tables(epi_panel_el133uf1.tables);
}

static uint32_t rng_state;
static uint32_t rng(void) {
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state >> 8;
}

/* A mid-tone target inside the panel's range (between black and white ink). */
static void random_target(int32_t t[3]) {
    const epi_colour_tables_t* tb = tables();
    for (int c = 0; c < 3; c++) {
        int32_t lo = tb->ink_lin[0][c], hi = tb->ink_lin[1][c];
        t[c] = lo + (int32_t)(rng() % (uint32_t)(hi - lo));
    }
}

/* The error row, cell x = 0: after a finished row, the row below it. */
static const int32_t* err_cells(const epi_dither_ctx_t* ctx) {
    return (const int32_t*)ctx->err + 3 * EPI_DITHER_ERR_PAD;
}

static void run(uint16_t w, uint16_t h, const int32_t (*t)[3], uint8_t* out) {
    static int32_t mem[(MAXW + 2 * EPI_DITHER_ERR_PAD) * 3];
    TEST_ASSERT_TRUE(epi_dither_err_size(&tables()->base, w) <= sizeof(mem));
    epi_dither_ctx_t ctx;
    epi_dither_init(&ctx, &tables()->base, w, mem);
    for (uint16_t y = 0; y < h; y++) {
        epi_dither_row(&ctx, t + (size_t)y * w, out + (size_t)y * w);
    }
    TEST_ASSERT_EQUAL_UINT32(h, ctx.row);
}

void test_dither_flat_ink(void) {
    const epi_colour_tables_t* tb = tables();
    static int32_t t[MAXH * MAXW][3];
    static uint8_t out[MAXH * MAXW];
    const uint16_t widths[] = {1, 2, 3, 17};
    for (unsigned i = 0; i < tb->base.ink_count; i++) {
        for (size_t wi = 0; wi < sizeof(widths) / sizeof(widths[0]); wi++) {
            uint16_t w = widths[wi];
            for (int p = 0; p < MAXH * w; p++) {
                memcpy(t[p], tb->ink_lin[i], sizeof(t[p]));
            }
            memset(out, 0xff, sizeof(out));
            run(w, MAXH, (const int32_t (*)[3])t, out);
            for (int p = 0; p < MAXH * w; p++) {
                TEST_ASSERT_EQUAL_UINT8(i, out[p]);
            }
        }
    }
}

/*
 * Row 1 runs right-to-left with the kernel mirrored: a row of T on row 1
 * (after a zero-error ink row) must be the mirror image of reverse(T) on
 * row 2 (after two zero-error ink rows), indices and the error left below.
 */
void test_dither_serpentine(void) {
    const epi_colour_tables_t* tb = tables();
    enum { W = 23 };
    static int32_t mem_a[(W + 2 * EPI_DITHER_ERR_PAD) * 3], mem_b[(W + 2 * EPI_DITHER_ERR_PAD) * 3];
    int32_t white[W][3], row[W][3], rev[W][3];
    uint8_t oa[W], ob[W], junk[W];
    rng_state = 7;
    for (int x = 0; x < W; x++) {
        memcpy(white[x], tb->ink_lin[1], sizeof(white[x]));
        random_target(row[x]);
    }
    for (int x = 0; x < W; x++) {
        memcpy(rev[x], row[W - 1 - x], sizeof(rev[x]));
    }
    epi_dither_ctx_t a, b;
    epi_dither_init(&a, &tb->base, W, mem_a);
    epi_dither_init(&b, &tb->base, W, mem_b);
    epi_dither_row(&a, (const int32_t (*)[3])white, junk);
    epi_dither_row(&a, (const int32_t (*)[3])row, oa);
    epi_dither_row(&b, (const int32_t (*)[3])white, junk);
    epi_dither_row(&b, (const int32_t (*)[3])white, junk);
    epi_dither_row(&b, (const int32_t (*)[3])rev, ob);

    int nonwhite = 0;
    for (int x = 0; x < W; x++) {
        TEST_ASSERT_EQUAL_UINT8(oa[x], ob[W - 1 - x]);
        nonwhite += oa[x] != 1;
    }
    TEST_ASSERT_TRUE(nonwhite > 0);

    /* Error carried to the next row, including the padding cells. */
    const int32_t* ea = err_cells(&a);
    const int32_t* eb = err_cells(&b);
    int nonzero = 0;
    for (int x = -EPI_DITHER_ERR_PAD; x < W + EPI_DITHER_ERR_PAD; x++) {
        for (int c = 0; c < 3; c++) {
            TEST_ASSERT_EQUAL_INT32(ea[3 * x + c], eb[3 * (W - 1 - x) + c]);
            nonzero += ea[3 * x + c] != 0;
        }
    }
    TEST_ASSERT_TRUE(nonzero > 0);

    /* And the two runs really differ from a left-to-right row of T. */
    int32_t mem_c[(W + 2 * EPI_DITHER_ERR_PAD) * 3];
    uint8_t oc[W];
    epi_dither_ctx_t c;
    epi_dither_init(&c, &tb->base, W, mem_c);
    epi_dither_row(&c, (const int32_t (*)[3])row, oc);
    int same = 0;
    for (int x = 0; x < W; x++) {
        same += oc[x] == oa[x];
    }
    /* Row 0 of T (left-to-right) is a different traversal of the same targets. */
    TEST_ASSERT_TRUE(same < W);
}

void test_dither_fs_split_conserves(void) {
    const int32_t samples[] = {0,  1,   -1,    2,      -2,    7,      -7,      8,        -8, 15, -15, 16, -16,
                               17, -17, 65535, -65535, 65536, -65536, 1234567, -1234567, 3,  -3, 5,   -5};
    rng_state = 11;
    for (int k = 0; k < 20000; k++) {
        int32_t e = k < (int)(sizeof(samples) / sizeof(samples[0])) ? samples[k] : (int32_t)(rng() % 400001u) - 200000;
        int32_t p[4];
        epi_dither_fs_split(e, p);
        TEST_ASSERT_EQUAL_INT64((int64_t)e, (int64_t)p[0] + p[1] + p[2] + p[3]);
        /* The 7, 3, 5 parts are rounded; the 1/16 remainder stays close. */
        TEST_ASSERT_TRUE(llabs((int64_t)p[3] * 16 - e) <= 3 * 8 + 8);
    }
}

/*
 * End to end: the error left below by a pixel is exactly its damped error
 * split by the kernel. Width 2, row 0: x = 0 is the white ink (zero error),
 * x = 1 a non-ink target whose 7/16 is dropped past the right edge.
 */
void test_dither_error_conservation(void) {
    const epi_colour_tables_t* tb = tables();
    static int32_t mem[(2 + 2 * EPI_DITHER_ERR_PAD) * 3];
    rng_state = 3;
    for (int trial = 0; trial < 200; trial++) {
        int32_t t[2][3];
        memcpy(t[0], tb->ink_lin[1], sizeof(t[0]));
        random_target(t[1]);
        uint8_t out[2];
        epi_dither_ctx_t ctx;
        epi_dither_init(&ctx, &tb->base, 2, mem);
        epi_dither_row(&ctx, (const int32_t (*)[3])t, out);
        TEST_ASSERT_EQUAL_UINT8(1, out[0]);

        int32_t kl, kc, e[3], ed[3];
        epi_dither_damp_coeffs(tb, t[1], &kl, &kc);
        for (int c = 0; c < 3; c++) {
            e[c] = t[1][c] - tb->ink_lin[out[1]][c];
        }
        epi_dither_damp(tb, e, kl, kc, ed);
        const int32_t* below = err_cells(&ctx);
        for (int c = 0; c < 3; c++) {
            int32_t p[4];
            epi_dither_fs_split(ed[c], p);
            TEST_ASSERT_EQUAL_INT32(p[1], below[3 * 0 + c]); /* behind-below */
            TEST_ASSERT_EQUAL_INT32(p[2], below[3 * 1 + c]); /* below */
            TEST_ASSERT_EQUAL_INT32(
                0, below[3 * 2 + c]
            ); /* ahead (p[0]) dropped in the padding, ahead-below in the carry */
            TEST_ASSERT_EQUAL_INT32(0, below[-3 + c]);
            TEST_ASSERT_EQUAL_INT64((int64_t)ed[c], (int64_t)p[0] + p[1] + p[2] + p[3]);
        }
    }
}

/* One pixel's split on the two-row model: cur and nxt are indexed from cell
 * x = -1, step cells apart. */
static void model_taps(int32_t* cur, int32_t* nxt, int step, int x, int d, int32_t ed) {
    int32_t p[4];
    epi_dither_fs_split(ed, p);
    cur[(x + 1 + d) * step] += p[0];
    nxt[(x + 1 - d) * step] += p[1];
    nxt[(x + 1) * step] += p[2];
    nxt[(x + 1 + d) * step] += p[3];
}

/*
 * Row after row, the error row against a model of two full rows with padding,
 * replayed from the indices chosen: the cells hold exactly the row below,
 * with nothing left of earlier rows, and the padding is zero after every row.
 */
void test_dither_error_row_model(void) {
    enum { W = 9, H = 12 };
    const epi_colour_tables_t* tb = tables();
    static int32_t mem[(W + 2 * EPI_DITHER_ERR_PAD) * 3];
    int32_t cur[(W + 2) * 3] = {0}, nxt[(W + 2) * 3], t[W][3];
    uint8_t out[W];
    epi_dither_ctx_t ctx;
    rng_state = 19;
    epi_dither_init(&ctx, &tb->base, W, mem);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            random_target(t[x]);
        }
        epi_dither_row(&ctx, (const int32_t (*)[3])t, out);
        const int d = y & 1 ? -1 : 1;
        memset(nxt, 0, sizeof nxt);
        for (int k = 0; k < W; k++) {
            const int x = d > 0 ? k : W - 1 - k;
            int32_t kl, kc, e[3], ed[3];
            epi_dither_damp_coeffs(tb, t[x], &kl, &kc);
            for (int c = 0; c < 3; c++) {
                e[c] = t[x][c] + cur[(x + 1) * 3 + c] - tb->ink_lin[out[x]][c];
            }
            epi_dither_damp(tb, e, kl, kc, ed);
            for (int c = 0; c < 3; c++) {
                model_taps(cur + c, nxt + c, 3, x, d, ed[c]);
            }
        }
        const int32_t* cells = err_cells(&ctx);
        TEST_ASSERT_EQUAL_INT32_ARRAY(nxt + 3, cells, W * 3);
        for (int c = 0; c < 3; c++) {
            TEST_ASSERT_EQUAL_INT32(0, cells[-3 + c]);
            TEST_ASSERT_EQUAL_INT32(0, cells[W * 3 + c]);
        }
        memcpy(cur, nxt, sizeof cur);
    }
    TEST_ASSERT_EQUAL_UINT32(H, ctx.row);
}

/* The same on grey: one channel, the target through the coverage toe. */
void test_dither_grey_error_row_model(void) {
    enum { W = 9, H = 12 };
    const epi_grey_tables_t* tb = epi_grey_tables(epi_panel_gdey075t7_grey4.tables);
    static int32_t mem[W + 2 * EPI_DITHER_ERR_PAD];
    int32_t cur[W + 2] = {0}, nxt[W + 2], t[W];
    uint8_t out[W];
    epi_dither_ctx_t ctx;
    rng_state = 23;
    TEST_ASSERT_EQUAL_size_t(sizeof mem, epi_dither_err_size(&tb->base, W));
    epi_dither_init(&ctx, &tb->base, W, mem);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            t[x] = (int32_t)(rng() % 65537u);
        }
        epi_dither_grey_row(&ctx, t, out);
        const int d = y & 1 ? -1 : 1;
        memset(nxt, 0, sizeof nxt);
        for (int k = 0; k < W; k++) {
            const int x = d > 0 ? k : W - 1 - k;
            const int32_t tt = epi_dither_grey_toe(&ctx, t[x]);
            const int32_t e = tt + cur[x + 1] - tb->level_y[out[x]];
            const int32_t kl = epi_dither_grey_cutl(tt);
            const int32_t ed = epi_sat_i32((int64_t)e - epi_round_shift((int64_t)kl * e, 24));
            model_taps(cur, nxt, 1, x, d, ed);
        }
        TEST_ASSERT_EQUAL_INT32_ARRAY(nxt + 1, mem + EPI_DITHER_ERR_PAD, W);
        TEST_ASSERT_EQUAL_INT32(0, mem[0]);
        TEST_ASSERT_EQUAL_INT32(0, mem[W + 2 * EPI_DITHER_ERR_PAD - 1]);
        memcpy(cur, nxt, sizeof cur);
    }
}

/* Damping: no cut for a white target; the full .05 for a dark chromatic one. */
void test_dither_damp(void) {
    const epi_colour_tables_t* tb = tables();
    int32_t kl, kc;
    epi_dither_damp_coeffs(tb, tb->ink_lin[1], &kl, &kc); /* white: L = 1, C = 0 */
    TEST_ASSERT_EQUAL_INT32(0, kl);
    /* C_ok(white) is 0 up to the Q16 pipeline's rounding (1 LSB → kc 320). */
    TEST_ASSERT_TRUE(kc <= 3 * 320);
    epi_dither_damp_coeffs(tb, tb->ink_lin[3], &kl, &kc); /* red: dark, chromatic */
    TEST_ASSERT_EQUAL_INT32(838861, kl);                  /* .05 in Q24, rounded */
    TEST_ASSERT_EQUAL_INT32(838861, kc);
    /* An error along n̂ loses 5% (kl); one orthogonal to it loses 5% (kc). */
    int32_t e[3], out[3];
    for (int c = 0; c < 3; c++) {
        e[c] = tb->nhat[c];
    }
    epi_dither_damp(tb, e, 838861, 0, out);
    for (int c = 0; c < 3; c++) {
        TEST_ASSERT_INT32_WITHIN(2, (int32_t)((int64_t)tb->nhat[c] * 95 / 100), out[c]);
    }
    epi_dither_damp(tb, e, 0, 838861, out);
    for (int c = 0; c < 3; c++) {
        TEST_ASSERT_INT32_WITHIN(2, tb->nhat[c], out[c]);
    }
}

/* nearest_ink's precondition: every |ink_oklab| < 2^30, for each built-in colour panel. */
void test_dither_inks_small(void) {
    const epi_panel_t* panels[] = {&epi_panel_el133uf1, &epi_panel_gdey075t7_mono, &epi_panel_gdey075t7_grey4};
    int colour = 0;
    for (size_t p = 0; p < sizeof panels / sizeof panels[0]; p++) {
        if (panels[p]->tables->kind != EPI_PANEL_COLOUR) {
            continue;
        }
        const epi_colour_tables_t* tb = epi_colour_tables(panels[p]->tables);
        colour++;
        for (unsigned i = 0; i < tb->base.ink_count; i++) {
            for (int c = 0; c < 3; c++) {
                TEST_ASSERT_TRUE(tb->ink_oklab[i][c] > -(1 << 30) && tb->ink_oklab[i][c] < (1 << 30));
            }
        }
    }
    TEST_ASSERT_GREATER_THAN_INT(0, colour);
}

/* Widths 1 and 2: every tap but "below" may fall outside; no overrun. */
void test_dither_narrow(void) {
    const epi_colour_tables_t* tb = tables();
    for (uint16_t w = 1; w <= 2; w++) {
        size_t n = epi_dither_err_size(&tb->base, w) / sizeof(int32_t);
        int32_t* mem = malloc((n + 32) * sizeof(int32_t));
        TEST_ASSERT_NOT_NULL(mem);
        for (size_t k = n; k < n + 32; k++) {
            mem[k] = 0x5a5a5a5a;
        }
        enum { H = 50 };
        int32_t t[H * 2][3];
        uint8_t out[H * 2];
        rng_state = 5u + w;
        for (int p = 0; p < H * w; p++) {
            random_target(t[p]);
        }
        epi_dither_ctx_t ctx;
        epi_dither_init(&ctx, &tb->base, w, mem);
        for (int y = 0; y < H; y++) {
            epi_dither_row(&ctx, (const int32_t (*)[3])t + (size_t)y * w, out + (size_t)y * w);
        }
        for (int p = 0; p < H * w; p++) {
            TEST_ASSERT_TRUE(out[p] < tb->base.ink_count);
        }
        for (size_t k = n; k < n + 32; k++) {
            TEST_ASSERT_EQUAL_HEX32(0x5a5a5a5a, (uint32_t)mem[k]);
        }
        free(mem);
    }
}
