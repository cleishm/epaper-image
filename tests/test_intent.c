/*
 * Tests for the rendering intent, stages S1–S6 (src/epi_intent.h):
 * structural checks on the EL133UF1 panel.
 */
#include "epaper_image.h"
#include "epi_intent.h"
#include "epi_intent_stages.h"
#include "epi_math.h"
#include "epi_panel_tables.h"
#include "test_util.h"
#include "unity.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const epi_panel_tables_t* tables(void) {
    return epi_panel_el133uf1.tables;
}

static const epi_colour_tables_t* colour_tables(void) {
    return epi_colour_tables(tables());
}

static const epi_intent_t* preset(size_t i) {
    return epi_panel_el133uf1.presets[i].intent;
}

static int hull_inside(const int32_t xyz[3]) {
    const epi_colour_tables_t* t = colour_tables();
    for (int f = 0; f < t->hull_face_count; f++) {
        int64_t s = (int64_t)t->hull_n[f][0] * xyz[0] + (int64_t)t->hull_n[f][1] * xyz[1] +
            (int64_t)t->hull_n[f][2] * xyz[2] + t->hull_d[f];
        if (s > 0) {
            return 0;
        }
    }
    return 1;
}

/* One flat pixel: base = L*src (as the blur of a flat image gives). */
typedef struct {
    epi_intent_stages_t st;
    int32_t target[3];
} pixel_t;

static pixel_t run_pixel(const epi_intent_t* in, epi_rgb_t rgb) {
    epi_intent_ctx_t ctx;
    TEST_ASSERT_EQUAL(EPI_OK, epi_intent_init(&ctx, tables(), in));
    pixel_t p;
    int16_t ls;
    epi_intent_lstar_row(&rgb, &ls, 1);
    epi_intent_pixel_stages(&ctx, &rgb, ls, p.target, &p.st);
    TEST_ASSERT_EQUAL_INT32(ls, p.st.lsrc); /* S1 is recomputed identically */
    /* The production row call agrees with the staged one. */
    int32_t row_target[1][3];
    epi_intent_row(&ctx, &rgb, &ls, row_target, 1);
    TEST_ASSERT_EQUAL_INT32_ARRAY(p.target, row_target[0], 3);
    return p;
}

void test_intent_init_validation(void) {
    epi_intent_ctx_t ctx;
    const epi_intent_t ok = *preset(0);
    TEST_ASSERT_EQUAL(EPI_OK, epi_intent_init(&ctx, tables(), &ok));

    for (size_t i = 0; i < epi_panel_el133uf1.preset_count; i++) {
        TEST_ASSERT_EQUAL(EPI_OK, epi_intent_init(&ctx, tables(), preset(i)));
    }

    const float nan = bits_f(0x7fc00000u);
    const float inf = bits_f(0x7f800000u);
    const float bad01[] = {-0.001f, 1.001f, nan, inf, -inf};
    const float bad03[] = {-0.001f, 3.001f, nan, inf, -inf};
    for (size_t i = 0; i < sizeof bad01 / sizeof bad01[0]; i++) {
        epi_intent_t in = ok;
        in.tone = bad01[i];
        TEST_ASSERT_EQUAL(EPI_ERR_INVALID_ARG, epi_intent_init(&ctx, tables(), &in));
        in = ok;
        in.detail = bad01[i];
        TEST_ASSERT_EQUAL(EPI_ERR_INVALID_ARG, epi_intent_init(&ctx, tables(), &in));
        in = ok;
        in.adapt = bad01[i];
        TEST_ASSERT_EQUAL(EPI_ERR_INVALID_ARG, epi_intent_init(&ctx, tables(), &in));
        in = ok;
        in.vibrance = bad03[i];
        TEST_ASSERT_EQUAL(EPI_ERR_INVALID_ARG, epi_intent_init(&ctx, tables(), &in));
        in = ok;
        in.veil = bad03[i];
        TEST_ASSERT_EQUAL(EPI_ERR_INVALID_ARG, epi_intent_init(&ctx, tables(), &in));
    }
    const float bad20[] = {-0.1f, 20.1f, nan};
    for (size_t i = 0; i < sizeof bad20 / sizeof bad20[0]; i++) {
        epi_intent_t in = ok;
        in.lift = bad20[i];
        TEST_ASSERT_EQUAL(EPI_ERR_INVALID_ARG, epi_intent_init(&ctx, tables(), &in));
    }
    /* The range ends are valid. */
    epi_intent_t lo = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    epi_intent_t hi = {1.0f, 1.0f, 1.0f, 3.0f, 3.0f, 20.0f};
    TEST_ASSERT_EQUAL(EPI_OK, epi_intent_init(&ctx, tables(), &lo));
    TEST_ASSERT_EQUAL(EPI_OK, epi_intent_init(&ctx, tables(), &hi));
}

