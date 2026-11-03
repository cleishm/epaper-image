/*
 * Streaming resampler (docs/design.md, "Resampling",
 * "Fixed-point arithmetic and determinism"). The coefficient computation
 * is derived from Pillow's resampling code; see LICENSE.
 *
 * Separable convolution of 8-bit RGB, one axis at a time:
 *
 *   Coefficients     epi_axis_bounds / epi_axis_coeffs: scale =
 *                    (double)(float)(x1 − x0) / out; the filter's support
 *                    is scaled by max(1, scale); output i is centred at
 *                    x0 + (i + 0.5)·scale and its window is
 *                    [trunc(centre − support + 0.5), trunc(centre + support
 *                    + 0.5)) clamped to the image. Weights are normalised by
 *                    their sum in double, then rounded to 22-bit fixed point.
 *   Passes           epi_h_pass / epi_v_pass: int32 accumulation from 2^21
 *                    (round half up), then (sum >> 22) clamped to 0..255.
 *                    The horizontal pass runs first and is clamped to 8 bits
 *                    before the vertical pass reads it; it runs only on the
 *                    rows the vertical pass reads.
 *   Copies           epi_plan: an axis at scale 1 from offset 0 is copied,
 *                    not convolved; so are both axes of a crop at integer
 *                    offsets whose size is the output's.
 *   Tall sources     when src_height > 100·src_width and the height shrinks,
 *                    the vertical pass runs first and the horizontal pass
 *                    second (each with its own copy rule, and the 8-bit
 *                    clamp between them); the ring then holds source rows.
 *
 * Streaming: the ring holds the horizontally resampled rows (or, on the
 * tall path, the source columns the horizontal pass needs) of the largest
 * vertical window. Output row y of the placement is computed at pop once the
 * last source row of its window has been pushed. Only the part of the
 * placement inside the output is computed (each output pixel depends only on
 * its own coefficients, so clipping changes nothing else).
 *
 * Determinism: the per-pixel loops are integer-only. Coefficients are
 * computed in double with + − × ÷ and the library's own sin; never libm.
 *
 * No fused multiply-add: every expression rounds after each operation as
 * written (centre = x0 + (i + 0.5)·scale and the bicubic polynomial
 * included); epi_fp.h rejects at compile time a build that would fuse them
 * (docs/design.md, "Fixed-point arithmetic and determinism"). A fused centre can move its last
 * bit, and on BOX with an integer rect that bit decides whether an edge
 * source pixel is in the window.
 */
#include "epaper_image.h"
#include "epi_fp.h"
#include "epi_math.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ---- Filters ------------------------------------------------------------- */

typedef double (*epi_filter_fn)(double x);

static double epi_f_box(double x) {
    return (x > -0.5 && x <= 0.5) ? 1.0 : 0.0;
}

static double epi_f_bilinear(double x) {
    if (x < 0.0) {
        x = -x;
    }
    return x < 1.0 ? 1.0 - x : 0.0;
}

/* a = −0.5 (unfused; see "No fused multiply-add"):
 * |x| < 1: ((a + 2)·x − (a + 3))·x·x + 1; |x| < 2: (((x − 5)·x + 8)·x − 4)·a */
static double epi_f_bicubic(double x) {
    if (x < 0.0) {
        x = -x;
    }
    if (x < 1.0) {
        return (1.5 * x - 2.5) * x * x + 1.0;
    }
    if (x < 2.0) {
        return (((x - 5.0) * x + 8.0) * x - 4.0) * -0.5;
    }
    return 0.0;
}

static double epi_sinc(double x) {
    if (x == 0.0) {
        return 1.0;
    }
    x = x * 0x1.921fb54442d18p+1; /* M_PI */
    return epi_sin_d(x) / x;
}

static double epi_f_lanczos(double x) {
    if (-3.0 <= x && x < 3.0) {
        return epi_sinc(x) * epi_sinc(x / 3.0);
    }
    return 0.0;
}

static const struct {
    epi_filter_fn fn;
    double support;
} epi_filters[] = {
    [EPI_FILTER_BOX] = {epi_f_box, 0.5},
    [EPI_FILTER_BILINEAR] = {epi_f_bilinear, 1.0},
    [EPI_FILTER_BICUBIC] = {epi_f_bicubic, 2.0},
    [EPI_FILTER_LANCZOS3] = {epi_f_lanczos, 3.0},
};

