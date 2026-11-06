/*
 * Colour panels from measured inks (docs/design.md, "Colour panels from inks").
 *
 * Everything is derived at init, deterministically (design.md "Fixed-point
 * arithmetic and determinism"): in double with + − × ÷ plus the library's
 * epi_cbrt_d, epi_sqrt_d, epi_sin_d and epi_exp_d (no libm), each value rounded
 * once to its integer format. tools/panelgen builds the const colour panels by
 * running this derivation, so a panel from either is the same panel.
 */
#include "epaper_image.h"
#include "epi_fp.h"
#include "epi_intent.h"
#include "epi_math.h"
#include "epi_panel_tables.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define COLOUR_INTENT(t, v) {.tone = (t), .detail = 0.75f, .adapt = 0.5f, .vibrance = (v), .veil = 0.0f, .lift = 8.0f}

static const epi_intent_t colour_intent_default = COLOUR_INTENT(0.5f, 1.0f);
static const epi_intent_t colour_intent_soft = COLOUR_INTENT(0.0f, 1.0f);
static const epi_intent_t colour_intent_deep = COLOUR_INTENT(1.0f, 1.0f);
static const epi_intent_t colour_intent_vivid = COLOUR_INTENT(0.5f, 1.3f);

const epi_preset_t epi_colour_presets[EPI_COLOUR_PRESET_COUNT] = {
    {"Default", &colour_intent_default},
    {"Soft", &colour_intent_soft},
    {"Deep", &colour_intent_deep},
    {"Vivid", &colour_intent_vivid},
};

/* What the caller's storage holds (placed at its first aligned address). */
typedef struct {
    epi_panel_t panel;
    epi_colour_tables_t tables;
    epi_ink_t inks[EPI_MAX_INKS];
} colour_storage_t;

#define COLOUR_ALIGN ((size_t)_Alignof(colour_storage_t))
#define COLOUR_STORAGE_SIZE (sizeof(colour_storage_t) + COLOUR_ALIGN - 1)

typedef struct {
    double m[3][3];
} mat3_t;

/* IEC 61966-2-1 linear sRGB to XYZ (D65), in its 4-decimal form. */
static const mat3_t M_SRGB = {{
    {0.4124, 0.3576, 0.1805},
    {0.2126, 0.7152, 0.0722},
    {0.0193, 0.1192, 0.9505},
}};
static const mat3_t M_BRADFORD = {{
    {0.8951, 0.2664, -0.1614},
    {-0.7502, 1.7135, 0.0367},
    {0.0389, -0.0685, 1.0296},
}};
/* Ottosson's OKLab: linear sRGB to LMS, and cbrt(LMS) to OKLab. */
static const mat3_t OK_M1 = {{
    {0.4122214708, 0.5363325363, 0.0514459929},
    {0.2119034982, 0.6806995451, 0.1073969566},
    {0.0883024619, 0.2817188376, 0.6299787005},
}};
static const mat3_t OK_M2 = {{
    {0.2104542553, 0.7936177850, -0.0040720468},
    {1.9779984951, -2.4285922050, 0.4505937099},
    {0.0259040371, 0.7827717662, -0.8086757660},
}};

#define PI 3.14159265358979323846
#define Q8 256.0
#define Q16 65536.0
#define Q32 4294967296.0

#define HULL_DEGENERATE 1e-12 /* |normal| of a triple with no plane */
#define HULL_SIDE_TOL 1e-11   /* a point this close to a plane is on it */
#define HULL_DUP_TOL 1e-9     /* faces this close are one face */
#define HULL_TOL 1e-7         /* containment in the cusp search, XYZ */
#define NEUTRAL_EPS 1e-9      /* shrinks the neutral interval, Y */

#define CUSP_L_SAMPLES 64
#define CUSP_REFINE 17
#define CUSP_REFINE_PASSES 4
#define CUSP_BISECT_ITERS 20
#define CUSP_C_LSB (1.0 / (1 << CUSP_BISECT_ITERS)) /* of the probe's chroma */
#define CUSP_C_PROBE 200.0                          /* chroma of the probe; checked to be outside */