/* Source white lands on paper white (D = 1 at L*src 100) in every preset. */
void test_intent_white(void) {
    const epi_colour_tables_t* t = colour_tables();
    const epi_rgb_t white = {255, 255, 255};
    for (size_t i = 0; i < epi_panel_el133uf1.preset_count; i++) {
        pixel_t p = run_pixel(preset(i), white);
        TEST_ASSERT_EQUAL_INT32(100 * 256, p.st.lsrc);
        TEST_ASSERT_EQUAL_INT32(65536, p.st.d);
        TEST_ASSERT_EQUAL_INT32(100 * 256, p.st.lab_panel[0]);
        TEST_ASSERT_EQUAL_INT32(0, p.st.lab_panel[1]);
        TEST_ASSERT_EQUAL_INT32(0, p.st.lab_panel[2]);
        TEST_ASSERT_EQUAL_INT32_ARRAY(p.st.lab_panel, p.st.lab_colour, 3);
        TEST_ASSERT_EQUAL_UINT8(0, p.st.moved);
        /* Exactly the white ink: zero diffusion error on white. */
        TEST_ASSERT_EQUAL_INT32_ARRAY(t->white_xyz, p.st.xyz, 3);
        TEST_ASSERT_EQUAL_INT32_ARRAY(t->ink_lin[1], p.target, 3);
    }
}

/* Source black: L*src 0, tone below L*min, so the cusp move lifts it onto
 * the bottom of the neutral interval, near the black ink. */
void test_intent_black(void) {
    const epi_colour_tables_t* t = colour_tables();
    const epi_rgb_t black = {0, 0, 0};
    for (size_t i = 0; i < epi_panel_el133uf1.preset_count; i++) {
        epi_intent_ctx_t ctx;
        TEST_ASSERT_EQUAL(EPI_OK, epi_intent_init(&ctx, &t->base, preset(i)));
        pixel_t p = run_pixel(preset(i), black);
        TEST_ASSERT_EQUAL_INT32(0, p.st.lsrc);
        TEST_ASSERT_EQUAL_INT32(ctx.adapt_q16, p.st.d);
        TEST_ASSERT_EQUAL_INT32(ctx.tone_t_q8[0], p.st.lab_tone[0]);
        TEST_ASSERT_TRUE(hull_inside(p.st.xyz));
        for (int c = 0; c < 3; c++) {
            TEST_ASSERT_INT32_WITHIN(200, t->ink_lin[0][c], p.target[c]); /* ±.003 linear */
        }
    }
}

/* A mid grey: L* is the tone table's value, a* and b* the D = adapt tint;
 * well inside the hull, so S5 leaves it alone. */
void test_intent_mid_grey(void) {
    const epi_rgb_t grey = {119, 119, 119}; /* L*src ≈ 50 */
    for (size_t i = 0; i < epi_panel_el133uf1.preset_count; i++) {
        epi_intent_ctx_t ctx;
        TEST_ASSERT_EQUAL(EPI_OK, epi_intent_init(&ctx, tables(), preset(i)));
        pixel_t p = run_pixel(preset(i), grey);
        TEST_ASSERT_INT32_WITHIN(64, 50 * 256, p.st.lsrc);
        TEST_ASSERT_EQUAL_INT32(ctx.adapt_q16, p.st.d);
        int32_t k = p.st.lsrc >> 6, fr = p.st.lsrc & 63;
        int32_t tv = ctx.tone_t_q8[k] + ((ctx.tone_t_q8[k + 1] - ctx.tone_t_q8[k]) * fr + 32) / 64;
        TEST_ASSERT_INT32_WITHIN(1, tv, p.st.lab_tone[0]);
        TEST_ASSERT_EQUAL_INT32_ARRAY(&p.st.lab_panel[1], &p.st.lab_tone[1], 2);
        TEST_ASSERT_INT32_WITHIN(3 * 256, 0, p.st.lab_panel[1]); /* a mild tint only */
        TEST_ASSERT_INT32_WITHIN(3 * 256, 0, p.st.lab_panel[2]);
        TEST_ASSERT_EQUAL_UINT8(0, p.st.moved);
        TEST_ASSERT_TRUE(hull_inside(p.st.xyz));
    }
}

