/*
 * Tests for the streaming resampler (src/epi_resampler.c).
 *
 * - test_resampler_reference: the output against an independent
 *   double-precision model of the documented algorithm, on every regression
 *   case's configuration and on seeded random ones.
 * - Properties: flat stays flat, mirror-symmetric input and config give a
 *   mirror-symmetric output, same-size uncropped resizes are the identity.
 * - test_resampler_goldens: frozen FNV-1a 64 hashes of the regression cases'
 *   outputs (test_resampler_cases.h).
 *
 * Every resample runs through the streaming API one source row at a time,
 * popping until not OK after each push. The reference uses libm (tests only).
 */
#include "epaper_image.h"
#include "epi_math.h"
#include "test_util.h"
#include "unity.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Regression cases: a synthetic source (make_source with the case's seed), a
 * rect, an output size and a placement on a background canvas, and the
 * FNV-1a 64 hash of the whole output. For layout 1 (cover) and 2 (fit) the
 * rect and placement are what epi_layout_cover / epi_layout_fit give, and
 * the test checks that they still do. */
typedef struct {
    const char* name;
    uint16_t src_width, src_height;
    uint32_t seed;
    uint32_t rect_bits[4]; /* src_x, src_y, src_w, src_h as float32 bits */
    uint16_t dst_width, dst_height;
    int32_t dst_x, dst_y;
    uint16_t dst_w, dst_h;
    uint8_t bg[3];
    uint8_t filter;
    uint8_t layout; /* 0 none, 1 cover, 2 fit */
    uint64_t hash;
} epi_rs_case_t;

#include "test_resampler_cases.h"

/* ---- helpers ---- */

/* Uniform in [0, n). */
static uint32_t rnd(uint64_t* s, uint32_t n) {
    return (uint32_t)((xorshift64(s) >> 32) % n);
}

/* Uniform in [0, 1). */
static double rndf(uint64_t* s) {
    return (double)(xorshift64(s) >> 11) / 9007199254740992.0;
}

/* Synthetic source, integer-only: four regions (noise, gradients, a 0/255
 * checkerboard, noisy gradients) from a xorshift32 seed. */
static void make_source(epi_rgb_t* img, uint32_t w, uint32_t h, uint32_t seed) {
    uint32_t s = seed;
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint32_t region = ((4 * x) / w + (3 * y) / h) & 3u;
            uint8_t v[3];
            for (uint32_t c = 0; c < 3; c++) {
                int32_t n = (int32_t)(xorshift32(&s) >> 24);
                int32_t g;
                if (c == 0) {
                    g = (int32_t)((255 * x) / (w > 1 ? w - 1 : 1));
                } else if (c == 1) {
                    g = (int32_t)((255 * y) / (h > 1 ? h - 1 : 1));
                } else {
                    g = (int32_t)((255 * (x + y)) / (w + h > 2 ? w + h - 2 : 1));
                }
                int32_t o;
                if (region == 0) {
                    o = n;
                } else if (region == 1) {
                    o = g;
                } else if (region == 2) {
                    o = ((x / 3 + y / 2 + c) & 1u) ? 255 : 0;
                } else {
                    o = g + (n & 31) - 16;
                    o = o < 0 ? 0 : (o > 255 ? 255 : o);
                }
                v[c] = (uint8_t)o;
            }
            img[(size_t)y * w + x] = (epi_rgb_t){v[0], v[1], v[2]};
        }
    }
}

static epi_rgb_t* new_source(uint32_t w, uint32_t h, uint32_t seed) {
    epi_rgb_t* img = malloc((size_t)w * h * sizeof *img);
    TEST_ASSERT_NOT_NULL(img);
    make_source(img, w, h, seed);
    return img;
}

static epi_resampler_config_t base_cfg(uint32_t sw, uint32_t sh, uint16_t dw, uint16_t dh, epi_filter_t f) {
    epi_resampler_config_t c;
    memset(&c, 0, sizeof c);
    c.src_width = sw;
    c.src_height = sh;
    c.src_w = (float)sw;
    c.src_h = (float)sh;
    c.dst_width = dw;
    c.dst_height = dh;
    c.dst_w = dw;
    c.dst_h = dh;
    c.filter = f;
    return c;
}

/* Workspace with a deliberately misaligned start. */
typedef struct {
    void* mem;
    void* ws;
    size_t size;
} ws_t;

static epi_resampler_t* make(const epi_resampler_config_t* cfg, ws_t* w, unsigned misalign) {
    w->size = epi_resampler_workspace_size(cfg);
    TEST_ASSERT_NOT_EQUAL(0, w->size);
    w->mem = malloc(w->size + 8);
    TEST_ASSERT_NOT_NULL(w->mem);
    w->ws = (uint8_t*)w->mem + (misalign % 8u);
    epi_resampler_t* r = NULL;
    TEST_ASSERT_EQUAL(EPI_OK, epi_resampler_init(cfg, w->ws, w->size, &r));
    TEST_ASSERT_NOT_NULL(r);
    return r;
}

/* Resamples src (src_width × src_height) through the streaming API, one row
 * at a time, popping until not OK after each push. Returns the
 * dst_width × dst_height output (caller frees). One spare row catches an
 * extra pop before the count assert fails. */
static epi_rgb_t* resample(const epi_resampler_config_t* c, const epi_rgb_t* src, unsigned misalign, const char* name) {
    epi_rgb_t* out = malloc((size_t)c->dst_width * (c->dst_height + 1u) * sizeof *out);
    TEST_ASSERT_NOT_NULL(out);
    ws_t w;
    epi_resampler_t* r = make(c, &w, misalign);
    uint32_t n = 0;
    for (uint32_t y = 0; y <= c->src_height; y++) {
        if (y > 0) {
            epi_resampler_push(r, src + (size_t)(y - 1) * c->src_width);
        }
        while (epi_resampler_pop(r, out + (size_t)n * c->dst_width) == EPI_OK) {
            n++;
            TEST_ASSERT_TRUE_MESSAGE(n <= c->dst_height, name);
        }
    }
    char msg[200];
    snprintf(msg, sizeof msg, "%s: %u of %u rows after the last push", name, (unsigned)n, (unsigned)c->dst_height);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(c->dst_height, n, msg);
    free(w.mem);
    return out;
}

/* ---- regression cases ---- */

static epi_resampler_config_t case_cfg(const epi_rs_case_t* g) {
    epi_resampler_config_t c =
        base_cfg(g->src_width, g->src_height, g->dst_width, g->dst_height, (epi_filter_t)g->filter);
    c.src_x = bits_f(g->rect_bits[0]);
    c.src_y = bits_f(g->rect_bits[1]);
    c.src_w = bits_f(g->rect_bits[2]);
    c.src_h = bits_f(g->rect_bits[3]);
    c.dst_x = g->dst_x;
    c.dst_y = g->dst_y;
    c.dst_w = g->dst_w;
    c.dst_h = g->dst_h;
    c.background = (epi_rgb_t){g->bg[0], g->bg[1], g->bg[2]};
    return c;
}

/* epi_layout_cover (layout 1) or epi_layout_fit (2) must give want's rect
 * and placement. */
static void check_layout(const char* name, uint8_t layout, const epi_resampler_config_t* want) {
    epi_resampler_config_t c =
        base_cfg(want->src_width, want->src_height, want->dst_width, want->dst_height, EPI_FILTER_BOX);
    c.src_x = c.src_y = c.src_w = c.src_h = -1.0f;
    c.dst_x = c.dst_y = -99;
    c.dst_w = c.dst_h = 0;
    if (layout == 1) {
        epi_layout_cover(&c);
    } else {
        epi_layout_fit(&c);
    }
    char msg[160];
    snprintf(msg, sizeof msg, "%s: layout differs from the recorded one", name);
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(f_bits(want->src_x), f_bits(c.src_x), msg);
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(f_bits(want->src_y), f_bits(c.src_y), msg);
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(f_bits(want->src_w), f_bits(c.src_w), msg);
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(f_bits(want->src_h), f_bits(c.src_h), msg);
    TEST_ASSERT_EQUAL_INT32_MESSAGE(want->dst_x, c.dst_x, msg);
    TEST_ASSERT_EQUAL_INT32_MESSAGE(want->dst_y, c.dst_y, msg);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(want->dst_w, c.dst_w, msg);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(want->dst_h, c.dst_h, msg);
}