/* ---- One axis: coefficients, one output index at a time ------------------ */

#define EPI_RS_BITS 22 /* fixed-point weight precision */

typedef struct {
    uint8_t conv;     /* 1: convolution; 0: copy (pass skipped or cropped) */
    int32_t off;      /* copy: source index of output index 0 */
    uint32_t in_size; /* conv: source extent along the axis */
    uint32_t max_n;   /* conv: largest window epi_axis_bounds returns (set by epi_plan) */
    double in0, scale, support, inv_fs;
} epi_axis_t;

static void epi_axis_conv(epi_axis_t* a, uint32_t in_size, float x0, float x1, uint32_t out, double base_support) {
    float w = x1 - x0; /* the extent, in float */
    double scale = (double)w / (double)out;
    double fs = scale < 1.0 ? 1.0 : scale;
    a->conv = 1;
    a->in_size = in_size;
    a->max_n = UINT32_MAX; /* the plan measures the true maximum with the same function */
    a->in0 = (double)x0;
    a->scale = scale;
    a->support = base_support * fs;
    a->inv_fs = 1.0 / fs;
}

/* Source window [*lo, *lo + *n) of output index i; returns the centre. */
static double epi_axis_bounds(const epi_axis_t* a, uint32_t i, uint32_t* lo, uint32_t* n) {
    if (!a->conv) {
        *lo = (uint32_t)a->off + i;
        *n = 1;
        return 0.0;
    }
    double center = a->in0 + ((double)i + 0.5) * a->scale; /* unfused */
    int32_t xmin = (int32_t)(center - a->support + 0.5);
    if (xmin < 0) {
        xmin = 0;
    }
    int32_t xmax = (int32_t)(center + a->support + 0.5);
    if (xmax > (int32_t)a->in_size) {
        xmax = (int32_t)a->in_size;
    }
    *lo = (uint32_t)xmin;
    *n = (uint32_t)(xmax - xmin);
    /* Planning and later windows agree on conforming targets (a no-op); under
     * excess precision this keeps the window arrays in bounds, it does not
     * make the output correct. */
    if (*n > a->max_n) {
        *n = a->max_n;
    }
    return center;
}

/* A normalised weight to fixed point, rounding half away from zero. */
static int32_t epi_to_fixed(double p) {
    if (p < 0) {
        return (int32_t)(-0.5 + p * (double)(1 << EPI_RS_BITS));
    }
    return (int32_t)(0.5 + p * (double)(1 << EPI_RS_BITS));
}

/* Fixed-point weights k[0..n) of output index i (conv axes only). Each tap
 * is evaluated once, into the scratch w[0..n), summing in tap order; the
 * normalisation then reads w, so it is the same value the sum saw. */
static void epi_axis_coeffs(
    const epi_axis_t* a,
    epi_filter_fn f,
    uint32_t i,
    double* w,
    int32_t* k,
    uint32_t* lo_out,
    uint32_t* n_out
) {
    uint32_t lo, n;
    double center = epi_axis_bounds(a, i, &lo, &n);
    double ww = 0.0;
    for (uint32_t t = 0; t < n; t++) {
        w[t] = f(((double)(int32_t)(t + lo) - center + 0.5) * a->inv_fs);
        ww += w[t];
    }
    for (uint32_t t = 0; t < n; t++) {
        k[t] = epi_to_fixed(ww != 0.0 ? w[t] / ww : w[t]);
    }
    *lo_out = lo;
    *n_out = n;
}

/* ---- Plan: validation and every derived size ------------------------------ */

typedef struct {
    epi_axis_t h, v;
    uint8_t vh; /* tall path: vertical pass before horizontal */
    uint8_t filter;
    uint16_t src_height, dst_width, dst_height;
    epi_rgb_t background;
    uint32_t vis_x0, vis_w; /* visible placement columns, output coords */
    uint32_t vis_y0, vis_h; /* visible placement rows, output coords */
    uint32_t col0, row0;    /* first visible resampled column / row */
    uint32_t h_taps;        /* max horizontal window (0 if h copies; may be 0 if it convolves) */
    uint64_t hk_len;        /* horizontal weights: the visible columns' windows, summed */
    uint32_t v_taps;        /* max vertical window = ring rows (0: no row is ever stored) */
    uint32_t ring_w;        /* pixels per ring row */
    uint32_t cbase;         /* tall path: first source column held */
    uint32_t ybox_last;     /* source rows the output reads */
} epi_rs_plan_t;