/* The hull in double, before quantisation: inside iff n·x + d ≤ 0 on every face. */
typedef struct {
    unsigned count;
    double n[EPI_MAX_HULL_FACES][3];
    double d[EPI_MAX_HULL_FACES];
} hull_t;

static epi_err_t reject(const char** why, const char* reason) {
    *why = reason;
    return EPI_ERR_INVALID_ARG;
}

/* q(x, n) = sign(x)·⌊|x|·2^n + 0.5⌋, with scale = 2^n. */
static int64_t q_round(double x, double scale) {
    const int64_t m = (int64_t)((x < 0.0 ? -x : x) * scale + 0.5);
    return x < 0.0 ? -m : m;
}

static int32_t q16(double x) {
    return (int32_t)q_round(x, Q16);
}

static void mat3_vec(const mat3_t* a, const double v[3], double out[3]) {
    for (int r = 0; r < 3; r++) {
        out[r] = a->m[r][0] * v[0] + a->m[r][1] * v[1] + a->m[r][2] * v[2];
    }
}

static mat3_t mat3_mul(const mat3_t* a, const mat3_t* b) {
    mat3_t out;
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            out.m[r][c] = a->m[r][0] * b->m[0][c] + a->m[r][1] * b->m[1][c] + a->m[r][2] * b->m[2][c];
        }
    }
    return out;
}