/*
 * Frozen output hashes, from the implementation checked by
 * test_resampler_reference and the property tests. Update only on an
 * intentional change to the output: run the test and copy the hashes it
 * prints for the changed cases into test_resampler_cases.h.
 */
void test_resampler_goldens(void) {
    unsigned failures = 0;
    char first[200] = "";
    for (unsigned i = 0; i < EPI_RS_CASE_COUNT; i++) {
        const epi_rs_case_t* g = &epi_rs_cases[i];
        const epi_resampler_config_t c = case_cfg(g);
        if (g->layout) {
            check_layout(g->name, g->layout, &c);
        }
        epi_rgb_t* src = new_source(g->src_width, g->src_height, g->seed);
        epi_rgb_t* out = resample(&c, src, i, g->name);
        const uint64_t h = fnv1a64((const uint8_t*)out, (size_t)c.dst_width * c.dst_height * sizeof *out);
        free(out);
        free(src);
        if (h != g->hash) {
            printf("  changed: %u %s UINT64_C(0x%016llx)\n", i, g->name, (unsigned long long)h);
            if (!failures) {
                snprintf(first, sizeof first, "%s", g->name);
            }
            failures++;
        }
    }
    char msg[300];
    snprintf(
        msg,
        sizeof msg,
        "%u of %u cases changed output (new hashes printed above); first: %s",
        failures,
        (unsigned)EPI_RS_CASE_COUNT,
        first
    );
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, failures, msg);
}

/* ---- reference model ---- */

/*
 * A separate model of the algorithm epaper_image.h documents: a separable
 * resample in double precision, no fixed point. Per axis, with the rect
 * starting at x0 and spanning w = (x0 + extent) − x0 (in float, as the API
 * defines the rect) over n outputs: scale = w / n, s = max(1, scale),
 * support = (0.5, 1, 2 or 3)·s. Output i is centred at x0 + (i + 0.5)·scale;
 * its window is source pixels [⌊centre − support + 0.5⌋,
 * ⌊centre + support + 0.5⌋) clipped to the image, pixel j weighing
 * K((j − centre + 0.5)·(1/s)), and the weights are normalised to sum 1 (an
 * empty window, or a zero sum, gives 0). Horizontal pass, then vertical,
 * unrounded in between; the result rounds half up and clamps to 0..255.
 * Kernels: BOX 1 on (−0.5, 0.5]; BILINEAR 1 − |x| on |x| < 1; BICUBIC
 * Keys with a = −0.5 on |x| < 2; LANCZOS3 sinc(x)·sinc(x/3) on |x| < 3
 * (libm sin).
 *
 * The window bounds and the tap distance are written as the library writes
 * them because BOX is discontinuous: where a pixel centre lies exactly on a
 * box edge, the last bit of the distance decides whether a whole tap is in,
 * and evaluating it differently (say (j + 0.5 − centre) / s) moved BOX
 * outputs by up to 128 levels on integer-ratio crops. The other kernels are
 * continuous and zero at their support's edge, so for them the form is
 * immaterial.
 *
 * staged = 1 models the library's two documented 8-bit stages instead: the
 * first pass's result is rounded and clamped to 0..255, and on the tall path
 * (src_height > 100·src_width with a shrinking height) the vertical pass
 * runs first.
 */
static double ref_kernel(int f, double x) {
    const double ax = fabs(x);
    switch (f) {
    case EPI_FILTER_BOX:
        return (x > -0.5 && x <= 0.5) ? 1.0 : 0.0;
    case EPI_FILTER_BILINEAR:
        return ax < 1.0 ? 1.0 - ax : 0.0;
    case EPI_FILTER_BICUBIC: {
        const double a = -0.5;
        if (ax < 1.0) {
            return (a + 2.0) * ax * ax * ax - (a + 3.0) * ax * ax + 1.0;
        }
        if (ax < 2.0) {
            return a * ax * ax * ax - 5.0 * a * ax * ax + 8.0 * a * ax - 4.0 * a;
        }
        return 0.0;
    }
    default: {
        if (ax >= 3.0) {
            return 0.0;
        }
        if (ax == 0.0) {
            return 1.0;
        }
        const double pi = 3.14159265358979323846;
        return (sin(pi * x) / (pi * x)) * (sin(pi * x / 3.0) / (pi * x / 3.0));
    }
    }
}

static const double ref_support[4] = {0.5, 1.0, 2.0, 3.0};

/* Weights of output i over source pixels j0[i] <= j < j1[i], stored from
 * w + off[i]. Sparse: a dense out × in matrix of doubles does not fit an
 * ESP32's heap on the tall cases. */
typedef struct {
    uint32_t *j0, *j1;
    size_t* off;
    double* w;
} ref_weights_t;

static void ref_free(ref_weights_t* W) {
    free(W->w);
    free(W->off);
    free(W->j1);
    free(W->j0);
}

static ref_weights_t ref_axis(uint32_t in, float x0, float extent, uint32_t out, int f) {
    ref_weights_t W = {calloc(out, sizeof(uint32_t)), calloc(out, sizeof(uint32_t)), calloc(out, sizeof(size_t)), NULL};
    TEST_ASSERT_NOT_NULL(W.j0);
    TEST_ASSERT_NOT_NULL(W.j1);
    TEST_ASSERT_NOT_NULL(W.off);
    const float x1 = x0 + extent;
    const double scale = (double)(x1 - x0) / (double)out;
    const double s = scale > 1.0 ? scale : 1.0;
    const double support = ref_support[f] * s;
    size_t total = 0;
    for (uint32_t i = 0; i < out; i++) {
        const double centre = (double)x0 + ((double)i + 0.5) * scale;
        const double lo = floor(centre - support + 0.5), hi = floor(centre + support + 0.5);
        W.j0[i] = lo < 0.0 ? 0u : (lo > (double)in ? in : (uint32_t)lo);
        W.j1[i] = hi < (double)W.j0[i] ? W.j0[i] : (hi > (double)in ? in : (uint32_t)hi);
        W.off[i] = total;
        total += W.j1[i] - W.j0[i];
    }
    W.w = calloc(total ? total : 1, sizeof *W.w);
    TEST_ASSERT_NOT_NULL(W.w);
    for (uint32_t i = 0; i < out; i++) {
        const double centre = (double)x0 + ((double)i + 0.5) * scale;
        double* row = W.w + W.off[i];
        double sum = 0.0;
        for (uint32_t j = W.j0[i]; j < W.j1[i]; j++) {
            row[j - W.j0[i]] = ref_kernel(f, ((double)j - centre + 0.5) * (1.0 / s));
            sum += row[j - W.j0[i]];
        }
        for (uint32_t j = W.j0[i]; j < W.j1[i]; j++) {
            row[j - W.j0[i]] = sum != 0.0 ? row[j - W.j0[i]] / sum : 0.0;
        }
    }
    return W;
}

static double ref_q8(double v) {
    return v <= 0.0 ? 0.0 : (v >= 255.0 ? 255.0 : floor(v + 0.5));
}

/* The dst_width × dst_height reference output (caller frees). With staged
 * and clamped non-NULL, clamped[3·pixel + channel] is set where the output
 * reads a first-pass value that the 8-bit clamp changed. */