static int epi_is_int_f(float v) { /* v − (int)v == 0, v in [0, 65535] */
    return v - (float)(int32_t)v == 0.0f;
}

/* A side over EPI_MAX_SRC_DIMENSION, or an area over the limit (uint64_t:
 * 65536 × 65536 would wrap to 0 in 32 bits). */
static int epi_source_too_large(const epi_resampler_config_t* cfg) {
    const uint32_t limit = cfg->max_src_pixels != 0 ? cfg->max_src_pixels : EPI_DEFAULT_MAX_SRC_PIXELS;
    return cfg->src_width > EPI_MAX_SRC_DIMENSION || cfg->src_height > EPI_MAX_SRC_DIMENSION ||
        (uint64_t)cfg->src_width * cfg->src_height > limit;
}

static epi_err_t epi_plan(const epi_resampler_config_t* cfg, epi_rs_plan_t* p) {
    if (cfg == NULL || cfg->src_width == 0 || cfg->src_height == 0) {
        return EPI_ERR_INVALID_ARG;
    }
    if (epi_source_too_large(cfg)) {
        return EPI_ERR_SOURCE_TOO_LARGE;
    }
    if (cfg->dst_width == 0 || cfg->dst_height == 0 || cfg->dst_w == 0 || cfg->dst_h == 0 ||
        (unsigned)cfg->filter > (unsigned)EPI_FILTER_LANCZOS3) {
        return EPI_ERR_INVALID_ARG;
    }
    const float x0 = cfg->src_x, y0 = cfg->src_y;
    /* False for NaN and −inf; +inf anywhere fails the far-corner test below. */
    if (!(cfg->src_w > 0.0f) || !(cfg->src_h > 0.0f) || !(x0 >= 0.0f) || !(y0 >= 0.0f)) {
        return EPI_ERR_INVALID_ARG;
    }
    const float x1 = x0 + cfg->src_w; /* the rect's far corner (x1, y1) */
    const float y1 = y0 + cfg->src_h;
    if (x1 > (float)cfg->src_width || y1 > (float)cfg->src_height) {
        return EPI_ERR_INVALID_ARG;
    }

    memset(p, 0, sizeof *p);
    p->filter = (uint8_t)cfg->filter;
    p->src_height = (uint16_t)cfg->src_height; /* ≤ EPI_MAX_SRC_DIMENSION, checked above */
    p->dst_width = cfg->dst_width;
    p->dst_height = cfg->dst_height;
    p->background = cfg->background;
    const uint32_t sw = cfg->src_width, sh = cfg->src_height;
    const uint32_t dw = cfg->dst_w, dh = cfg->dst_h;
    const double sup = epi_filters[cfg->filter].support;
    const int crop_x = epi_is_int_f(x0) && (float)(x1 - x0) == (float)dw;
    const int crop_y = epi_is_int_f(y0) && (float)(y1 - y0) == (float)dh;

    const int tall = sh > 100u * sw && dh < sh;
    const int id_x = dw == sw && x0 == 0.0f && x1 == (float)dw;
    const int id_y = dh == sh && y0 == 0.0f && y1 == (float)dh;
    /* A crop copies only if the other axis crops too, except on the tall path. */
    const int copy_x = (crop_x && (tall || crop_y)) || id_x;
    const int copy_y = (crop_y && (tall || crop_x)) || id_y;

    if (copy_x) {
        p->h.off = (int32_t)x0;
    } else {
        epi_axis_conv(&p->h, sw, x0, x1, dw, sup);
    }
    if (copy_y) {
        p->v.off = (int32_t)y0;
    } else {
        epi_axis_conv(&p->v, sh, y0, y1, dh, sup);
    }
    /* With either pass a copy the order does not matter. */
    p->vh = (uint8_t)(tall && !copy_x && !copy_y);

    /* Visible part of the placement (int64: dst_x + dst_w may exceed int32). */
    int64_t cx0 = cfg->dst_x < 0 ? 0 : cfg->dst_x;
    int64_t cx1 = (int64_t)cfg->dst_x + dw;
    if (cx1 > cfg->dst_width) {
        cx1 = cfg->dst_width;
    }
    int64_t cy0 = cfg->dst_y < 0 ? 0 : cfg->dst_y;
    int64_t cy1 = (int64_t)cfg->dst_y + dh;
    if (cy1 > cfg->dst_height) {
        cy1 = cfg->dst_height;
    }
    if (cx1 <= cx0 || cy1 <= cy0) {
        return EPI_OK; /* nothing visible: all background */
    }
    p->vis_x0 = (uint32_t)cx0;
    p->vis_w = (uint32_t)(cx1 - cx0);
    p->vis_y0 = (uint32_t)cy0;
    p->vis_h = (uint32_t)(cy1 - cy0);
    p->col0 = (uint32_t)(cx0 - cfg->dst_x);
    p->row0 = (uint32_t)(cy0 - cfg->dst_y);

    uint32_t lo, n, first_lo = 0, last_end = 0;
    for (uint32_t j = 0; j < p->vis_w; j++) {
        epi_axis_bounds(&p->h, p->col0 + j, &lo, &n);
        if (p->h.conv) {
            p->h_taps = n > p->h_taps ? n : p->h_taps;
            p->hk_len += n;
        }
        if (j == 0) {
            first_lo = lo;
        }
        last_end = lo + n; /* bounds are monotone in j */
    }
    for (uint32_t i = 0; i < p->vis_h; i++) {
        epi_axis_bounds(&p->v, p->row0 + i, &lo, &n);
        if (n > p->v_taps) {
            p->v_taps = n;
        }
        p->ybox_last = lo + n;
    }
    p->h.max_n = p->h_taps;
    p->v.max_n = p->v_taps;
    if (p->vh) {
        p->cbase = first_lo;
        p->ring_w = last_end - first_lo;
    } else {
        p->ring_w = p->vis_w;
    }
    return EPI_OK;
}