/* Saturated sources far outside the hull end exactly inside it, at
 * reduced chroma and with their hue roughly kept. */
void test_intent_saturated_ends_inside(void) {
    const epi_rgb_t cols[] = {
        {0, 0, 255}, {255, 0, 0}, {0, 255, 0}, {255, 0, 255}, {0, 255, 255}, {255, 255, 0}, {40, 0, 90}
    };
    for (size_t i = 0; i < epi_panel_el133uf1.preset_count; i++) {
        for (size_t j = 0; j < sizeof cols / sizeof cols[0]; j++) {
            pixel_t p = run_pixel(preset(i), cols[j]);
            TEST_ASSERT_EQUAL_UINT8(1, p.st.moved);
            TEST_ASSERT_TRUE(hull_inside(p.st.xyz));
        }
    }
    /* Vibrance only changes chroma, so a neutral is unaffected and a
     * saturated colour gains chroma before the move. */
    epi_intent_t viv = *preset(3);
    TEST_ASSERT_TRUE(viv.vibrance != 1.0f);
    pixel_t n = run_pixel(&viv, (epi_rgb_t){119, 119, 119});
    TEST_ASSERT_EQUAL_INT32_ARRAY(n.st.lab_tone, n.st.lab_colour, 3);
    pixel_t s = run_pixel(&viv, (epi_rgb_t){60, 90, 160});
    TEST_ASSERT_TRUE(s.st.lab_colour[2] < s.st.lab_tone[2]);
}

void test_intent_blur_constant(void) {
    const epi_panel_tables_t* t = tables();
    const int r = t->kernel_radius;
    const uint16_t widths[] = {1, 5, 37, 300};
    int16_t in[300], h[300], out[300];
    const int16_t* rows[2 * EPI_MAX_KERNEL_RADIUS + 1];
    const int16_t values[] = {0, 1, 12345, 25600, -7};
    for (size_t vi = 0; vi < sizeof values / sizeof values[0]; vi++) {
        for (size_t wi = 0; wi < sizeof widths / sizeof widths[0]; wi++) {
            uint16_t w = widths[wi];
            for (int x = 0; x < w; x++) {
                in[x] = values[vi];
            }
            epi_blur_h(t, in, h, w);
            for (int j = 0; j <= 2 * r; j++) {
                rows[j] = h;
            }
            epi_blur_v(t, rows, out, w);
            for (int x = 0; x < w; x++) {
                TEST_ASSERT_EQUAL_INT16(values[vi], h[x]);
                TEST_ASSERT_EQUAL_INT16(values[vi], out[x]);
            }
        }
    }
}

/* Mirroring the input mirrors the output (symmetric kernel, symmetric
 * edge replication, rounding half away from zero). */
void test_intent_blur_symmetry(void) {
    const epi_panel_tables_t* t = tables();
    const int r = t->kernel_radius;
    enum { W = 101 };
    int16_t in[W], rev[W], out[W], out_rev[W];
    uint32_t s = 12345u;
    for (int x = 0; x < W; x++) {
        s = s * 1103515245u + 12345u;
        in[x] = (int16_t)((s >> 16) % 25601u);
        rev[W - 1 - x] = in[x];
    }
    epi_blur_h(t, in, out, W);
    epi_blur_h(t, rev, out_rev, W);
    for (int x = 0; x < W; x++) {
        TEST_ASSERT_EQUAL_INT16(out[x], out_rev[W - 1 - x]);
    }
    /* An impulse spreads into the kernel itself (rounded). */
    int16_t imp[W] = {0};
    imp[50] = 25600;
    epi_blur_h(t, imp, out, W);
    for (int j = -r; j <= r; j++) {
        int32_t want = (int32_t)(((int64_t)t->kernel[j + r] * 25600 + 16384) >> 15);
        TEST_ASSERT_EQUAL_INT16(want, out[50 + j]);
    }
    /* Vertical: reversed row pointers give the same output. */
    static int16_t img[2 * EPI_MAX_KERNEL_RADIUS + 1][W];
    const int16_t* rows[2 * EPI_MAX_KERNEL_RADIUS + 1];
    const int16_t* rows_rev[2 * EPI_MAX_KERNEL_RADIUS + 1];
    for (int j = 0; j <= 2 * r; j++) {
        for (int x = 0; x < W; x++) {
            s = s * 1103515245u + 12345u;
            img[j][x] = (int16_t)((s >> 16) % 25601u);
        }
        rows[j] = img[j];
        rows_rev[2 * r - j] = img[j];
    }
    epi_blur_v(t, rows, out, W);
    epi_blur_v(t, rows_rev, out_rev, W);
    TEST_ASSERT_EQUAL_INT16_ARRAY(out, out_rev, W);
}