static epi_rgb_t* ref_resample(const epi_resampler_config_t* c, const epi_rgb_t* src, int staged, uint8_t* clamped) {
    const uint32_t sw = c->src_width, sh = c->src_height, dw = c->dst_w, dh = c->dst_h;
    ref_weights_t Wh = ref_axis(sw, c->src_x, c->src_w, dw, (int)c->filter);
    ref_weights_t Wv = ref_axis(sh, c->src_y, c->src_h, dh, (int)c->filter);
    const int v_first = staged && sh > 100u * sw && dh < sh;
    /* First pass into T (rows × cols), second into O (dh × dw). */
    const uint32_t trows = v_first ? dh : sh, tcols = v_first ? sw : dw;
    double* T = calloc((size_t)trows * tcols * 3, sizeof *T);
    double* O = calloc((size_t)dh * dw * 3, sizeof *O);
    uint8_t* Tc = calloc((size_t)trows * tcols * 3, 1);
    uint8_t* Oc = calloc((size_t)dh * dw * 3, 1);
    TEST_ASSERT_NOT_NULL(T);
    TEST_ASSERT_NOT_NULL(O);
    TEST_ASSERT_NOT_NULL(Tc);
    TEST_ASSERT_NOT_NULL(Oc);
    for (uint32_t r = 0; r < trows; r++) {
        for (uint32_t q = 0; q < tcols; q++) {
            double a[3] = {0.0, 0.0, 0.0};
            const ref_weights_t* A = v_first ? &Wv : &Wh;
            const uint32_t i = v_first ? r : q;
            for (uint32_t t = A->j0[i]; t < A->j1[i]; t++) {
                const double k = A->w[A->off[i] + (t - A->j0[i])];
                if (k != 0.0) {
                    const epi_rgb_t p = v_first ? src[(size_t)t * sw + q] : src[(size_t)r * sw + t];
                    a[0] += k * p.r;
                    a[1] += k * p.g;
                    a[2] += k * p.b;
                }
            }
            for (int ch = 0; ch < 3; ch++) {
                const size_t at = ((size_t)r * tcols + q) * 3 + ch;
                T[at] = staged ? ref_q8(a[ch]) : a[ch];
                Tc[at] = (uint8_t)(staged && (a[ch] < -0.5 || a[ch] >= 255.5));
            }
        }
    }
    for (uint32_t y = 0; y < dh; y++) {
        for (uint32_t x = 0; x < dw; x++) {
            double a[3] = {0.0, 0.0, 0.0};
            const ref_weights_t* A = v_first ? &Wh : &Wv;
            const uint32_t i = v_first ? x : y;
            for (uint32_t t = A->j0[i]; t < A->j1[i]; t++) {
                const double k = A->w[A->off[i] + (t - A->j0[i])];
                if (k != 0.0) {
                    const size_t at = v_first ? ((size_t)y * tcols + t) * 3 : ((size_t)t * tcols + x) * 3;
                    for (int ch = 0; ch < 3; ch++) {
                        a[ch] += k * T[at + ch];
                        Oc[((size_t)y * dw + x) * 3 + ch] |= Tc[at + ch];
                    }
                }
            }
            for (int ch = 0; ch < 3; ch++) {
                O[((size_t)y * dw + x) * 3 + ch] = a[ch];
            }
        }
    }
    epi_rgb_t* out = malloc((size_t)c->dst_width * c->dst_height * sizeof *out);
    TEST_ASSERT_NOT_NULL(out);
    for (uint32_t Y = 0; Y < c->dst_height; Y++) {
        for (uint32_t X = 0; X < c->dst_width; X++) {
            const int64_t px = (int64_t)X - c->dst_x, py = (int64_t)Y - c->dst_y;
            const size_t at = (size_t)Y * c->dst_width + X;
            epi_rgb_t v = c->background;
            if (px >= 0 && px < dw && py >= 0 && py < dh) {
                const size_t o = ((size_t)py * dw + (size_t)px) * 3;
                v = (epi_rgb_t){(uint8_t)ref_q8(O[o]), (uint8_t)ref_q8(O[o + 1]), (uint8_t)ref_q8(O[o + 2])};
                if (clamped) {
                    memcpy(clamped + at * 3, Oc + o, 3);
                }
            } else if (clamped) {
                memset(clamped + at * 3, 0, 3);
            }
            out[at] = v;
        }
    }
    free(Oc);
    free(Tc);
    free(O);
    free(T);
    ref_free(&Wv);
    ref_free(&Wh);
    return out;
}

/* A random valid config: sizes from 1 (odd ones too), scales from 1/4 to 4
 * per axis independently, fractional rects, overhanging placements, and
 * one in eight on the tall path. */
static epi_resampler_config_t random_cfg(uint64_t* s) {
    for (;;) {
        uint16_t sw, sh;
        const int tall = rnd(s, 8) == 0;
        if (tall) {
            sw = (uint16_t)(1 + rnd(s, 2));
            sh = (uint16_t)(100u * sw + 1 + rnd(s, 120));
        } else {
            const uint32_t k = rnd(s, 10);
            sw = (uint16_t)(k == 0 ? 1 : k == 1 ? 2 : 3 + rnd(s, 46));
            const uint32_t m = rnd(s, 10);
            sh = (uint16_t)(m == 0 ? 1 : m == 1 ? 2 : 3 + rnd(s, 46));
        }
        epi_resampler_config_t c = base_cfg(sw, sh, 1, 1, (epi_filter_t)rnd(s, 4));
        if (rnd(s, 2)) {
            c.src_w = (float)(sw * (0.05 + 0.95 * rndf(s)));
            c.src_x = (float)((sw - c.src_w) * rndf(s));
        }
        if (rnd(s, 2)) {
            c.src_h = (float)(sh * (0.05 + 0.95 * rndf(s)));
            c.src_y = (float)((sh - c.src_h) * rndf(s));
        }
        double dw = c.src_w * exp2(4.0 * rndf(s) - 2.0);
        double dh = c.src_h * exp2(4.0 * rndf(s) - 2.0);
        if (rnd(s, 8) == 0) {
            dw = c.src_w; /* same size */
        }
        if (tall) {
            dh = 1 + rnd(s, sh - 1u);
        }
        c.dst_w = (uint16_t)(dw < 1.0 ? 1.0 : (dw > 96.0 ? 96.0 : floor(dw + 0.5)));
        c.dst_h = (uint16_t)(dh < 1.0 ? 1.0 : (dh > 96.0 ? 96.0 : floor(dh + 0.5)));
        c.dst_width = c.dst_w;
        c.dst_height = c.dst_h;
        if (rnd(s, 4) == 0) {
            const int32_t ow = (int32_t)c.dst_w + (int32_t)rnd(s, 9) - 4;
            const int32_t oh = (int32_t)c.dst_h + (int32_t)rnd(s, 9) - 4;
            c.dst_width = (uint16_t)(ow < 1 ? 1 : ow);
            c.dst_height = (uint16_t)(oh < 1 ? 1 : oh);
            c.dst_x = (int32_t)rnd(s, 9) - 4;
            c.dst_y = (int32_t)rnd(s, 9) - 4;
        }
        c.background = (epi_rgb_t){(uint8_t)rnd(s, 256), (uint8_t)rnd(s, 256), (uint8_t)rnd(s, 256)};
        if (epi_resampler_workspace_size(&c) != 0) {
            return c;
        }
    }
}

#ifndef EPI_TEST_EXHAUSTIVE
#define EPI_TEST_EXHAUSTIVE 1
#endif
#if EPI_TEST_EXHAUSTIVE
#define EPI_RS_RANDOM_CASES 600
#else
#define EPI_RS_RANDOM_CASES 120 /* emulated targets */
#endif