/* ---- Resampler state and workspace ---------------------------------------- */

struct epi_resampler {
    epi_rs_plan_t p;
    uint16_t* hlo;            /* [vis_w] first source column of each visible column */
    uint16_t* hn;             /* [vis_w] window length */
    int32_t* hk;              /* [hk_len] horizontal weights, hn[j] for column j, packed */
    int32_t* vk;              /* [v_taps] weights of the current row */
    const epi_rgb_t** win;    /* [v_taps] ring rows of the current window */
    epi_rgb_t* ring;          /* [v_taps · ring_w] */
    epi_rgb_t* tmp;           /* [ring_w] tall path: vertical result */
    double* wk;               /* [max(h_taps, v_taps)] scratch for epi_axis_coeffs */
    uint32_t pushed;          /* source rows accepted */
    uint32_t popped;          /* output rows written */
    uint32_t vis_done;        /* visible placement rows written */
    uint32_t next_lo, next_n; /* source window of the next visible row */
};

#define EPI_RS_ALIGN 8u

typedef struct {
    size_t o_win, o_hk, o_vk, o_hlo, o_hn, o_ring, o_tmp, o_wk;
} epi_rs_layout_t;

static int epi_add_region(uint64_t* at, uint64_t bytes, size_t* off) {
    *at = (*at + (EPI_RS_ALIGN - 1u)) & ~(uint64_t)(EPI_RS_ALIGN - 1u);
    if (*at > SIZE_MAX) {
        return 0;
    }
    *off = (size_t)*at;
    *at += bytes;
    return 1;
}