/* Same input and intent → identical context and output; prints an FNV-1a
 * fingerprint of a synthetic image's targets for cross-target comparison. */
void test_intent_deterministic(void) {
    enum { W = 64 };
    epi_rgb_t src[W];
    int16_t ls[W];
    int32_t a[W][3], b[W][3];
    for (int x = 0; x < W; x++) {
        src[x] = (epi_rgb_t){(uint8_t)(x * 4), (uint8_t)(255 - x * 3), (uint8_t)((x * 37) & 255)};
    }
    uint64_t hash = 0xcbf29ce484222325u;
    for (size_t i = 0; i < epi_panel_el133uf1.preset_count; i++) {
        epi_intent_ctx_t c1, c2;
        memset(&c1, 0xa5, sizeof c1);
        memset(&c2, 0xa5, sizeof c2); /* same fill: padding bytes compare equal */
        TEST_ASSERT_EQUAL(EPI_OK, epi_intent_init(&c1, tables(), preset(i)));
        TEST_ASSERT_EQUAL(EPI_OK, epi_intent_init(&c2, tables(), preset(i)));
        TEST_ASSERT_EQUAL_MEMORY(&c1, &c2, sizeof c1);
        epi_intent_lstar_row(src, ls, W);
        epi_intent_row(&c1, src, ls, a, W);
        epi_intent_row(&c2, src, ls, b, W);
        TEST_ASSERT_EQUAL_MEMORY(a, b, sizeof a);
        for (int x = 0; x < W; x++) {
            for (int c = 0; c < 3; c++) {
                uint32_t u = (uint32_t)a[x][c];
                for (int k = 0; k < 4; k++) {
                    hash = (hash ^ ((u >> (8 * k)) & 0xffu)) * 0x100000001b3u;
                }
            }
        }
    }
    printf("intent fingerprint 0x%016llx\n", (unsigned long long)hash);
}

/* Veil: source white sits f below paper white, and stays neutral. */
void test_intent_veil(void) {
    epi_intent_t in = *preset(0);
    in.veil = 2.0f;
    pixel_t p = run_pixel(&in, (epi_rgb_t){255, 255, 255});
    TEST_ASSERT_EQUAL_INT32(100 * 256 - 2 * 256, p.st.lab_colour[0]);
    TEST_ASSERT_EQUAL_INT32(0, p.st.lab_colour[1]);
    TEST_ASSERT_EQUAL_INT32(0, p.st.lab_colour[2]);
    TEST_ASSERT_EQUAL_UINT8(0, p.st.moved);
    /* Below L* 90 the veil does nothing. */
    pixel_t g = run_pixel(&in, (epi_rgb_t){119, 119, 119});
    TEST_ASSERT_EQUAL_INT32_ARRAY(g.st.lab_tone, g.st.lab_colour, 3);
}

/* Lift: the tone curve becomes T(P(L)), the source colour rises with it, and
 * black, white, the highlights and saturated colours are left alone. */