/*
 * Tolerances, per channel. Measured maxima over this file's cases (480
 * regression + EPI_RS_RANDOM_CASES random): 1, 55 and 2.
 *
 * - Against the pure reference, ±1 wherever the library's 8-bit clamp
 *   between passes leaves every first-pass value the output reads
 *   unchanged: what remains is fixed-point weights and two roundings.
 * - Where the clamp changed one (BICUBIC and LANCZOS3 only, whose negative
 *   lobes overshoot 0..255 at hard edges, e.g. the checkerboard region),
 *   the measured maximum: clipping an overshoot removes it from the second
 *   pass, which the pure reference keeps.
 * - Against the staged reference, which models that clamp (and the
 *   tall-path order), ±2: it rounds the first pass exactly where the
 *   library uses 22-bit weights, so a value on a rounding boundary can land
 *   one level apart before the second pass adds its own ±1.
 */
#define EPI_RS_TOL_PURE 1
#define EPI_RS_TOL_CLAMPED 55
#define EPI_RS_TOL_STAGED 2

typedef struct {
    unsigned dev[3]; /* max |library − reference|: pure unclamped, pure clamped, staged */
    char at[3][120];
    unsigned checked, skipped;
} ref_stats_t;

/* Heap one ref_check needs, roughly: the four output-sized buffers and both
 * references' double first and second passes. Without PSRAM an ESP32 has
 * about 290 KB of heap in pieces of at most about 110 KB, so the emulated
 * targets check only the cases that fit; the regression hashes, which do
 * run there, already show the output is byte-identical to the host's. */
#if !EPI_TEST_EXHAUSTIVE
#define EPI_RS_REF_BUDGET 96000u
static size_t ref_bytes(const epi_resampler_config_t* c) {
    const size_t n = (size_t)c->dst_width * (c->dst_height + 1u);
    const size_t o = (size_t)c->dst_w * c->dst_h;
    const size_t t1 = (size_t)c->src_height * c->dst_w, t2 = (size_t)c->dst_h * c->src_width;
    return n * 12u + (o + (t1 > t2 ? t1 : t2)) * 27u;
}
#endif

static void ref_note(ref_stats_t* st, int k, unsigned d, const char* name, size_t px, uint32_t w) {
    if (d > st->dev[k]) {
        st->dev[k] = d;
        snprintf(st->at[k], sizeof st->at[k], "%s (%u, %u)", name, (unsigned)(px % w), (unsigned)(px / w));
    }
}

static void
ref_check(const epi_resampler_config_t* c, const epi_rgb_t* src, unsigned misalign, const char* name, ref_stats_t* st) {
#if !EPI_TEST_EXHAUSTIVE
    if (ref_bytes(c) > EPI_RS_REF_BUDGET) {
        st->skipped++;
        return;
    }
#endif
    st->checked++;
    const size_t n = (size_t)c->dst_width * c->dst_height;
    uint8_t* clamped = malloc(n * 3);
    TEST_ASSERT_NOT_NULL(clamped);
    epi_rgb_t* got = resample(c, src, misalign, name);
    epi_rgb_t* pure = ref_resample(c, src, 0, NULL);
    epi_rgb_t* staged = ref_resample(c, src, 1, clamped);
    for (size_t i = 0; i < n; i++) {
        const int g[3] = {got[i].r, got[i].g, got[i].b};
        const int p[3] = {pure[i].r, pure[i].g, pure[i].b};
        const int s[3] = {staged[i].r, staged[i].g, staged[i].b};
        for (int ch = 0; ch < 3; ch++) {
            ref_note(st, clamped[i * 3 + ch] ? 1 : 0, (unsigned)abs(g[ch] - p[ch]), name, i, c->dst_width);
            ref_note(st, 2, (unsigned)abs(g[ch] - s[ch]), name, i, c->dst_width);
        }
    }
    free(staged);
    free(pure);
    free(got);
    free(clamped);
}

void test_resampler_reference(void) {
    ref_stats_t st;
    memset(&st, 0, sizeof st);
    for (unsigned i = 0; i < EPI_RS_CASE_COUNT; i++) {
        const epi_rs_case_t* g = &epi_rs_cases[i];
        const epi_resampler_config_t c = case_cfg(g);
        epi_rgb_t* src = new_source(g->src_width, g->src_height, g->seed);
        ref_check(&c, src, i, g->name, &st);
        free(src);
    }
    uint64_t s = UINT64_C(0x5DEECE66D2545F49);
    for (unsigned i = 0; i < EPI_RS_RANDOM_CASES; i++) {
        const epi_resampler_config_t c = random_cfg(&s);
        epi_rgb_t* src = new_source(c.src_width, c.src_height, (uint32_t)(xorshift64(&s) | 1u));
        char name[48];
        snprintf(name, sizeof name, "random %u", i);
        ref_check(&c, src, i, name, &st);
        free(src);
    }
    printf(
        "  reference: %u cases (%u over the heap budget skipped); max deviation %u unclamped (%s), "
        "%u clamped (%s); staged %u (%s)\n",
        st.checked,
        st.skipped,
        st.dev[0],
        st.at[0],
        st.dev[1],
        st.at[1],
        st.dev[2],
        st.at[2]
    );
    TEST_ASSERT_GREATER_THAN_UINT(0, st.checked);
    TEST_ASSERT_LESS_OR_EQUAL_UINT(EPI_RS_TOL_PURE, st.dev[0]);
    TEST_ASSERT_LESS_OR_EQUAL_UINT(EPI_RS_TOL_CLAMPED, st.dev[1]);
    TEST_ASSERT_LESS_OR_EQUAL_UINT(EPI_RS_TOL_STAGED, st.dev[2]);
}

/* ---- properties ---- */

/*
 * A flat source gives exactly its colour over the placement (and the
 * background around it), for every filter over random sizes, scales, rects
 * and placements: each window's fixed-point weights sum to 1 within far
 * less than half a level, and a flat first pass never overshoots.
 */
void test_resampler_flat(void) {
    static const epi_rgb_t colours[] = {{0, 0, 0}, {255, 255, 255}, {255, 0, 97}, {1, 254, 128}};
    uint64_t s = UINT64_C(0x2545F4914F6CDD1D);
    unsigned bad = 0;
    for (unsigned i = 0; i < 400; i++) {
        epi_resampler_config_t c = random_cfg(&s);
        c.filter = (epi_filter_t)(i % 4);
        const epi_rgb_t k = colours[(i / 4) % 4];
        c.background = (epi_rgb_t){(uint8_t)~k.r, (uint8_t)~k.g, (uint8_t)~k.b};
        epi_rgb_t* src = malloc((size_t)c.src_width * c.src_height * sizeof *src);
        TEST_ASSERT_NOT_NULL(src);
        for (size_t p = 0; p < (size_t)c.src_width * c.src_height; p++) {
            src[p] = k;
        }
        char name[48];
        snprintf(name, sizeof name, "flat %u", i);
        epi_rgb_t* out = resample(&c, src, i, name);
        unsigned case_bad = 0;
        for (uint32_t Y = 0; Y < c.dst_height; Y++) {
            for (uint32_t X = 0; X < c.dst_width; X++) {
                const int64_t px = (int64_t)X - c.dst_x, py = (int64_t)Y - c.dst_y;
                const int in = px >= 0 && px < c.dst_w && py >= 0 && py < c.dst_h;
                const epi_rgb_t want = in ? k : c.background;
                const epi_rgb_t got = out[(size_t)Y * c.dst_width + X];
                case_bad += got.r != want.r || got.g != want.g || got.b != want.b;
            }
        }
        if (case_bad) {
            printf(
                "  flat %u (filter %d, %ux%u -> %ux%u): %u pixels\n",
                i,
                (int)c.filter,
                (unsigned)c.src_width,
                (unsigned)c.src_height,
                c.dst_w,
                c.dst_h,
                case_bad
            );
            bad++;
        }
        free(out);
        free(src);
    }
    TEST_ASSERT_EQUAL_UINT(0, bad);
}