static size_t epi_layout_bytes(const epi_rs_plan_t* p, epi_rs_layout_t* l) {
    uint64_t at = sizeof(struct epi_resampler);
    const uint64_t vt = p->v_taps, ht = p->h_taps, vw = p->vis_w, rw = p->ring_w;
    if (!epi_add_region(&at, vt * sizeof(const epi_rgb_t*), &l->o_win) ||
        !epi_add_region(&at, p->hk_len * sizeof(int32_t), &l->o_hk) ||
        !epi_add_region(&at, (p->v.conv ? vt : 0u) * sizeof(int32_t), &l->o_vk) ||
        !epi_add_region(&at, (p->h.conv ? vw : 0u) * sizeof(uint16_t), &l->o_hlo) ||
        !epi_add_region(&at, (p->h.conv ? vw : 0u) * sizeof(uint16_t), &l->o_hn) ||
        !epi_add_region(&at, vt * rw * sizeof(epi_rgb_t), &l->o_ring) ||
        !epi_add_region(&at, (p->vh ? rw : 0u) * sizeof(epi_rgb_t), &l->o_tmp) ||
        !epi_add_region(&at, (ht > vt ? ht : vt) * sizeof(double), &l->o_wk)) {
        return 0;
    }
    /* Alignment slack: the caller's buffer may start anywhere. */
    uint64_t total = at + (EPI_RS_ALIGN - 1u);
    if (total > SIZE_MAX) {
        return 0;
    }
    return (size_t)total;
}

size_t epi_resampler_workspace_size(const epi_resampler_config_t* cfg) {
    epi_rs_plan_t p;
    epi_rs_layout_t l;
    if (epi_plan(cfg, &p) != EPI_OK) {
        return 0;
    }
    return epi_layout_bytes(&p, &l);
}

static void epi_next_window(epi_resampler_t* r) {
    if (r->vis_done < r->p.vis_h) {
        epi_axis_bounds(&r->p.v, r->p.row0 + r->vis_done, &r->next_lo, &r->next_n);
    }
}

epi_err_t epi_resampler_init(const epi_resampler_config_t* cfg, void* workspace, size_t size, epi_resampler_t** out) {
    epi_rs_plan_t p;
    epi_rs_layout_t l;
    if (out != NULL) {
        *out = NULL;
    }
    if (out == NULL || workspace == NULL) {
        return EPI_ERR_INVALID_ARG;
    }
    epi_err_t err = epi_plan(cfg, &p);
    if (err != EPI_OK) {
        return err;
    }
    size_t need = epi_layout_bytes(&p, &l);
    if (need == 0) {
        return EPI_ERR_INVALID_ARG;
    }
    if (size < need) {
        return EPI_ERR_WORKSPACE_TOO_SMALL;
    }
    uint8_t* base = epi_align_ptr(workspace, EPI_RS_ALIGN);
    epi_resampler_t* r = (epi_resampler_t*)(void*)base;
    memset(r, 0, sizeof *r);
    r->p = p;
    r->win = (const epi_rgb_t**)(void*)(base + l.o_win);
    r->hk = (int32_t*)(void*)(base + l.o_hk);
    r->vk = (int32_t*)(void*)(base + l.o_vk);
    r->hlo = (uint16_t*)(void*)(base + l.o_hlo);
    r->hn = (uint16_t*)(void*)(base + l.o_hn);
    r->ring = (epi_rgb_t*)(void*)(base + l.o_ring);
    r->tmp = (epi_rgb_t*)(void*)(base + l.o_tmp);
    r->wk = (double*)(void*)(base + l.o_wk);

    /* Every visible column gets bounds, even when all windows are empty
     * (h_taps = 0: a box collapsed to x0 = x1 under BOX gives
     * black, the rounding offset alone). */
    if (p.h.conv) {
        const epi_filter_fn f = epi_filters[p.filter].fn;
        int32_t* k = r->hk;
        uint64_t used = 0;
        for (uint32_t j = 0; j < p.vis_w; j++) {
            uint32_t lo, n;
            epi_axis_bounds(&p.h, p.col0 + j, &lo, &n);
            used += n;
            if (used > p.hk_len) { /* the same belt as max_n, for the packed table */
                return EPI_ERR_INVALID_ARG;
            }
            epi_axis_coeffs(&p.h, f, p.col0 + j, r->wk, k, &lo, &n);
            r->hlo[j] = (uint16_t)lo;
            r->hn[j] = (uint16_t)n;
            k += n;
        }
    }
    epi_next_window(r);
    *out = r;
    return EPI_OK;
}

/* ---- Per-pixel passes (integer only) -------------------------------------- */

static inline uint8_t epi_clip8(int32_t ss) {
    if (ss < 0) {
        return 0; /* (ss >> 22) < 0 → 0 */
    }
    uint32_t v = (uint32_t)ss >> EPI_RS_BITS;
    return v > 255u ? 255u : (uint8_t)v;
}