/* The adjugate over the determinant, for the two constant matrices. */
static mat3_t mat3_inverse(const mat3_t* a) {
    const double (*m)[3] = a->m;
    const double c00 = m[1][1] * m[2][2] - m[1][2] * m[2][1];
    const double c10 = m[1][2] * m[2][0] - m[1][0] * m[2][2];
    const double c20 = m[1][0] * m[2][1] - m[1][1] * m[2][0];
    const double det = m[0][0] * c00 + m[0][1] * c10 + m[0][2] * c20;
    mat3_t out;
    out.m[0][0] = c00 / det;
    out.m[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / det;
    out.m[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / det;
    out.m[1][0] = c10 / det;
    out.m[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / det;
    out.m[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / det;
    out.m[2][0] = c20 / det;
    out.m[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / det;
    out.m[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / det;
    return out;
}

/* x^(1/5) for x in (0, 1]: Newton from 1, which decreases monotonically from
 * above until rounding stops it. */
static double fifth_root(double x) {
    double y = 1.0;
    for (;;) {
        const double y2 = y * y;
        const double next = (4.0 * y + x / (y2 * y2)) / 5.0;
        if (next >= y) {
            return y;
        }
        y = next;
    }
}

/* The sRGB EOTF of an 8-bit value: x^2.4 = x²·(x²)^(1/5). */
static double srgb_lin(uint8_t u) {
    const double c = u / 255.0;
    if (c <= 0.04045) {
        return c / 12.92;
    }
    const double b = (c + 0.055) / 1.055;
    const double b2 = b * b;
    return b2 * fifth_root(b2);
}

static void ink_linear(const epi_ink_t* ink, double lin[3]) {
    lin[0] = srgb_lin(ink->measured.r);
    lin[1] = srgb_lin(ink->measured.g);
    lin[2] = srgb_lin(ink->measured.b);
}

/* Y of an ink's nominal colour, as inks_hull takes Y of the measured one. */
static double nominal_y(const epi_ink_t* ink) {
    const double lin[3] = {srgb_lin(ink->nominal.r), srgb_lin(ink->nominal.g), srgb_lin(ink->nominal.b)};
    double xyz[3];
    mat3_vec(&M_SRGB, lin, xyz);
    return xyz[1];
}

/*
 * The convex hull of the inks' XYZ: a triple of inks, in lexicographic order, is a
 * face if all the other inks lie on one side of its plane. That order is the
 * canonical order of the faces, and the duplicate test merges the triples of a
 * face with more than three inks. 16 inks make at most 28 faces, but the tolerances
 * can keep slightly different planes of a nearly flat face, hence the check.
 */
static epi_err_t hull_build(double xyz[][3], size_t n, hull_t* h, const char** why) {
    h->count = 0;
    for (size_t i = 0; i < n; i++) {
        for (size_t j = i + 1; j < n; j++) {
            for (size_t k = j + 1; k < n; k++) {
                const double* a = xyz[i];
                const double u[3] = {xyz[j][0] - a[0], xyz[j][1] - a[1], xyz[j][2] - a[2]};
                const double v[3] = {xyz[k][0] - a[0], xyz[k][1] - a[1], xyz[k][2] - a[2]};
                double nf[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
                const double len = epi_sqrt_d(nf[0] * nf[0] + nf[1] * nf[1] + nf[2] * nf[2]);
                if (len < HULL_DEGENERATE) {
                    continue;
                }
                nf[0] /= len;
                nf[1] /= len;
                nf[2] /= len;
                double df = -(nf[0] * a[0] + nf[1] * a[1] + nf[2] * a[2]);

                bool above = false, below = false;
                for (size_t p = 0; p < n; p++) {
                    const double s = nf[0] * xyz[p][0] + nf[1] * xyz[p][1] + nf[2] * xyz[p][2] + df;
                    above = above || s > HULL_SIDE_TOL;
                    below = below || s < -HULL_SIDE_TOL;
                }
                if (above && below) {
                    continue;
                }
                if (!above && !below) {
                    return reject(why, "inks do not span three dimensions");
                }
                if (above) {
                    nf[0] = -nf[0];
                    nf[1] = -nf[1];
                    nf[2] = -nf[2];
                    df = -df;
                }

                bool duplicate = false;
                for (unsigned f = 0; f < h->count && !duplicate; f++) {
                    duplicate = nf[0] - h->n[f][0] < HULL_DUP_TOL && h->n[f][0] - nf[0] < HULL_DUP_TOL &&
                        nf[1] - h->n[f][1] < HULL_DUP_TOL && h->n[f][1] - nf[1] < HULL_DUP_TOL &&
                        nf[2] - h->n[f][2] < HULL_DUP_TOL && h->n[f][2] - nf[2] < HULL_DUP_TOL &&
                        df - h->d[f] < HULL_DUP_TOL && h->d[f] - df < HULL_DUP_TOL;
                }
                if (duplicate) {
                    continue;
                }
                if (h->count == EPI_MAX_HULL_FACES) {
                    return reject(why, "ink hull has too many faces");
                }
                memcpy(h->n[h->count], nf, sizeof nf);
                h->d[h->count++] = df;
            }
        }
    }
    return h->count >= 4 ? EPI_OK : reject(why, "inks do not span three dimensions");
}

/* Q16 normals and Q32 offsets, each offset lowered just enough that every ink,
 * quantised to Q16, tests inside its face in exact integer arithmetic. */
static void hull_quantise(const hull_t* h, double xyz[][3], size_t n, epi_colour_tables_t* t) {
    t->hull_face_count = (uint8_t)h->count;
    for (unsigned f = 0; f < h->count; f++) {
        for (int c = 0; c < 3; c++) {
            t->hull_n[f][c] = q16(h->n[f][c]);
        }
        int64_t worst = INT64_MIN;
        for (size_t i = 0; i < n; i++) {
            const int32_t x[3] = {q16(xyz[i][0]), q16(xyz[i][1]), q16(xyz[i][2])};
            const int64_t s = epi_dot3(t->hull_n[f], x);
            worst = s > worst ? s : worst;
        }
        const int64_t d = q_round(h->d[f], Q32);
        t->hull_d[f] = d < -worst ? d : -worst;
    }
}

/*
 * M_adapt = diag(k)·A·M_srgb, with A the Bradford matrix from D65 to the white
 * ink's chromaticity and k the black-point scale, and M_bpc = (W_y − B_y)·M_srgb.
 * Source white must land exactly on the white ink, and rounding entry by entry
 * can miss a row's sum by an LSB, which would put white just outside the hull's
 * white vertex: each row's residual goes on its largest-magnitude entry.
 *
 * Every entry fits Q16 for any inks. XYZ is M_srgb times a non-negative colour, so
 * X/Y is in [0.5, 2.5] and Z/Y in [0.09, 13.2]. Those keep |adapt_lin| under 12 and
 * b/w under 146 in each of X, Y and Z, and k = W_y·(1 − b/w) with W_y at most 1, so
 * an entry is under 1,800 in magnitude.
 */
static void adaptation_matrices(const double w[3], const double b[3], epi_colour_tables_t* t) {
    const double neutral[3] = {w[0] / w[1], 1.0, w[2] / w[1]};
    const double one[3] = {1.0, 1.0, 1.0};
    double d65[3], rs[3], rd[3], adapted_white[3];
    mat3_vec(&M_SRGB, one, d65);
    mat3_vec(&M_BRADFORD, d65, rs);
    mat3_vec(&M_BRADFORD, neutral, rd);
    const mat3_t scale = {{{rd[0] / rs[0], 0.0, 0.0}, {0.0, rd[1] / rs[1], 0.0}, {0.0, 0.0, rd[2] / rs[2]}}};
    const mat3_t inv = mat3_inverse(&M_BRADFORD);
    const mat3_t scaled = mat3_mul(&scale, &M_BRADFORD);
    const mat3_t adapt = mat3_mul(&inv, &scaled);
    const mat3_t adapt_lin = mat3_mul(&adapt, &M_SRGB);
    mat3_vec(&adapt, d65, adapted_white);

    for (int r = 0; r < 3; r++) {
        const double k = (w[r] - b[r]) / adapted_white[r];
        int32_t* row = t->lin_to_panel_adapt[r];
        int largest = 0;
        for (int c = 0; c < 3; c++) {
            row[c] = q16(k * adapt_lin.m[r][c]);
            largest = epi_abs32(row[c]) > epi_abs32(row[largest]) ? c : largest;
            t->lin_to_panel_bpc[r][c] = q16((w[1] - b[1]) * M_SRGB.m[r][c]);
        }
        row[largest] += (int32_t)((int64_t)t->white_xyz[r] - t->black_xyz[r] - row[0] - row[1] - row[2]);
    }
}

/*
 * L*min and L*max: the ends of the segment of the neutral axis, the ray
 * Y·W/W_y, inside the hull. Shrunk a hair so that they test inside. W is on the
 * ray and no ink is above it in Y, so the segment ends at W_y at most, which keeps
 * L*max below 100. The ray lies in the plane of a face through a neutral black and
 * white, where k and d are both rounding noise, so such a face bounds nothing.
 */
static bool neutral_lstar(const hull_t* h, const double w[3], double* lmin, double* lmax) {
    const double neutral[3] = {w[0] / w[1], 1.0, w[2] / w[1]};
    double lo = 0.0, hi = w[1];
    for (unsigned f = 0; f < h->count; f++) {
        const double k = h->n[f][0] * neutral[0] + h->n[f][1] * neutral[1] + h->n[f][2] * neutral[2];
        if (k > HULL_SIDE_TOL) {
            const double y = -h->d[f] / k;
            hi = y < hi ? y : hi;
        } else if (k < -HULL_SIDE_TOL) {
            const double y = -h->d[f] / k;
            lo = y > lo ? y : lo;
        } else if (h->d[f] > HULL_TOL) {
            return false;
        }
    }
    if (lo > hi) {
        return false;
    }
    *lmin = 116.0 * epi_lab_f_d((lo + NEUTRAL_EPS) / w[1]) - 16.0;
    *lmax = 116.0 * epi_lab_f_d((hi - NEUTRAL_EPS) / w[1]) - 16.0;
    return true;
}

/* Tests the faces cyclically from *hint and leaves in *hint the face that
 * rejected the point, like hull_inside (src/epi_intent.c): the result is the AND
 * of every face test, whatever the order. */
static inline bool hull_contains(const hull_t* h, double x, double y, double z, unsigned* hint) {
    unsigned f = *hint;
    for (unsigned i = 0; i < h->count; i++) {
        if (h->n[f][0] * x + h->n[f][1] * y + h->n[f][2] * z + h->d[f] > HULL_TOL) {
            *hint = f;
            return false;
        }
        f = f + 1 == h->count ? 0 : f + 1;
    }
    return true;
}

/*
 * The in-hull chroma limit at (L*, hue), as a fraction of the probe's chroma:
 * bisects a probe at constant L* and hue, keeping the inside end; negative if the
 * probe itself isn't outside the hull. fx = fy + s·dfx, fz = fy − s·dfz, where
 * dfx = C·cos(h)/500 and dfz = C·sin(h)/200 for the probe's C.
 *
 * The result stays below hi, so the search gives up with 0 once hi falls to
 * at_least, the smallest limit the caller can use. Testing at_least directly would
 * skip midpoints and could change the result: near a cusp a ray can leave the hull
 * and come back, and the result is wherever these midpoints lead.
 */
static double
max_chroma(const hull_t* h, const double w[3], double lstar, double dfx, double dfz, double at_least, unsigned* hint) {
    const double fy = (lstar + 16.0) / 116.0;
    const double y = w[1] * epi_lab_finv_d(fy);
    double lo = 0.0, hi = 1.0;
    for (int i = 0; i < CUSP_BISECT_ITERS; i++) {
        const double mid = 0.5 * (lo + hi);
        const double x = w[0] * epi_lab_finv_d(fy + mid * dfx);
        const double z = w[2] * epi_lab_finv_d(fy - mid * dfz);
        if (hull_contains(h, x, y, z, hint)) {
            lo = mid;
        } else {
            hi = mid;
            if (hi <= at_least) {
                return 0.0;
            }
        }
    }
    return hi == 1.0 ? -1.0 : lo;
}

/* A sample of the first pass of the cusp search. */
static double coarse_l(double lmin, double lmax, double step, int i) {
    return i == CUSP_L_SAMPLES - 1 ? lmax : lmin + i * step;
}

/*
 * For each integer hue, the L* of maximum in-hull chroma: 64 samples over the
 * neutral interval, then 4 refinement passes of 17 samples spanning the best
 * sample's neighbours, each narrowing the step by 8 (the ridge has a kink at the
 * cusp), the first maximum winning. A pass starts from a sample whose limit is
 * known, so that max_chroma can give up on the others: the previous pass's best,
 * its centre, or else the sample that won the previous hue's first pass. A panel
 * that fails stops at the first bad hue.
 */
static epi_err_t cusp_table(
    const hull_t* h,
    const double w[3],
    double lmin,
    double lmax,
    epi_colour_tables_t* t,
    const char** why,
    epi_progress_fn_t progress,
    void* progress_ctx
) {
    const double coarse_step = (lmax - lmin) / (CUSP_L_SAMPLES - 1);
    unsigned hint = 0;
    int coarse_best = -1; /* the sample that won the previous hue's first pass */
    for (int hue = 0; hue < 360; hue++) {
        const double r = hue * PI / 180.0;
        const double dfx = CUSP_C_PROBE * epi_sin_d(r + PI / 2.0) / 500.0; /* cos(r) */
        const double dfz = CUSP_C_PROBE * epi_sin_d(r) / 200.0;
        double step = coarse_step;
        double best_l = lmin, best_c = 0.0;
        for (int pass = 0; pass <= CUSP_REFINE_PASSES; pass++) {
            const double centre = best_l;
            const int samples = pass == 0 ? CUSP_L_SAMPLES : CUSP_REFINE;
            const int known = pass == 0 ? coarse_best : CUSP_REFINE / 2;
            if (pass == 0 && known >= 0) {
                best_c = max_chroma(h, w, coarse_l(lmin, lmax, coarse_step, known), dfx, dfz, 0.0, &hint);
                if (best_c < 0.0) {
                    return reject(why, "ink hull reaches beyond chroma 200");
                }
            }
            const double known_c = best_c;
            double at_least = best_c;
            for (int i = 0; i < samples; i++) {
                double l;
                if (pass == 0) {
                    l = coarse_l(lmin, lmax, coarse_step, i);
                } else {
                    l = centre + step * (-1.0 + 2.0 * i / (CUSP_REFINE - 1));
                    l = l < lmin ? lmin : (l > lmax ? lmax : l);
                }
                const double c = i == known ? known_c : max_chroma(h, w, l, dfx, dfz, at_least, &hint);
                if (c < 0.0) {
                    return reject(why, "ink hull reaches beyond chroma 200");
                }
                if (c >= at_least) {
                    best_c = c;
                    best_l = l;
                    at_least = c + CUSP_C_LSB;
                    coarse_best = pass == 0 ? i : coarse_best;
                }
            }
            if (pass > 0) {
                step *= 2.0 / (CUSP_REFINE - 1);
            }
        }
        const int32_t cusp = (int32_t)q_round(best_l, Q8);
        if (cusp < t->lstar_min + EPI_CUSP_MARGIN_Q8 || cusp > t->lstar_max - EPI_CUSP_MARGIN_Q8) {
            return reject(why, "a hue peaks too close to the end of the neutral range; often a strongly tinted black");
        }
        t->cusp_lstar[hue] = (uint16_t)cusp;
        if (progress != NULL && !progress(progress_ctx, (unsigned)hue + 1, 360)) {
            return EPI_ERR_CANCELLED;
        }
    }
    return EPI_OK;
}

/* The hull from the inks' XYZ, in its Q16 form too. W and B are the first inks of
 * largest and smallest Y. */
static epi_err_t inks_hull(
    const epi_ink_t* inks,
    size_t n,
    hull_t* h,
    epi_colour_tables_t* t,
    size_t* iw,
    size_t* ib,
    double w[3],
    double b[3],
    const char** why
) {
    double xyz[EPI_MAX_INKS][3];
    *ib = 0;
    *iw = 0;
    for (size_t i = 0; i < n; i++) {
        double lin[3];
        ink_linear(&inks[i], lin);
        mat3_vec(&M_SRGB, lin, xyz[i]);
        *iw = xyz[i][1] > xyz[*iw][1] ? i : *iw;
        *ib = xyz[i][1] < xyz[*ib][1] ? i : *ib;
    }
    const epi_err_t err = hull_build(xyz, n, h, why);
    if (err != EPI_OK) {
        return err;
    }
    hull_quantise(h, xyz, n, t);
    memcpy(w, xyz[*iw], sizeof xyz[0]);
    memcpy(b, xyz[*ib], sizeof xyz[0]);
    return EPI_OK;
}

/* Each ink's linear sRGB and ink-relative OKLab: OKLab_ink(v) = M2·cbrt(M1·(v/w)),
 * which is exactly (1, 0, 0) for white. Every cone response is checked against
 * epi_cbrt_d's domain. */
static bool ink_tables(const epi_ink_t* inks, size_t n, const double wl[3], epi_colour_tables_t* t) {
    for (size_t i = 0; i < n; i++) {
        double lin[3], lms[3], oklab[3];
        ink_linear(&inks[i], lin);
        for (int c = 0; c < 3; c++) {
            t->ink_lin[i][c] = q16(lin[c]);
            lin[c] /= wl[c];
        }
        mat3_vec(&OK_M1, lin, lms);
        for (int c = 0; c < 3; c++) {
            if (lms[c] != 0.0 && !(lms[c] >= 0x1p-16 && lms[c] < 2.0)) {
                return false;
            }
            lms[c] = epi_cbrt_d(lms[c]);
        }
        mat3_vec(&OK_M2, lms, oklab);
        for (int c = 0; c < 3; c++) {
            t->ink_oklab[i][c] = q16(oklab[c]);
        }
    }
    return true;
}

/* The neutral-cast offsets at integer L*: the knots (valid_config's rules)
 * interpolated linearly, zero outside the first and last. */
static void neutral_table(const epi_neutral_knot_t* knots, size_t n, epi_colour_tables_t* t) {
    t->has_neutral = n > 0;
    for (int i = 0; i < EPI_NEUTRAL_LEN && n > 0; i++) {
        const double l = i;
        if (l < knots[0].lstar || l > knots[n - 1].lstar) {
            continue;
        }
        size_t k = 0;
        while (k + 1 < n && knots[k + 1].lstar <= l) {
            k++;
        }
        double a = knots[k].a, b = knots[k].b;
        if (k + 1 < n) {
            const double f = (l - knots[k].lstar) / ((double)knots[k + 1].lstar - knots[k].lstar);
            a += f * ((double)knots[k + 1].a - knots[k].a);
            b += f * ((double)knots[k + 1].b - knots[k].b);
        }
        t->neutral_q8[i][0] = (int16_t)q_round(a, Q8);
        t->neutral_q8[i][1] = (int16_t)q_round(b, Q8);
    }
}

/*
 * Derives the colour tables of n inks (at most 16) from their measured colours, and
 * the neutral-cast table from knot_count knots (valid_config's rules). *t
 * must be zeroed; everything but the detail kernel is written. Returns EPI_OK;
 * EPI_ERR_INVALID_ARG if the inks can't make a panel, with a short static reason in
 * *why; or EPI_ERR_CANCELLED if progress (optional) returned false. *t is unspecified
 * after an error.
 */
static epi_err_t epi_colour_tables_derive(
    const epi_ink_t* inks,
    size_t n,
    const epi_neutral_knot_t* knots,
    size_t knot_count,
    epi_colour_tables_t* t,
    const char** why,
    epi_progress_fn_t progress,
    void* progress_ctx
) {
    neutral_table(knots, knot_count, t);
    hull_t h;
    size_t iw, ib;
    double w[3], b[3];
    const epi_err_t err = inks_hull(inks, n, &h, t, &iw, &ib, w, b, why);
    if (err != EPI_OK) {
        return err;
    }
    /* White and black are the lightest and darkest measured; the nominal colours
     * must agree. */
    double nominal_y_max = 0.0, nominal_y_min = 1.0;
    for (size_t i = 0; i < n; i++) {
        const double y = nominal_y(&inks[i]);
        nominal_y_max = y > nominal_y_max ? y : nominal_y_max;
        nominal_y_min = y < nominal_y_min ? y : nominal_y_min;
    }
    if (nominal_y(&inks[iw]) < nominal_y_max) {
        return reject(why, "lightest measured ink is not the white ink");
    }
    if (nominal_y(&inks[ib]) > nominal_y_min) {
        return reject(why, "darkest measured ink is not the black ink");
    }
    t->base.kind = EPI_PANEL_COLOUR;
    t->base.ink_count = (uint8_t)n;
    for (int c = 0; c < 3; c++) {
        t->white_xyz[c] = q16(w[c]);
        t->black_xyz[c] = q16(b[c]);
        /* Above 512 keeps the reciprocal in int32 (and W_y from dividing by zero). */
        if (t->white_xyz[c] <= 512) {
            return reject(why, "white ink too dark");
        }
    }
    if (t->black_xyz[1] >= t->white_xyz[1]) {
        return reject(why, "black ink not darker than the white ink");
    }
    double wl[3];
    ink_linear(&inks[iw], wl);
    if (wl[0] == 0.0 || wl[1] == 0.0 || wl[2] == 0.0) {
        return reject(why, "white ink has a zero channel");
    }
    if (!ink_tables(inks, n, wl, t)) {
        return reject(why, "ink outside the range the white ink allows");
    }
    adaptation_matrices(w, b, t);
    const mat3_t m_inv = mat3_inverse(&M_SRGB);
    const double wl_len = epi_sqrt_d(wl[0] * wl[0] + wl[1] * wl[1] + wl[2] * wl[2]);
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            t->xyz_to_lin[r][c] = q16(m_inv.m[r][c]);
            t->lin_to_lms[r][c] = q16(OK_M1.m[r][c] / wl[c]);
            t->lms_to_oklab[r][c] = q16(OK_M2.m[r][c]);
        }
        t->nhat[r] = q16(wl[r] / wl_len);
    }

    double lmin, lmax;
    if (!neutral_lstar(&h, w, &lmin, &lmax)) {
        return reject(why, "neutral axis misses the ink hull");
    }
    t->lstar_min = (int32_t)q_round(lmin, Q8);
    t->lstar_max = (int32_t)q_round(lmax, Q8);
    if (t->lstar_min < 0) {
        return reject(why, "darkest neutral L* is below 0");
    }
    if (!epi_midtone_monotone(t->lstar_min)) {
        return reject(why, "darkest neutral L* is too light for the tone curve");
    }
    return cusp_table(&h, w, lmin, lmax, t, why, progress, progress_ctx);
}

/* The structure of cfg; epi_colour_tables_derive judges the inks themselves.
 * NULL when valid, else why not. */
static const char* valid_config(const epi_colour_panel_config_t* cfg) {
    if (cfg == NULL) {
        return "config is NULL";
    }
    if (cfg->name == NULL || cfg->inks == NULL) {
        return "name or inks is NULL";
    }
    if (cfg->width < 1 || cfg->height < 1) {
        return "zero width or height";
    }
    if (cfg->ink_count < 5 || cfg->ink_count > EPI_MAX_INKS) {
        return "a colour panel needs 5 to 16 inks";
    }
    /* Sigma: (0, 10.5]; false for NaN; radius ⌈3σ⌉ ≤ 32 = EPI_MAX_KERNEL_RADIUS. */
    if (!(cfg->detail_sigma > 0.0f && cfg->detail_sigma <= 10.5f)) {
        return "detail sigma outside (0, 10.5]";
    }
    if (cfg->neutral_count > EPI_MAX_NEUTRAL_KNOTS) {
        return "more than 16 neutral knots";
    }
    if ((cfg->neutral == NULL) != (cfg->neutral_count == 0)) {
        return "neutral knots and neutral count disagree";
    }
    /* The range tests are false for NaN, and exclude infinities. */
    for (size_t i = 0; i < cfg->neutral_count; i++) {
        const epi_neutral_knot_t* k = &cfg->neutral[i];
        if (!(k->lstar >= 0.0f && k->lstar <= 100.0f)) {
            return "neutral knot L* outside [0, 100]";
        }
        if (!(k->a >= -30.0f && k->a <= 30.0f && k->b >= -30.0f && k->b <= 30.0f)) {
            return "neutral knot a* or b* outside [-30, 30]";
        }
        if (i > 0 && !(k->lstar > cfg->neutral[i - 1].lstar)) {
            return "neutral knot L* not strictly ascending";
        }
    }
    return NULL;
}

size_t epi_colour_panel_size(const epi_colour_panel_config_t* cfg) {
    return valid_config(cfg) != NULL ? 0 : COLOUR_STORAGE_SIZE;
}

epi_err_t epi_colour_panel_init(
    const epi_colour_panel_config_t* cfg,
    void* storage,
    size_t size,
    const epi_panel_t** out,
    const char** reason
) {
    if (out != NULL) {
        *out = NULL;
    }
    if (reason != NULL) {
        *reason = NULL;
    }
    const char* invalid = valid_config(cfg);
    if (invalid == NULL && (storage == NULL || out == NULL)) {
        invalid = "storage or out is NULL";
    }
    if (invalid != NULL) {
        if (reason != NULL) {
            *reason = invalid;
        }
        return EPI_ERR_INVALID_ARG;
    }
    if (size < COLOUR_STORAGE_SIZE) {
        return EPI_ERR_WORKSPACE_TOO_SMALL;
    }
    colour_storage_t* s = epi_align_ptr(storage, COLOUR_ALIGN);
    /* cfg->inks may alias this storage (re-init from the panel's own inks):
     * take the inks before the memset. */
    const size_t n = cfg->ink_count;
    epi_ink_t inks[EPI_MAX_INKS];
    memcpy(inks, cfg->inks, n * sizeof inks[0]);
    memset(s, 0, sizeof *s);
    memcpy(s->inks, inks, n * sizeof s->inks[0]);

    epi_colour_tables_t* t = &s->tables;
    const char* why;
    const epi_err_t err = epi_colour_tables_derive(
        s->inks, n, cfg->neutral, cfg->neutral_count, t, &why, cfg->progress, cfg->progress_ctx
    );
    if (err != EPI_OK) {
        if (err == EPI_ERR_INVALID_ARG && reason != NULL) {
            *reason = why;
        }
        return err;
    }
    t->base.kernel_radius = (uint8_t)epi_gaussian_kernel_q15(cfg->detail_sigma, t->base.kernel);

    epi_panel_t* p = &s->panel;
    p->name = cfg->name;
    p->width = cfg->width;
    p->height = cfg->height;
    p->inks = s->inks;
    p->ink_count = n;
    p->detail_sigma = cfg->detail_sigma;
    p->presets = epi_colour_presets;
    p->preset_count = EPI_COLOUR_PRESET_COUNT;
    p->tables = &t->base;
    *out = p;
    return EPI_OK;
}