/* Mirror-symmetric (left-right and top-bottom) source of w × h. */
static epi_rgb_t* mirror_source(uint32_t w, uint32_t h, uint32_t seed) {
    epi_rgb_t* base = new_source(w, h, seed);
    epi_rgb_t* img = malloc((size_t)w * h * sizeof *img);
    TEST_ASSERT_NOT_NULL(img);
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            const uint32_t bx = x < w - 1 - x ? x : w - 1 - x, by = y < h - 1 - y ? y : h - 1 - y;
            img[(size_t)y * w + x] = base[(size_t)by * w + bx];
        }
    }
    free(base);
    return img;
}

/* Max |out − mirror(out)| over both axes. */
static unsigned mirror_dev(const epi_rgb_t* out, uint32_t w, uint32_t h) {
    unsigned m = 0;
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            const epi_rgb_t a = out[(size_t)y * w + x];
            const epi_rgb_t b = out[(size_t)y * w + (w - 1 - x)];
            const epi_rgb_t c = out[(size_t)(h - 1 - y) * w + x];
            const int d[6] = {a.r - b.r, a.g - b.g, a.b - b.b, a.r - c.r, a.g - c.g, a.b - c.b};
            for (int k = 0; k < 6; k++) {
                const unsigned e = (unsigned)abs(d[k]);
                m = e > m ? e : m;
            }
        }
    }
    return m;
}

/*
 * A source symmetric left-right and top-bottom, a centred rect and a
 * centred placement give a symmetric output, exactly. BILINEAR, BICUBIC
 * and LANCZOS3 on arbitrary centred rects (in 1/8 pixels, so x0 + x1 is
 * exactly the source width) and output sizes. BOX's kernel is half-open,
 * (−0.5, 0.5], so a pixel centre exactly on a box edge falls on one side
 * only and breaks symmetry by design: BOX runs on tie-free configs (integer
 * rect offsets, integer or 1/k scale factors).
 */
void test_resampler_mirror(void) {
    uint64_t s = UINT64_C(0x9E3779B97F4A7C15);
    unsigned bad = 0;
    for (unsigned i = 0; i < 400; i++) {
        const uint16_t sw = (uint16_t)(1 + rnd(&s, 48)), sh = (uint16_t)(1 + rnd(&s, 48));
        epi_resampler_config_t c = base_cfg(sw, sh, 1, 1, (epi_filter_t)(i % 4));
        if (c.filter == EPI_FILTER_BOX) {
            const uint32_t kx = rnd(&s, (sw + 1u) / 2u), ky = rnd(&s, (sh + 1u) / 2u);
            const uint32_t w = sw - 2u * kx, h = sh - 2u * ky;
            const uint32_t fx = 1 + rnd(&s, 4), fy = 1 + rnd(&s, 4);
            c.src_x = (float)kx;
            c.src_y = (float)ky;
            c.src_w = (float)w;
            c.src_h = (float)h;
            c.dst_w = (uint16_t)(rnd(&s, 2) ? w * fx : (w % fx ? w : w / fx));
            c.dst_h = (uint16_t)(rnd(&s, 2) ? h * fy : (h % fy ? h : h / fy));
        } else {
            c.src_w = (float)(1 + rnd(&s, 8u * sw)) / 8.0f;
            c.src_h = (float)(1 + rnd(&s, 8u * sh)) / 8.0f;
            c.src_x = ((float)sw - c.src_w) / 2.0f;
            c.src_y = ((float)sh - c.src_h) / 2.0f;
            c.dst_w = (uint16_t)(1 + rnd(&s, 64));
            c.dst_h = (uint16_t)(1 + rnd(&s, 64));
        }
        const uint32_t mx = rnd(&s, 3), my = rnd(&s, 3); /* symmetric margins */
        c.dst_width = (uint16_t)(c.dst_w + 2u * mx);
        c.dst_height = (uint16_t)(c.dst_h + 2u * my);
        c.dst_x = (int32_t)mx;
        c.dst_y = (int32_t)my;
        epi_rgb_t* src = mirror_source(sw, sh, (uint32_t)(xorshift64(&s) | 1u));
        char name[48];
        snprintf(name, sizeof name, "mirror %u", i);
        epi_rgb_t* out = resample(&c, src, i, name);
        if (mirror_dev(out, c.dst_width, c.dst_height) != 0) {
            printf(
                "  %s: filter %d, %ux%u [%g %g %g %g] -> %ux%u not symmetric\n",
                name,
                (int)c.filter,
                sw,
                sh,
                c.src_x,
                c.src_y,
                c.src_w,
                c.src_h,
                c.dst_w,
                c.dst_h
            );
            bad++;
        }
        free(out);
        free(src);
    }
    TEST_ASSERT_EQUAL_UINT(0, bad);
}

/*
 * Same size and uncropped is an exact copy for every filter (a pass whose
 * scale is 1 at offset 0 is skipped), and so is a crop at integer offsets
 * whose size is the output's.
 */
void test_resampler_identity(void) {
    static const uint16_t sizes[][2] = {{1, 1}, {1, 9}, {9, 1}, {2, 3}, {37, 23}, {64, 48}, {3, 301}};
    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        const uint16_t w = sizes[i][0], h = sizes[i][1];
        epi_rgb_t* src = new_source(w, h, 0x1234567u + i);
        for (int f = EPI_FILTER_BOX; f <= EPI_FILTER_LANCZOS3; f++) {
            epi_resampler_config_t c = base_cfg(w, h, w, h, (epi_filter_t)f);
            epi_rgb_t* out = resample(&c, src, (unsigned)f, "identity");
            TEST_ASSERT_EQUAL_MEMORY(src, out, (size_t)w * h * sizeof *src);
            free(out);

            const uint16_t kx = w >= 3 ? 1 : 0, ky = h >= 3 ? 1 : 0;
            const uint16_t cw = (uint16_t)(w - 2 * kx), ch = (uint16_t)(h - 2 * ky);
            c = base_cfg(w, h, cw, ch, (epi_filter_t)f);
            c.src_x = kx;
            c.src_y = ky;
            c.src_w = cw;
            c.src_h = ch;
            out = resample(&c, src, (unsigned)f, "crop");
            for (uint32_t y = 0; y < ch; y++) {
                TEST_ASSERT_EQUAL_MEMORY(
                    src + (size_t)(y + ky) * w + kx, out + (size_t)y * cw, (size_t)cw * sizeof *src
                );
            }
            free(out);
        }
        free(src);
    }
}

/* ---- validation and workspace ---- */

static void expect_invalid(const epi_resampler_config_t* c) {
    static uint64_t buf[4096];
    epi_resampler_t* r = NULL;
    TEST_ASSERT_TRUE(epi_resampler_workspace_size(c) == 0);
    TEST_ASSERT_EQUAL(EPI_ERR_INVALID_ARG, epi_resampler_init(c, buf, sizeof buf, &r));
    TEST_ASSERT_NULL(r);
}