#define EPI_RS_HALF ((int32_t)1 << (EPI_RS_BITS - 1))

/* Horizontal pass of one row. in[c − in_base] is source column c. Sums are
 * bounded by 255·Σ|k| + 2^21 < 1.4·2^30 for every filter. */
static void epi_h_pass(const epi_resampler_t* r, const epi_rgb_t* in, uint32_t in_base, epi_rgb_t* out) {
    const epi_rs_plan_t* p = &r->p;
    if (!p->h.conv) {
        memcpy(out, in + ((uint32_t)p->h.off + p->col0 - in_base), (size_t)p->vis_w * sizeof *out);
        return;
    }
    const int32_t* k = r->hk;
    for (uint32_t j = 0; j < p->vis_w; j++) {
        const epi_rgb_t* s = in + (r->hlo[j] - in_base);
        const uint32_t n = r->hn[j];
        int32_t s0 = EPI_RS_HALF, s1 = EPI_RS_HALF, s2 = EPI_RS_HALF;
        for (uint32_t t = 0; t < n; t++) {
            s0 += (int32_t)s[t].r * k[t];
            s1 += (int32_t)s[t].g * k[t];
            s2 += (int32_t)s[t].b * k[t];
        }
        out[j].r = epi_clip8(s0);
        out[j].g = epi_clip8(s1);
        out[j].b = epi_clip8(s2);
        k += n;
    }
}

/* Vertical pass over the current window into out[0, width). */
static void epi_v_pass(const epi_resampler_t* r, uint32_t width, epi_rgb_t* out) {
    const uint32_t n = r->next_n;
    const int32_t* k = r->vk;
    for (uint32_t j = 0; j < width; j++) {
        int32_t s0 = EPI_RS_HALF, s1 = EPI_RS_HALF, s2 = EPI_RS_HALF;
        for (uint32_t t = 0; t < n; t++) {
            const epi_rgb_t* q = r->win[t] + j;
            s0 += (int32_t)q->r * k[t];
            s1 += (int32_t)q->g * k[t];
            s2 += (int32_t)q->b * k[t];
        }
        out[j].r = epi_clip8(s0);
        out[j].g = epi_clip8(s1);
        out[j].b = epi_clip8(s2);
    }
}

static inline epi_rgb_t* epi_ring_row(const epi_resampler_t* r, uint32_t src_row) {
    return r->ring + (size_t)(src_row % r->p.v_taps) * r->p.ring_w;
}

epi_err_t epi_resampler_push(epi_resampler_t* r, const epi_rgb_t* src_row) {
    if (r == NULL || src_row == NULL) {
        return EPI_ERR_INVALID_ARG;
    }
    if (r->pushed >= r->p.src_height) {
        return EPI_ERR_FINISHED;
    }
    const epi_rs_plan_t* p = &r->p;
    const uint32_t s = r->pushed;
    if (r->vis_done < p->vis_h && s >= r->next_lo && s < p->ybox_last) {
        /* Accepting a row more than a window past the next pending row
         * would overwrite a ring row that row still needs. */
        if (s - r->next_lo >= p->v_taps) {
            assert(!"epi_resampler_push: pop until not OK after every push");
            return EPI_ERR_NOT_DRAINED;
        }
        epi_rgb_t* dst = epi_ring_row(r, s);
        if (p->vh) {
            memcpy(dst, src_row + p->cbase, (size_t)p->ring_w * sizeof *dst);
        } else {
            epi_h_pass(r, src_row, 0, dst);
        }
    }
    r->pushed = s + 1;
    return EPI_OK;
}

static void epi_fill(epi_rgb_t* row, uint32_t n, epi_rgb_t c) {
    for (uint32_t i = 0; i < n; i++) {
        row[i] = c;
    }
}