void test_intent_lift(void) {
    epi_intent_t off = *preset(0);
    off.lift = 0.0f;
    epi_intent_t in = off;
    in.lift = 8.0f;
    epi_intent_ctx_t c0, c8;
    TEST_ASSERT_EQUAL(EPI_OK, epi_intent_init(&c0, tables(), &off));
    TEST_ASSERT_EQUAL(EPI_OK, epi_intent_init(&c8, tables(), &in));

    /* L* 35..45 has full weight, so T(40) at lift 8 is T(48) at lift 0; 1/4-L* knots. */
    TEST_ASSERT_EQUAL_INT32(c0.tone_t_q8[48 * 4], c8.tone_t_q8[40 * 4]);
    /* Black and everything from L* 82 up keep their curve. */
    TEST_ASSERT_EQUAL_INT32(c0.tone_t_q8[0], c8.tone_t_q8[0]);
    for (int i = 82 * 4; i < EPI_TONE_TABLE_LEN; i++) {
        TEST_ASSERT_EQUAL_INT32(c0.tone_t_q8[i], c8.tone_t_q8[i]);
    }
    /* The curve's falling slope around L* 60 is mostly kept out of local contrast by detail 0.75. */
    TEST_ASSERT_TRUE(c8.tone_g_q16[60 * 4] > 55000);

    /* A muted colour comes out lighter and more colourful; S1's L* and D don't move. */
    const epi_rgb_t muted = {120, 100, 90};
    pixel_t m0 = run_pixel(&off, muted);
    pixel_t m8 = run_pixel(&in, muted);
    TEST_ASSERT_TRUE(m8.st.lab_tone[0] > m0.st.lab_tone[0]);
    TEST_ASSERT_EQUAL_INT32(m0.st.lsrc, m8.st.lsrc);
    TEST_ASSERT_EQUAL_INT32(m0.st.d, m8.st.d);
    TEST_ASSERT_TRUE(
        epi_hypot_q(m8.st.lab_tone[1], m8.st.lab_tone[2]) > epi_hypot_q(m0.st.lab_tone[1], m0.st.lab_tone[2])
    );

    /* A saturated red takes none of it, so it doesn't climb above its ink in white dots. */
    pixel_t r0 = run_pixel(&off, (epi_rgb_t){170, 30, 30});
    pixel_t r8 = run_pixel(&in, (epi_rgb_t){170, 30, 30});
    TEST_ASSERT_EQUAL_INT32_ARRAY(r0.st.lab_tone, r8.st.lab_tone, 3);
    TEST_ASSERT_EQUAL_INT32_ARRAY(r0.target, r8.target, 3);

    /* Source white, source black and highlights are unchanged. */
    const epi_rgb_t fixed[] = {{255, 255, 255}, {0, 0, 0}, {215, 215, 215}};
    for (size_t i = 0; i < sizeof fixed / sizeof fixed[0]; i++) {
        pixel_t p0 = run_pixel(&off, fixed[i]);
        pixel_t p8 = run_pixel(&in, fixed[i]);
        TEST_ASSERT_EQUAL_INT32_ARRAY(p0.st.lab_tone, p8.st.lab_tone, 3);
        TEST_ASSERT_EQUAL_INT32_ARRAY(p0.target, p8.target, 3);
    }

    /* At the cap the curve is monotone, and colours driven past the source range end in the hull. */
    epi_intent_t cap = off;
    cap.lift = 20.0f;
    cap.vibrance = 3.0f;
    epi_intent_ctx_t c20;
    TEST_ASSERT_EQUAL(EPI_OK, epi_intent_init(&c20, tables(), &cap));
    for (int i = 1; i < EPI_TONE_TABLE_LEN; i++) {
        TEST_ASSERT_TRUE(c20.tone_t_q8[i] >= c20.tone_t_q8[i - 1]);
    }
    for (int r = 0; r < 256; r += 51) {
        for (int g = 0; g < 256; g += 51) {
            for (int b = 0; b < 256; b += 51) {
                pixel_t p = run_pixel(&cap, (epi_rgb_t){(uint8_t)r, (uint8_t)g, (uint8_t)b});
                TEST_ASSERT_TRUE(hull_inside(p.st.xyz));
            }
        }
    }
}

/* Vibrance at its maximum drives a* b* far out (through the linear branch
 * of f⁻¹); every result still ends inside the hull. */
void test_intent_extreme_vibrance(void) {
    epi_intent_t in = *preset(0);
    in.vibrance = 3.0f;
    for (int r = 0; r < 256; r += 51) {
        for (int g = 0; g < 256; g += 51) {
            for (int b = 0; b < 256; b += 51) {
                pixel_t p = run_pixel(&in, (epi_rgb_t){(uint8_t)r, (uint8_t)g, (uint8_t)b});
                TEST_ASSERT_TRUE(hull_inside(p.st.xyz));
            }
        }
    }
}