void test_resampler_validation(void) {
    const epi_resampler_config_t ok = base_cfg(20, 10, 8, 6, EPI_FILTER_LANCZOS3);
    TEST_ASSERT_NOT_EQUAL(0, epi_resampler_workspace_size(&ok));
    expect_invalid(NULL);
    epi_resampler_config_t c;
    c = ok;
    c.src_width = 0;
    expect_invalid(&c);
    c = ok;
    c.src_height = 0;
    expect_invalid(&c);
    c = ok;
    c.dst_width = 0;
    expect_invalid(&c);
    c = ok;
    c.dst_height = 0;
    expect_invalid(&c);
    c = ok;
    c.dst_w = 0;
    expect_invalid(&c);
    c = ok;
    c.dst_h = 0;
    expect_invalid(&c);
    c = ok;
    c.filter = (epi_filter_t)4;
    expect_invalid(&c);
    c = ok;
    c.filter = (epi_filter_t)-1;
    expect_invalid(&c);
    c = ok;
    c.src_x = -0.25f;
    c.src_w = 1.0f;
    expect_invalid(&c);
    c = ok;
    c.src_y = -1e-30f;
    c.src_h = 1.0f;
    expect_invalid(&c);
    c = ok;
    c.src_w = 0.0f;
    expect_invalid(&c);
    c = ok;
    c.src_h = -2.0f;
    expect_invalid(&c);
    c = ok;
    c.src_x = 0.5f;
    expect_invalid(&c); /* x1 = 20.5 > 20 */
    c = ok;
    c.src_y = 1.0f;
    c.src_h = 9.0001f;
    expect_invalid(&c);
    c = ok;
    c.src_w = 20.000002f;
    expect_invalid(&c);
    c = ok;
    c.src_x = NAN;
    expect_invalid(&c);
    c = ok;
    c.src_w = INFINITY;
    expect_invalid(&c);
    c = ok;
    c.src_h = NAN;
    expect_invalid(&c);
    c = ok;
    c.src_y = -INFINITY;
    expect_invalid(&c);
    /* Boundaries that are fine: a box touching the far edges, a sliver. */
    c = ok;
    c.src_x = 19.0f;
    c.src_w = 1.0f;
    TEST_ASSERT_NOT_EQUAL(0, epi_resampler_workspace_size(&c));
    c = ok;
    c.src_y = 9.999f;
    c.src_h = 0.001f;
    TEST_ASSERT_NOT_EQUAL(0, epi_resampler_workspace_size(&c));
    /* Placement may lie anywhere, including entirely outside the output. */
    c = ok;
    c.dst_x = INT32_MIN;
    c.dst_y = INT32_MAX;
    TEST_ASSERT_NOT_EQUAL(0, epi_resampler_workspace_size(&c));

    /* init: NULL arguments, too small, every misalignment. */
    size_t need = epi_resampler_workspace_size(&ok);
    uint8_t* mem = malloc(need + 8);
    TEST_ASSERT_NOT_NULL(mem);
    epi_resampler_t* r = NULL;
    TEST_ASSERT_EQUAL(EPI_ERR_INVALID_ARG, epi_resampler_init(&ok, mem, need, NULL));
    TEST_ASSERT_EQUAL(EPI_ERR_INVALID_ARG, epi_resampler_init(&ok, NULL, need, &r));
    {
        /* *out is NULL after a failed init. */
        epi_resampler_config_t zero = ok;
        zero.dst_width = 0;
        r = (epi_resampler_t*)(void*)mem; /* non-NULL sentinel */
        TEST_ASSERT_EQUAL(EPI_ERR_INVALID_ARG, epi_resampler_init(&zero, mem, need, &r));
        TEST_ASSERT_NULL(r);
    }
    TEST_ASSERT_EQUAL(EPI_ERR_WORKSPACE_TOO_SMALL, epi_resampler_init(&ok, mem, need - 1, &r));
    TEST_ASSERT_EQUAL(EPI_ERR_WORKSPACE_TOO_SMALL, epi_resampler_init(&ok, mem, 0, &r));
    for (unsigned m = 0; m < 8; m++) {
        r = NULL;
        TEST_ASSERT_EQUAL(EPI_OK, epi_resampler_init(&ok, mem + m, need, &r));
        TEST_ASSERT_TRUE((uint8_t*)r >= mem + m && (uint8_t*)r < mem + m + need);
    }
    /* NULL handles / buffers: invalid argument. */
    epi_rgb_t px[20];
    TEST_ASSERT_EQUAL_INT(EPI_ERR_INVALID_ARG, epi_resampler_push(NULL, px));
    TEST_ASSERT_EQUAL_INT(EPI_ERR_INVALID_ARG, epi_resampler_push(NULL, NULL));
    TEST_ASSERT_EQUAL_INT(EPI_ERR_INVALID_ARG, epi_resampler_push(r, NULL));
    TEST_ASSERT_EQUAL_INT(EPI_ERR_INVALID_ARG, epi_resampler_pop(NULL, px));
    TEST_ASSERT_EQUAL_INT(EPI_ERR_INVALID_ARG, epi_resampler_pop(r, NULL));
    free(mem);
}

/* ---- source size limit ---- */

static epi_resampler_config_t sized_cfg(uint32_t sw, uint32_t sh, uint32_t limit) {
    epi_resampler_config_t c = base_cfg(1, 1, 1200, 1600, EPI_FILTER_BOX);
    c.src_width = sw;
    c.src_height = sh;
    c.max_src_pixels = limit;
    epi_layout_cover(&c);
    return c;
}

static void expect_too_large(const epi_resampler_config_t* c) {
    uint64_t buf[8];
    epi_resampler_t* r = (epi_resampler_t*)(void*)buf; /* non-NULL sentinel */
    TEST_ASSERT_TRUE(epi_resampler_workspace_size(c) == 0);
    TEST_ASSERT_EQUAL(EPI_ERR_SOURCE_TOO_LARGE, epi_resampler_init(c, buf, sizeof buf, &r));
    TEST_ASSERT_NULL(r);
}

static void expect_sized(const epi_resampler_config_t* c) {
    TEST_ASSERT_NOT_EQUAL(0, epi_resampler_workspace_size(c));
}

void test_resampler_source_limit(void) {
    epi_resampler_config_t c;
    /* Default limit: 100 megapixels. */
    c = sized_cfg(10000, 10000, 0);
    expect_sized(&c);
    c = sized_cfg(10000, 10001, 0);
    expect_too_large(&c);
    c = sized_cfg(10001, 10000, 0);
    expect_too_large(&c);
    /* An explicit limit: the area itself passes, one more fails. */
    c = sized_cfg(300, 200, 60000);
    expect_sized(&c);
    c = sized_cfg(300, 200, 59999);
    expect_too_large(&c);
    c = sized_cfg(1, 1, 1);
    expect_sized(&c);
    c = sized_cfg(2, 1, 1);
    expect_too_large(&c);
    /* UINT32_MAX allows every size up to 65535 × 65535, and no more. */
    c = sized_cfg(65535, 65535, UINT32_MAX);
    expect_sized(&c);
    c = sized_cfg(65535, 65535, 0);
    expect_too_large(&c);
    c = sized_cfg(65536, 1, UINT32_MAX);
    expect_too_large(&c);
    c = sized_cfg(1, 65536, UINT32_MAX);
    expect_too_large(&c);
    c = sized_cfg(70000, 70000, UINT32_MAX);
    expect_too_large(&c);
    c = sized_cfg(UINT32_MAX, 1, UINT32_MAX);
    expect_too_large(&c);
    c = sized_cfg(1, UINT32_MAX, UINT32_MAX);
    expect_too_large(&c);
    c = sized_cfg((uint32_t)-1, (uint32_t)-1, UINT32_MAX);
    expect_too_large(&c);
    c = sized_cfg(65536, 65536, UINT32_MAX);
    expect_too_large(&c); /* 2^32 wraps to 0 */
    /* A side of 0 is invalid, not too large. */
    c = sized_cfg(1, 1, 0);
    c.src_width = 0;
    expect_invalid(&c);
    c.src_width = 1;
    c.src_height = 0;
    expect_invalid(&c);
    c.src_width = 0;
    c.src_height = 70000;
    expect_invalid(&c);
    /* The layout helpers are defined for any size; the resampler rejects it. */
    static const uint32_t big[] = {65536, 70000, UINT32_MAX};
    for (unsigned i = 0; i < sizeof big / sizeof big[0]; i++) {
        c = base_cfg(1, 1, 1200, 1600, EPI_FILTER_BOX);
        c.src_width = big[i];
        c.src_height = 3000;
        epi_layout_cover(&c);
        expect_too_large(&c);
        c = base_cfg(1, 1, 1200, 1600, EPI_FILTER_BOX);
        c.src_width = 3000;
        c.src_height = big[i];
        epi_layout_fit(&c);
        expect_too_large(&c);
    }
}