epi_err_t epi_resampler_pop(epi_resampler_t* r, epi_rgb_t* dst_row) {
    if (r == NULL || dst_row == NULL) {
        return EPI_ERR_INVALID_ARG;
    }
    if (r->popped >= r->p.dst_height) {
        return EPI_ERR_FINISHED;
    }
    const epi_rs_plan_t* p = &r->p;
    const uint32_t y = r->popped;
    if (y < p->vis_y0 || y >= p->vis_y0 + p->vis_h) {
        epi_fill(dst_row, p->dst_width, p->background);
        r->popped = y + 1;
        return EPI_OK;
    }
    if (r->pushed < r->next_lo + r->next_n) {
        return EPI_ERR_NOT_READY;
    }
    epi_fill(dst_row, p->vis_x0, p->background);
    epi_fill(dst_row + p->vis_x0 + p->vis_w, p->dst_width - p->vis_x0 - p->vis_w, p->background);
    epi_rgb_t* out = dst_row + p->vis_x0;
    if (!p->v.conv) {
        memcpy(out, epi_ring_row(r, r->next_lo), (size_t)p->vis_w * sizeof *out);
    } else {
        uint32_t lo, n;
        epi_axis_coeffs(&p->v, epi_filters[p->filter].fn, p->row0 + r->vis_done, r->wk, r->vk, &lo, &n);
        r->next_n = n;
        for (uint32_t t = 0; t < n; t++) {
            r->win[t] = epi_ring_row(r, lo + t);
        }
        if (p->vh) {
            epi_v_pass(r, p->ring_w, r->tmp);
            epi_h_pass(r, r->tmp, p->cbase, out);
        } else {
            epi_v_pass(r, p->vis_w, out);
        }
    }
    r->popped = y + 1;
    r->vis_done++;
    epi_next_window(r);
    return EPI_OK;
}

/* ---- Layout helpers ------------------------------------------------------- */

/* Round half away from zero for v ≥ 0 (exact: v − trunc(v) is exact). */
static uint32_t epi_round_pos_f(float v) {
    uint32_t t = (uint32_t)v;
    return (v - (float)t >= 0.5f) ? t + 1u : t;
}

/* A source side cropped to extent len (at most the side), centred. */
static void epi_crop_axis(float side, float len, float* pos, float* size) {
    *size = len > side ? side : len;
    *pos = (side - *size) * 0.5f;
}

void epi_layout_cover(epi_resampler_config_t* cfg) {
    if (cfg == NULL || cfg->src_width == 0 || cfg->src_height == 0 || cfg->dst_width == 0 || cfg->dst_height == 0) {
        return;
    }
    const float sw = (float)cfg->src_width, sh = (float)cfg->src_height;
    const float dw = (float)cfg->dst_width, dh = (float)cfg->dst_height;
    const float sx = dw / sw, sy = dh / sh;
    if (sx >= sy) { /* width fills: the whole source width, centred rows */
        cfg->src_x = 0.0f;
        cfg->src_w = sw;
        epi_crop_axis(sh, dh / sx, &cfg->src_y, &cfg->src_h);
    } else {
        cfg->src_y = 0.0f;
        cfg->src_h = sh;
        epi_crop_axis(sw, dw / sy, &cfg->src_x, &cfg->src_w);
    }
    cfg->dst_x = 0;
    cfg->dst_y = 0;
    cfg->dst_w = cfg->dst_width;
    cfg->dst_h = cfg->dst_height;
}

void epi_layout_fit(epi_resampler_config_t* cfg) {
    if (cfg == NULL || cfg->src_width == 0 || cfg->src_height == 0 || cfg->dst_width == 0 || cfg->dst_height == 0) {
        return;
    }
    const float sw = (float)cfg->src_width, sh = (float)cfg->src_height;
    const float sx = (float)cfg->dst_width / sw, sy = (float)cfg->dst_height / sh;
    const float scale = sx <= sy ? sx : sy;
    uint32_t w = epi_round_pos_f(sw * scale);
    uint32_t h = epi_round_pos_f(sh * scale);
    w = w < 1u ? 1u : (w > cfg->dst_width ? cfg->dst_width : w);
    h = h < 1u ? 1u : (h > cfg->dst_height ? cfg->dst_height : h);
    cfg->src_x = 0.0f;
    cfg->src_y = 0.0f;
    cfg->src_w = sw;
    cfg->src_h = sh;
    cfg->dst_w = (uint16_t)w;
    cfg->dst_h = (uint16_t)h;
    cfg->dst_x = (int32_t)((cfg->dst_width - w) / 2u);
    cfg->dst_y = (int32_t)((cfg->dst_height - h) / 2u);
}