void test_resampler_workspace(void) {
    /* Grows with the filter support, and stays small for a typical cover. */
    size_t prev = 0;
    for (int f = EPI_FILTER_BOX; f <= EPI_FILTER_LANCZOS3; f++) {
        epi_resampler_config_t c = base_cfg(4000, 3000, 1200, 1600, (epi_filter_t)f);
        epi_layout_cover(&c);
        size_t s = epi_resampler_workspace_size(&c);
        printf("  workspace 4000x3000 -> 1200x1600 cover, filter %d: %zu bytes\n", f, s);
        TEST_ASSERT_TRUE(s > prev);
        TEST_ASSERT_TRUE(s < 200000u);
        prev = s;
    }
    /* Only the visible part of an overhanging placement costs memory. */
    epi_resampler_config_t a = base_cfg(400, 300, 100, 80, EPI_FILTER_BICUBIC);
    epi_resampler_config_t b = a;
    b.dst_w = 4000;
    b.dst_h = 3000;
    b.dst_x = -1950;
    b.dst_y = -1460;
    size_t sa = epi_resampler_workspace_size(&a), sb = epi_resampler_workspace_size(&b);
    TEST_ASSERT_TRUE(sb < 2 * sa + 4096);
    /* Copy passes (identity) need one ring row and no coefficient table. */
    epi_resampler_config_t id = base_cfg(1000, 10, 1000, 10, EPI_FILTER_LANCZOS3);
    TEST_ASSERT_TRUE(epi_resampler_workspace_size(&id) < 1000 * 3 + 512);
}

/* ---- streaming: row counts and the draining contract ---- */

/* Pushes rows one at a time and records how many rows pop after each push
 * (index 0: before any push). Returns the total. */
static uint32_t pop_profile(const epi_resampler_config_t* c, uint32_t* counts) {
    ws_t w;
    epi_resampler_t* r = make(c, &w, 3);
    epi_rgb_t* src = calloc(c->src_width, sizeof *src);
    epi_rgb_t* row = calloc(c->dst_width, sizeof *row);
    TEST_ASSERT_NOT_NULL(src);
    TEST_ASSERT_NOT_NULL(row);
    uint32_t total = 0;
    for (uint32_t y = 0; y <= c->src_height; y++) {
        if (y > 0) {
            /* Accepted, including rows outside the window (counted and skipped). */
            TEST_ASSERT_EQUAL_INT(EPI_OK, epi_resampler_push(r, src));
        }
        uint32_t n = 0;
        while (epi_resampler_pop(r, row) == EPI_OK) {
            n++;
        }
        counts[y] = n;
        total += n;
        /* Not ready until the next push, or finished once every row is out. */
        TEST_ASSERT_EQUAL_INT(total == c->dst_height ? EPI_ERR_FINISHED : EPI_ERR_NOT_READY, epi_resampler_pop(r, row));
    }
    /* Further pushes are finished and release nothing. */
    TEST_ASSERT_EQUAL_INT(EPI_ERR_FINISHED, epi_resampler_push(r, src));
    TEST_ASSERT_EQUAL_INT(EPI_ERR_FINISHED, epi_resampler_pop(r, row));
    free(row);
    free(src);
    free(w.mem);
    return total;
}

static void expect_profile(const epi_resampler_config_t* c, const uint32_t* want) {
    uint32_t got[64];
    TEST_ASSERT_TRUE(c->src_height < 64);
    TEST_ASSERT_EQUAL_UINT32(c->dst_height, pop_profile(c, got));
    TEST_ASSERT_EQUAL_UINT32_ARRAY(want, got, c->src_height + 1u);
}

void test_resampler_streaming(void) {
    /* Identity: one row out per row in. */
    epi_resampler_config_t c = base_cfg(5, 4, 5, 4, EPI_FILTER_LANCZOS3);
    expect_profile(&c, (const uint32_t[]){0, 1, 1, 1, 1});

    /* BOX 2× vertical reduction: row i's window is source rows 2i, 2i + 1. */
    c = base_cfg(4, 8, 4, 4, EPI_FILTER_BOX);
    expect_profile(&c, (const uint32_t[]){0, 0, 1, 0, 1, 0, 1, 0, 1});

    /* BILINEAR 2× upscale of 3 rows: support 1 around centres 0.25, 0.75,
     * 1.25, ...: windows [0,1) [0,2) [0,2) [1,3) [1,3) [2,3). */
    c = base_cfg(4, 3, 4, 6, EPI_FILTER_BILINEAR);
    expect_profile(&c, (const uint32_t[]){0, 1, 2, 3});

    /* Placement at y = 3 on a 10-row canvas: three background rows pop
     * before any push, the four below follow the last placement row. */
    c = base_cfg(4, 4, 4, 10, EPI_FILTER_BICUBIC);
    c.dst_h = 4;
    c.dst_y = 3;
    expect_profile(&c, (const uint32_t[]){3, 1, 1, 1, 4});

    /* Placement entirely off the canvas: everything pops before any push. */
    c = base_cfg(4, 4, 6, 5, EPI_FILTER_BICUBIC);
    c.dst_x = 6;
    expect_profile(&c, (const uint32_t[]){5, 0, 0, 0, 0});

    /* Crop (integer box of the output's size): rows outside are skipped. */
    c = base_cfg(4, 10, 4, 2, EPI_FILTER_LANCZOS3);
    c.src_y = 6.0f;
    c.src_h = 2.0f;
    expect_profile(&c, (const uint32_t[]){0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0});

    /* Clipped placement rows above the output still read nothing early:
     * placement starts at y = −2, so output row 0 is placement row 2. */
    c = base_cfg(4, 4, 4, 2, EPI_FILTER_BOX);
    c.dst_h = 4;
    c.dst_y = -2;
    expect_profile(&c, (const uint32_t[]){0, 0, 0, 1, 1});
}

/* Pixel-level checks of placement, background and pass-skipping. */
void test_resampler_placement(void) {
    const epi_rgb_t bg = {1, 2, 3};
    epi_resampler_config_t c = base_cfg(3, 2, 7, 4, EPI_FILTER_LANCZOS3);
    c.dst_w = 3;
    c.dst_h = 2;
    c.dst_x = 5; /* overhangs the right edge by one column */
    c.dst_y = 1;
    c.background = bg;
    const epi_rgb_t src[6] = {{10, 20, 30}, {40, 50, 60}, {70, 80, 90}, {11, 21, 31}, {41, 51, 61}, {71, 81, 91}};
    epi_rgb_t* out = resample(&c, src, 0, "placement");
    for (uint32_t y = 0; y < 4; y++) {
        for (uint32_t x = 0; x < 7; x++) {
            epi_rgb_t want = bg;
            if (y >= 1 && y <= 2 && x >= 5) {
                want = src[(y - 1) * 3 + (x - 5)]; /* identity resize: a copy */
            }
            TEST_ASSERT_EQUAL_RGB(want, out[y * 7 + x]);
        }
    }
    free(out);
}

void test_resampler_drain_contract(void) {
    /* Pushing without popping: the ring holds one window (2 rows here). */
    epi_resampler_config_t c = base_cfg(4, 8, 4, 4, EPI_FILTER_BOX);
    ws_t w;
    epi_resampler_t* r = make(&c, &w, 0);
    epi_rgb_t src[4] = {{9, 9, 9}, {9, 9, 9}, {9, 9, 9}, {9, 9, 9}};
    epi_rgb_t row[4];
    TEST_ASSERT_EQUAL_INT(EPI_ERR_NOT_READY, epi_resampler_pop(r, row)); /* before enough pushes */
    TEST_ASSERT_EQUAL_INT(EPI_OK, epi_resampler_push(r, src));
    TEST_ASSERT_EQUAL_INT(EPI_OK, epi_resampler_push(r, src)); /* window of row 0 complete */
#ifdef NDEBUG
    /* A third push without popping would overwrite row 0's window: it must
     * be rejected (debug builds assert instead). */
    TEST_ASSERT_EQUAL_INT(EPI_ERR_NOT_DRAINED, epi_resampler_push(r, src));
#endif
    uint32_t n = 0;
    while (epi_resampler_pop(r, row) == EPI_OK) {
        TEST_ASSERT_EQUAL_UINT8(9, row[0].r);
        n++;
    }
    TEST_ASSERT_EQUAL_UINT32(1, n);
    for (int i = 0; i < 6; i++) {
        epi_resampler_push(r, src);
        while (epi_resampler_pop(r, row) == EPI_OK) {
            n++;
        }
    }
    TEST_ASSERT_EQUAL_UINT32(4, n);
    TEST_ASSERT_EQUAL_INT(EPI_ERR_FINISHED, epi_resampler_pop(r, row));
    TEST_ASSERT_EQUAL_INT(EPI_ERR_FINISHED, epi_resampler_push(r, src));
    free(w.mem);

#ifdef NDEBUG
    /* Lanczos has a wider window: repeated undrained pushes are all rejected
     * and the rows accepted before them still resample like a normal run. */
    epi_resampler_config_t lc = base_cfg(50, 500, 20, 40, EPI_FILTER_LANCZOS3);
    epi_layout_cover(&lc);
    epi_rgb_t* img = new_source(lc.src_width, lc.src_height, 7);
    epi_rgb_t* want = resample(&lc, img, 0, "lanczos drain");
    epi_resampler_t* lr = make(&lc, &w, 0);
    unsigned accepted = 0, rejected = 0;
    for (uint32_t y = 0; y < lc.src_height; y++) {
        epi_err_t e = epi_resampler_push(lr, img + (size_t)y * lc.src_width);
        if (e == EPI_OK) {
            TEST_ASSERT_EQUAL_UINT32(0, rejected); /* accepted rows are a prefix */
            accepted++;
        } else {
            TEST_ASSERT_EQUAL_INT(EPI_ERR_NOT_DRAINED, e);
            rejected++;
        }
    }
    TEST_ASSERT_TRUE(accepted > 0 && rejected > 1);
    epi_rgb_t out[20];
    TEST_ASSERT_EQUAL_UINT16(sizeof out / sizeof out[0], lc.dst_width);
    uint32_t popped = 0;
    while (epi_resampler_pop(lr, out) == EPI_OK) {
        TEST_ASSERT_EQUAL_MEMORY(want + (size_t)popped * lc.dst_width, out, sizeof out);
        popped++;
    }
    TEST_ASSERT_TRUE(popped > 0);
    free(w.mem);
    free(want);
    free(img);
#endif
}

/* ---- layout ---- */

#define EXPECT_F_EQ(want, got) TEST_ASSERT_TRUE((got) == (want))
#define EXPECT_F_NEAR(want, got) TEST_ASSERT_TRUE(fabsf((got) - (want)) <= 1e-3f)

void test_resampler_layout(void) {
    epi_resampler_config_t c = base_cfg(4000, 3000, 1200, 1600, EPI_FILTER_LANCZOS3);
    epi_layout_cover(&c); /* scale = 1600/3000: full height, centred width */
    EXPECT_F_EQ(0.0f, c.src_y);
    EXPECT_F_EQ(3000.0f, c.src_h);
    EXPECT_F_NEAR(2250.0f, c.src_w);
    EXPECT_F_NEAR(875.0f, c.src_x);
    TEST_ASSERT_TRUE(c.src_x + c.src_w <= 4000.0f);
    TEST_ASSERT_EQUAL_INT32(0, c.dst_x);
    TEST_ASSERT_EQUAL_INT32(0, c.dst_y);
    TEST_ASSERT_EQUAL_UINT16(1200, c.dst_w);
    TEST_ASSERT_EQUAL_UINT16(1600, c.dst_h);
    TEST_ASSERT_NOT_EQUAL(0, epi_resampler_workspace_size(&c));

    c = base_cfg(4000, 3000, 1200, 1600, EPI_FILTER_LANCZOS3);
    epi_layout_fit(&c); /* scale = 0.3: 1200 × 900 centred vertically */
    EXPECT_F_EQ(0.0f, c.src_x);
    EXPECT_F_EQ(0.0f, c.src_y);
    EXPECT_F_EQ(4000.0f, c.src_w);
    EXPECT_F_EQ(3000.0f, c.src_h);
    TEST_ASSERT_EQUAL_UINT16(1200, c.dst_w);
    TEST_ASSERT_EQUAL_UINT16(900, c.dst_h);
    TEST_ASSERT_EQUAL_INT32(0, c.dst_x);
    TEST_ASSERT_EQUAL_INT32(350, c.dst_y);

    /* Rounding: 3×1 into 2×2 is 2×(2/3) → 2×1, floor-centred at y = 0. */
    c = base_cfg(3, 1, 2, 2, EPI_FILTER_BOX);
    epi_layout_fit(&c);
    TEST_ASSERT_EQUAL_UINT16(2, c.dst_w);
    TEST_ASSERT_EQUAL_UINT16(1, c.dst_h);
    TEST_ASSERT_EQUAL_INT32(0, c.dst_y);
    /* Half rounds away from zero: 4×1 into 2×9 → 2 × 0.5 → 2×1. */
    c = base_cfg(4, 1, 2, 9, EPI_FILTER_BOX);
    epi_layout_fit(&c);
    TEST_ASSERT_EQUAL_UINT16(1, c.dst_h);
    TEST_ASSERT_EQUAL_INT32(4, c.dst_y);
    /* At least one pixel. */
    c = base_cfg(1000, 1, 10, 10, EPI_FILTER_BOX);
    epi_layout_fit(&c);
    TEST_ASSERT_EQUAL_UINT16(10, c.dst_w);
    TEST_ASSERT_EQUAL_UINT16(1, c.dst_h);

    /* Cover never produces a rect init rejects, over many aspect ratios. */
    for (uint32_t i = 1; i < 3000; i++) {
        uint16_t sw = (uint16_t)(1 + (i * 7919u) % 4000u), sh = (uint16_t)(1 + (i * 104729u) % 3000u);
        uint16_t dw = (uint16_t)(1 + (i * 31u) % 1600u), dh = (uint16_t)(1 + (i * 17u) % 1200u);
        c = base_cfg(sw, sh, dw, dh, EPI_FILTER_BOX);
        epi_layout_cover(&c);
        TEST_ASSERT_TRUE(c.src_x >= 0.0f && c.src_y >= 0.0f);
        TEST_ASSERT_TRUE(c.src_x + c.src_w <= (float)sw && c.src_y + c.src_h <= (float)sh);
        TEST_ASSERT_NOT_EQUAL(0, epi_resampler_workspace_size(&c));
        c = base_cfg(sw, sh, dw, dh, EPI_FILTER_BOX);
        epi_layout_fit(&c);
        TEST_ASSERT_TRUE(c.dst_w >= 1 && c.dst_w <= dw && c.dst_h >= 1 && c.dst_h <= dh);
        TEST_ASSERT_TRUE(c.dst_w == dw || c.dst_h == dh);
        TEST_ASSERT_NOT_EQUAL(0, epi_resampler_workspace_size(&c));
    }

    /* Zero sizes leave the config untouched (init then rejects it). */
    c = base_cfg(0, 10, 10, 10, EPI_FILTER_BOX);
    c.src_x = 7.0f;
    epi_layout_cover(&c);
    epi_layout_fit(&c);
    EXPECT_F_EQ(7.0f, c.src_x);
    epi_layout_cover(NULL);
    epi_layout_fit(NULL);
}
