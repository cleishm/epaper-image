/*
 * Stages S1–S6 (docs/design.md, "Rendering intent"): see epi_intent.h.
 *
 * Fixed-point formats (Qn = n fractional bits; every rounding is half away
 * from zero, via epi_round_shift or div_round_i32):
 *   linear light, XYZ           Q16 (products with Q16 matrices are Q32, int64)
 *   L*, a*, b*                  Q8, int32 (int16 only for L*src, base, blur rows)
 *   CIELAB ratio t = X/Xw       Q25 (forward) / Q24 (inverse)
 *   CIELAB f(t)                 Q19 (forward) / Q24 (inverse)
 *   D, gains, smoothsteps       Q16
 *   cusp-move L*, C* and anchor Q16 L* units
 *
 * The hot path (everything except epi_intent_init and the grey-table
 * helpers) is integer-only. Init uses double with + − × ÷ and epi_cbrt_d
 * only (design.md "Fixed-point arithmetic and determinism").
 *
 * Grey panels (design.md "Grey panels") share S1 (lstar_src), S2 and S3 (tone_detail, with
 * the tone table built by the same code from T0 with W_y = 1 and
 * B_y = level_y[0]), the veil, and the CIELAB f⁻¹ of S5; there is no
 * adaptation, vibrance, neutral cast or hull.
 */
#include "epi_intent.h"

#include "epi_fp.h"
#include "epi_intent_stages.h"
#include "epi_math.h"
#include "epi_srgb.h"

#include <stddef.h>
#include <string.h>

/* ---- integer helpers ---------------------------------------------------- */

/* x / d rounded half away from zero, d > 0, |x| + d/2 < 2^31. 32-bit
 * division by a constant (compilers emit a multiply-high). */
static inline int32_t div_round_i32(int32_t x, int32_t d) {
    if (x >= 0) {
        return (x + d / 2) / d;
    }
    return -((-x + d / 2) / d);
}

static inline int32_t clamp_i32(int32_t x, int32_t lo, int32_t hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

static inline void copy3(int32_t dst[3], const int32_t src[3]) {
    dst[0] = src[0];
    dst[1] = src[1];
    dst[2] = src[2];
}

/* ---- CIELAB f and f⁻¹ ---------------------------------------------- */

/* (6/29)^3 in Q25, and 4/29 in Q19 and Q24, 6/29 in Q24. */
#define LAB_EPS_Q25 297173
#define LAB_4_29_Q19 72316
#define LAB_DELTA_Q24 3471148
#define LAB_4_29_Q24 2314099
/* 3·(6/29)² = 108/841 in Q30. */
#define LAB_K_Q30 INT64_C(137888367)

/* f(t), t in Q25 → Q19. Above (6/29)³ this is epi_cbrt_q16 directly:
 * cbrt(t·2^25 / 2^16)·2^16 = cbrt(t)·2^19. Below, f = t·841/108 + 4/29;
 * t is floored at −64 so the 32-bit product cannot overflow (only reached
 * by meaningless inputs; panel and source XYZ are ≥ 0). */
static inline int32_t lab_f_q19(int32_t t_q25) {
    if (t_q25 > LAB_EPS_Q25) {
        return epi_cbrt_q16(t_q25);
    }
    if (t_q25 < -(1 << 21)) {
        t_q25 = -(1 << 21);
    }
    return div_round_i32(t_q25 * 841, 108 * 64) + LAB_4_29_Q19;
}

/* f⁻¹(f), f in Q24 → t in Q24. */
static inline int64_t lab_finv_q24(int32_t f_q24) {
    if (f_q24 > LAB_DELTA_Q24) {
        int64_t f2 = epi_round_shift((int64_t)f_q24 * f_q24, 24);
        return epi_round_shift(f2 * f_q24, 24);
    }
    return epi_round_shift((int64_t)(f_q24 - LAB_4_29_Q24) * LAB_K_Q30, 30);
}

/* fy = (L + 16)/116 of L* Q8 → Q24: (L_q8 + 4096)·2^14 / 29. */
static inline int32_t lab_fy_q24(int32_t l_q8) {
    return div_round_i32((l_q8 + 16 * 256) * 16384, 29);
}

/* L* Q8 from f(Y/Yn) Q19: 116·f − 16. */
static inline int32_t lstar_from_f(int32_t fy_q19) {
    return (int32_t)epi_round_shift((int64_t)116 * fy_q19, 11) - 16 * 256;
}

/* ---- S1 ----------------------------------------------------------------- */

/* Linear light, Q16, as int32: the table's values are 0..65536, so this is
 * exact, and products with the int32 matrices are signed 32 × 32 (on
 * 32-bit targets, an unsigned operand costs an extra multiply). */
static inline void srgb_lin(const epi_rgb_t* p, int32_t lin[3]) {
    lin[0] = (int32_t)epi_srgb_to_lin_q16[p->r];
    lin[1] = (int32_t)epi_srgb_to_lin_q16[p->g];
    lin[2] = (int32_t)epi_srgb_to_lin_q16[p->b];
}

/* L*src Q8 (source Y, white Y = 1): 0..25600 for every sRGB8 colour, since Y
 * is 0..1 (Q25 up to 2^25). */
static inline int32_t lstar_src(const int32_t lin[3]) {
    int64_t y_q32 = epi_dot3(epi_srgb_to_xyz_q16[1], lin);
    return lstar_from_f(lab_f_q19((int32_t)epi_round_shift(y_q32, 7)));
}

/* D = adapt + (1 − adapt)·ss(L*src, 75, 95), Q16 (fixed hi = 1). */
static inline int32_t adapt_degree(const epi_intent_ctx_t* ctx, int32_t lsrc_q8) {
    int32_t ss = epi_smoothstep_q16(lsrc_q8, 75 * 256, 95 * 256);
    return ctx->adapt_q16 + epi_mul_q16(EPI_Q16_ONE - ctx->adapt_q16, ss);
}

/* Panel CIELAB Q8: panel = M_bpc·lin + D·(M_adapt·lin − M_bpc·lin) + B,
 * in Q32 (M_adapt's rows are made to sum to W − B, so source white lands
 * exactly on the white ink); ratio to the white ink by the init-time
 * reciprocal (no division). */
static inline void panel_lab(const epi_intent_ctx_t* ctx, const int32_t lin[3], int32_t d_q16, int32_t lab[3]) {
    const epi_colour_tables_t* t = epi_colour_tables(ctx->tables);
    int32_t f[3];
    for (int i = 0; i < 3; i++) {
        int64_t pa = epi_dot3(t->lin_to_panel_adapt[i], lin);
        int64_t pb = epi_dot3(t->lin_to_panel_bpc[i], lin);
        int64_t p_q32 = pb + epi_round_shift((pa - pb) * d_q16, 16) + (int64_t)t->black_xyz[i] * EPI_Q16_ONE;
        int32_t r_q25 = epi_sat_i32(epi_round_shift(p_q32 * ctx->white_recip[i], 31));
        f[i] = lab_f_q19(r_q25);
    }
    lab[0] = lstar_from_f(f[1]);
    lab[1] = (int32_t)epi_round_shift((int64_t)500 * (f[0] - f[1]), 11);
    lab[2] = (int32_t)epi_round_shift((int64_t)200 * (f[1] - f[2]), 11);
}

/* ---- S3 ----------------------------------------------------------------- */

/* T(base) + g(base)·(L*src − base), T and g interpolated linearly between
 * the 1/4-L* knots (64 Q8 LSB apart). */
static inline int32_t tone_detail(const int32_t* tone_t, const int32_t* tone_g, int32_t base_q8, int32_t lsrc_q8) {
    int32_t b = clamp_i32(base_q8, 0, 100 * 256);
    int32_t i = b >> 6;
    int32_t fr = b & 63;
    if (i >= EPI_TONE_TABLE_LEN - 1) {
        i = EPI_TONE_TABLE_LEN - 2;
        fr = 64;
    }
    int32_t t0 = tone_t[i], t1 = tone_t[i + 1];
    int32_t g0 = tone_g[i], g1 = tone_g[i + 1];
    int32_t tv = t0 + (int32_t)epi_round_shift((int64_t)(t1 - t0) * fr, 6);
    int32_t gv = g0 + (int32_t)epi_round_shift((int64_t)(g1 - g0) * fr, 6);
    return tv + epi_mul_q16(gv, lsrc_q8 - base_q8);
}

/* ---- S4 ----------------------------------------------------------------- */

/* Vibrance: (a, b) ·= 1 + (v − 1)·ss(C, 4, 14)·(1 − skin). Hue is in
 * [0, 360); a hue in (−180, 180] would give the same skin weight, since
 * 1 − ss(h, 60, 75) is 0 for every h ≥ 75. */
static inline void vibrance(const epi_intent_ctx_t* ctx, int32_t lab[3]) {
    int32_t c = epi_hypot_q(lab[1], lab[2]);
    int32_t h = epi_atan2_deg_q8(lab[2], lab[1]);
    int32_t skin_h =
        epi_mul_q16(epi_smoothstep_q16(h, 10 * 256, 25 * 256), EPI_Q16_ONE - epi_smoothstep_q16(h, 60 * 256, 75 * 256));
    int32_t skin = epi_mul_q16(skin_h, EPI_Q16_ONE - epi_smoothstep_q16(c, 30 * 256, 45 * 256));
    int32_t w = epi_mul_q16(epi_smoothstep_q16(c, 4 * 256, 14 * 256), EPI_Q16_ONE - skin);
    int32_t gain = EPI_Q16_ONE + epi_mul_q16(ctx->vibrance_q16 - EPI_Q16_ONE, w);
    lab[1] = epi_mul_q16(lab[1], gain);
    lab[2] = epi_mul_q16(lab[2], gain);
}

/* Veil: L* −= veil·ss(L*, 90, 100), Q8; both pipelines. */
static inline int32_t veil_lstar(const epi_intent_ctx_t* ctx, int32_t l_q8) {
    if (ctx->veil_q8 > 0) {
        l_q8 -= epi_mul_q16(ctx->veil_q8, epi_smoothstep_q16(l_q8, 90 * 256, 100 * 256));
    }
    return l_q8;
}

/* 1 − ss(C*, 12, 32), Q16: the share a colour takes of the lift (S1) and of the neutral-cast correction, 1 for low
 * chroma down to 0 for saturated. The rounded hypot is at most k exactly when a² + b² ≤ k·(k + 1)
 * (epi_isqrt64_round), so most colours skip the root. */
static inline int32_t low_chroma_share(const int32_t lab[3]) {
    const uint64_t lo = 12 * 256, hi = 32 * 256;
    uint64_t ua = epi_abs32(lab[1]);
    uint64_t ub = epi_abs32(lab[2]);
    uint64_t c2 = ua * ua + ub * ub;
    if (c2 <= lo * (lo + 1)) {
        return EPI_Q16_ONE;
    }
    if (c2 > (hi - 1) * hi) {
        return 0;
    }
    return EPI_Q16_ONE - epi_smoothstep_q16(epi_hypot_q(lab[1], lab[2]), (int32_t)lo, (int32_t)hi);
}

/* Lift, S1: the source colour scaled at constant chromaticity to the lifted L*, where Y scales by
 * ((L' + 16)/(L + 16))³, clamped so that lin stays in S1's range. The lift is 0 from L* 82 and the curve is monotone,
 * so the Q16 ratio's numerator fits. Returns whether lin changed. */
static inline int lift_colour(const epi_intent_ctx_t* ctx, int32_t lsrc_q8, int32_t share, int32_t lin[3]) {
    if (share == 0) {
        return 0;
    }
    int32_t w = epi_mul_q16(
        epi_smoothstep_q16(lsrc_q8, 0, 35 * 256), EPI_Q16_ONE - epi_smoothstep_q16(lsrc_q8, 45 * 256, 82 * 256)
    );
    int32_t lift = epi_mul_q16(ctx->lift_q8, epi_mul_q16(w, share));
    if (lift == 0) {
        return 0;
    }
    int32_t g = div_round_i32((lsrc_q8 + lift + 16 * 256) * 65536, lsrc_q8 + 16 * 256);
    int32_t k = epi_mul_q16(epi_mul_q16(g, g), g);
    for (int i = 0; i < 3; i++) {
        lin[i] = clamp_i32(epi_mul_q16(lin[i], k), 0, EPI_Q16_ONE);
    }
    return 1;
}

/* ---- neutral cast ------------------------------------------------------- */

/* (a, b) += share·n(L*), n the panel's offsets at integer L* (Q8) interpolated linearly; L* is unchanged. */
static inline void neutral_cast(const epi_colour_tables_t* t, int32_t share, int32_t lab[3]) {
    int32_t l = clamp_i32(lab[0], 0, 100 * 256);
    int32_t i = l >> 8;
    int32_t fr = l & 255;
    if (i >= EPI_NEUTRAL_LEN - 1) {
        i = EPI_NEUTRAL_LEN - 2;
        fr = 256;
    }
    const int16_t* n0 = t->neutral_q8[i];
    const int16_t* n1 = t->neutral_q8[i + 1];
    int32_t da = n0[0] + (int32_t)epi_round_shift((int64_t)(n1[0] - n0[0]) * fr, 8);
    int32_t db = n0[1] + (int32_t)epi_round_shift((int64_t)(n1[1] - n0[1]) * fr, 8);
    lab[1] += epi_mul_q16(share, da);
    lab[2] += epi_mul_q16(share, db);
}

/* ---- S5 ----------------------------------------------------------------- */

/* CIELAB Q8 → XYZ Q16 (reference white = the white ink). The f values are
 * exact-rational divisions by constants in 32 bits:
 *   fy = (L + 16)/116                               (Q24, lab_fy_q24)
 *   a/500             = a_q8·2^14 / 125
 *   b/200             = b_q8·2^13 / 25
 * L is clamped to ±256 and a, b to ±512 L* so the products fit int32. */
static inline void lab_to_xyz(const epi_colour_tables_t* t, const int32_t lab[3], int32_t xyz[3]) {
    const int32_t ab_max = (1 << 17) - 1;
    int32_t l = clamp_i32(lab[0], -(1 << 16), 1 << 16);
    int32_t a = clamp_i32(lab[1], -ab_max, ab_max);
    int32_t b = clamp_i32(lab[2], -ab_max, ab_max);
    int32_t fy = lab_fy_q24(l);
    int32_t f[3];
    f[0] = fy + div_round_i32(a * 16384, 125);
    f[1] = fy;
    f[2] = fy - div_round_i32(b * 8192, 25);
    for (int i = 0; i < 3; i++) {
        int64_t r_q24 = lab_finv_q24(f[i]);
        xyz[i] = epi_sat_i32(epi_round_shift((int64_t)t->white_xyz[i] * r_q24, 24));
    }
}

/* Inside iff n·xyz + d ≤ 0 on every face (Q16 × Q16 = Q32, plus d Q32). */
static inline int64_t hull_face(const epi_colour_tables_t* t, unsigned f, const int32_t xyz[3]) {
    return epi_dot3(t->hull_n[f], xyz) + t->hull_d[f];
}

/* Tests the faces cyclically from *hint (0 for a new row) and leaves in *hint
 * the face that rejected xyz (unchanged when inside). The result is the AND of
 * every face test, whatever the order; a hull without faces contains xyz. */
static inline int hull_inside(const epi_colour_tables_t* t, const int32_t xyz[3], unsigned* hint) {
    const unsigned n = t->hull_face_count;
    unsigned f = *hint;
    for (unsigned i = 0; i < n; i++) {
        if (hull_face(t, f, xyz) > 0) {
            *hint = f;
            return 0;
        }
        f = f + 1 == n ? 0 : f + 1;
    }
    return 1;
}

/* P(s) = (L0 + s·(L − L0), s·a, s·b), s Q16, L and L0 in Q16 L*. Equal to
 * (L0 + s(L − L0), s·C, h) in LCh, without trigonometry.
 * Each component is a rounded linear function of s, so monotone in s. */
static inline void cusp_lab(int32_t s, int32_t l0_q16, int32_t l_q16, int32_t a, int32_t b, int32_t p[3]) {
    int32_t ls = l0_q16 + epi_mul_q16(s, l_q16 - l0_q16);
    p[0] = (int32_t)epi_round_shift(ls, 8);
    p[1] = epi_mul_q16(s, a);
    p[2] = epi_mul_q16(s, b);
}

static inline int lab_eq(const int32_t p[3], const int32_t q[3]) {
    return p[0] == q[0] && p[1] == q[1] && p[2] == q[2];
}

/* Cusp move of an out-of-hull CIELAB (Q8) to xyz (Q16). */
static void cusp_move(const epi_colour_tables_t* t, const int32_t lab[3], int32_t xyz[3], unsigned* hint) {
    const int64_t lmin = (int64_t)t->lstar_min << 8, lmax = (int64_t)t->lstar_max << 8;
    const int64_t span = lmax - lmin;
    int32_t a = lab[1], b = lab[2];
    const int32_t lq = clamp_i32(lab[0], t->lstar_min, t->lstar_max); /* L*, Q8 */
    int64_t l = (int64_t)lq << 8;
    int32_t p_lo[3] = {0, 0, 0}, p_hi[3] = {lq, a, b};

    /* s = 1 when the L*-clamped pixel is already inside. P(1) is exactly
     * p_hi whatever L0 (ls = L0 + (L − L0)), so when the clamp is a no-op it
     * is lab itself, which the caller found outside. */
    if (lq != lab[0]) {
        lab_to_xyz(t, p_hi, xyz);
        if (hull_inside(t, xyz, hint)) {
            return;
        }
    }

    /* Anchor (Ottosson's adaptive L0, α = .05), in Q16 L* units: the rule is
     * homogeneous in the span except for α·C′/k, which becomes α·C·span/k. */
    int64_t c = (int64_t)epi_hypot_q(a, b) << 8;
    int32_t h = epi_atan2_deg_q8(b, a);
    int32_t hi = h >> 8, hf = h & 255;
    int32_t c0 = t->cusp_lstar[hi], c1 = t->cusp_lstar[hi == 359 ? 0 : hi + 1];
    int64_t lc = ((int64_t)c0 << 8) + (int64_t)(c1 - c0) * hf;
    int64_t ld = l - lc;
    /* epi_colour_tables_derive, which panelgen runs for a const panel too, keeps
     * every cusp at least 0.5 L* inside the neutral interval, so k ≥ 2^16 and nothing
     * below overflows, whatever a and b (the bound is derived in
     * src/epi_panel_tables.h, EPI_CUSP_MARGIN_Q8). A corrupted table may misbehave
     * here. */
    int64_t k = 2 * (ld > 0 ? lmax - lc : lc - lmin);
    int64_t ald = ld < 0 ? -ld : ld;
    uint64_t num = (uint64_t)c * (uint64_t)span, den = (uint64_t)(20 * k);
    int64_t e1 = k / 2 + ald + (int64_t)((num + den / 2) / den);
    /* e1 ≥ k/2 + |Ld|, so e1² − 2k·|Ld| ≥ (k/2 − |Ld|)² ≥ 0 (k is even). */
    int64_t half = epi_round_shift(e1 - (int64_t)epi_isqrt64_round((uint64_t)(e1 * e1 - 2 * k * ald)), 1);
    int64_t l0 = ld > 0 ? lc + half : (ld < 0 ? lc - half : lc);
    l0 = l0 < lmin ? lmin : (l0 > lmax ? lmax : l0);

    /* Bisect s on [0, 1] in Q16, keeping the inside end. P(mid) is
     * evaluated (lab_to_xyz + in-gamut test) only when it differs from the
     * CIELAB point of both ends: an end with the same P has the same xyz and
     * so the same inside result. hi starts at s = 1, P(1) = (lq, a, b),
     * outside (above); lo's P is known once an inside point was found. */
    int32_t lo = 0, hi_s = EPI_Q16_ONE;
    int have_lo = 0;
    for (int it = 0; it < 16; it++) {
        int32_t mid = (lo + hi_s) >> 1;
        int32_t p[3];
        cusp_lab(mid, (int32_t)l0, (int32_t)l, a, b, p);
        int inside;
        if (lab_eq(p, p_hi)) {
            inside = 0;
        } else if (have_lo && lab_eq(p, p_lo)) {
            inside = 1;
        } else {
            int32_t q[3];
            lab_to_xyz(t, p, q);
            inside = hull_inside(t, q, hint);
            if (inside) {
                copy3(xyz, q);
                copy3(p_lo, p);
                have_lo = 1;
            } else {
                copy3(p_hi, p);
            }
        }
        if (inside) {
            lo = mid;
        } else {
            hi_s = mid;
        }
    }
    if (!have_lo) {
        cusp_lab(0, (int32_t)l0, (int32_t)l, a, b, p_lo);
        lab_to_xyz(t, p_lo, xyz);
    }
}

/* ---- init (double; + − × ÷ and epi_cbrt_d only) ------------------------- */

#define D_DELTA (6.0 / 29.0)

double epi_lab_f_d(double t) {
    return t > D_DELTA * D_DELTA * D_DELTA ? epi_cbrt_d(t) : t / (3.0 * D_DELTA * D_DELTA) + 4.0 / 29.0;
}

double epi_lab_finv_d(double f) {
    return f > D_DELTA ? f * f * f : 3.0 * D_DELTA * D_DELTA * (f - 4.0 / 29.0);
}

typedef struct {
    double wy, by, lmin, knee, tone, lift;
} tone_params_t;

/* The midtone curve's Hermite stays monotone (Fritsch–Carlson,
 * (.15/δ)² + (1/δ)² ≤ 9 with δ = (K − L*min)/K) iff δ ≥ sqrt(1.0225/9) =
 * 0.33706...; 0.3371 keeps a margin. The base knee is K = 60, which
 * holds for L*min ≤ 39.77 (every colour panel so far: EL133UF1 is 35.14).
 * For lighter blacks (low-contrast grey glass) the knee moves up just
 * enough, K = L*min / (1 − 0.3371); the curve is unchanged where K = 60.
 * There is no valid knee once K reaches 100 (L*min ≥ 66.29, contrast below
 * about 2.8:1). */
#define MID_DELTA_MIN 0.3371

static double midtone_knee(double lmin) {
    double k = lmin / (1.0 - MID_DELTA_MIN);
    return k > 60.0 ? k : 60.0;
}

/* T0(L) = L*(Y(L)·(W_y − B_y) + B_y; white W_y). */
static double bpc_lightness(const tone_params_t* p, double l) {
    double y = epi_lab_finv_d((l + 16.0) / 116.0);
    return 116.0 * epi_lab_f_d((y * (p->wy - p->by) + p->by) / p->wy) - 16.0;
}

/* Midtone curve, with τ clipped to [0, 1]; knee K as
 * midtone_knee (60 unless the black is light). */
static double midtone_curve(const tone_params_t* p, double l) {
    const double k = p->knee;
    if (l >= k) {
        return l;
    }
    double t = l / k;
    t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
    double t2 = t * t, t3 = t * t * t;
    return (2 * t3 - 3 * t2 + 1) * p->lmin + (t3 - 2 * t2 + t) * k * 0.15 + (-2 * t3 + 3 * t2) * k +
        (t3 - t2) * k * 1.0;
}

static double smoothstep_d(double x, double lo, double hi) {
    double t = (x - lo) / (hi - lo);
    t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
    return t * t * (3.0 - 2.0 * t);
}

/* T(P(L)), P the lift (design.md S3). */
static double tone_curve(const tone_params_t* p, double l) {
    l += p->lift * smoothstep_d(l, 0.0, 35.0) * (1.0 - smoothstep_d(l, 45.0, 82.0));
    return (1.0 - p->tone) * bpc_lightness(p, l) + p->tone * midtone_curve(p, l);
}

/* Round half away from zero (deterministic: one correctly rounded add). */
static int32_t round_d(double x) {
    return x >= 0.0 ? (int32_t)(x + 0.5) : -(int32_t)(-x + 0.5);
}

static int in_range(float v, float lo, float hi) {
    return v >= lo && v <= hi; /* false for NaN */
}

static void tone_tables(const tone_params_t* p, double detail, int32_t* tone_t, int32_t* tone_g) {
    const double eps = 1e-3;
    for (int i = 0; i < EPI_TONE_TABLE_LEN; i++) {
        double l = i * 0.25;
        double slope = (tone_curve(p, l + eps) - tone_curve(p, l - eps)) / (2.0 * eps);
        double g = slope + detail * (1.0 - slope);
        tone_t[i] = round_d(tone_curve(p, l) * 256.0);
        tone_g[i] = round_d(g * 65536.0);
    }
}

int32_t epi_grey_lstar_q8(int32_t y_q16) {
    return round_d((116.0 * epi_lab_f_d(y_q16 / 65536.0) - 16.0) * 256.0);
}

int epi_midtone_monotone(int32_t lstar_min_q8) {
    double lmin = lstar_min_q8 / 256.0;
    double k = midtone_knee(lmin);
    double delta = (k - lmin) / k;
    double fa = 0.15 / delta, fb = 1.0 / delta;
    return lmin >= 0.0 && k < 100.0 && fa * fa + fb * fb <= 9.0;
}

epi_err_t epi_intent_init(epi_intent_ctx_t* ctx, const epi_panel_tables_t* tables, const epi_intent_t* intent) {
    if (!in_range(intent->tone, 0.0f, 1.0f) || !in_range(intent->detail, 0.0f, 1.0f) ||
        !in_range(intent->adapt, 0.0f, 1.0f) || !in_range(intent->vibrance, 0.0f, 3.0f) ||
        !in_range(intent->veil, 0.0f, 3.0f) || !in_range(intent->lift, 0.0f, 20.0f)) {
        return EPI_ERR_INVALID_ARG;
    }
    const int grey = tables->kind == EPI_PANEL_GREY;
    const epi_grey_tables_t* gt = grey ? epi_grey_tables(tables) : NULL;
    const epi_colour_tables_t* ct = grey ? NULL : epi_colour_tables(tables);
    /* Grey: T0 relative to the lightest level (W_y = 1); L*min = L*(B_y). */
    const int32_t lmin = grey ? epi_grey_lstar_q8(gt->level_y[0]) : ct->lstar_min;
    tone_params_t p;
    p.wy = grey ? 1.0 : ct->white_xyz[1] / 65536.0;
    p.by = (grey ? gt->level_y[0] : ct->black_xyz[1]) / 65536.0;
    p.lmin = lmin / 256.0;
    p.knee = midtone_knee(p.lmin);
    p.tone = intent->tone;
    p.lift = intent->lift;

    memset(ctx, 0, sizeof *ctx);
    ctx->tables = tables;
    tone_tables(&p, intent->detail, ctx->tone_t_q8, ctx->tone_g_q16);
    if (!grey && intent->lift > 0.0f) {
        p.lift = 0.0;
        tone_tables(&p, intent->detail, ctx->unlifted_t_q8, ctx->unlifted_g_q16);
    }
    ctx->adapt_q16 = round_d((double)intent->adapt * 65536.0);
    ctx->vibrance_q16 = round_d((double)intent->vibrance * 65536.0);
    ctx->veil_q8 = round_d((double)intent->veil * 256.0);
    ctx->lift_q8 = round_d((double)intent->lift * 256.0);
    if (grey) {
        ctx->grey_lstar_min_q8 = lmin;
    } else {
        for (int i = 0; i < 3; i++) {
            /* round(2^40 / w), integer: (2^40 + w/2) / w. */
            int64_t w = ct->white_xyz[i];
            ctx->white_recip[i] = (int32_t)(((INT64_C(1) << 40) + w / 2) / w);
        }
    }
    return EPI_OK;
}

/* ---- rows --------------------------------------------------------------- */

void epi_intent_lstar_row(const epi_rgb_t* src, int16_t* lstar_q8, uint16_t width) {
    for (uint16_t x = 0; x < width; x++) {
        int32_t lin[3];
        srgb_lin(&src[x], lin);
        lstar_q8[x] = (int16_t)lstar_src(lin);
    }
}

/* Mirrored taps of the symmetric kernel share a multiply: the taps sum to 2^15,
 * so k[j] ≤ 2^14 for j < r, and the sum of two samples is within ±2^16, so
 * the product and the running total stay within ±2^30. */
void epi_blur_h(const epi_panel_tables_t* tables, const int16_t* in_q8, int16_t* out_q8, uint16_t width) {
    const int r = tables->kernel_radius;
    const uint16_t* k = tables->kernel;
    const int w = width;
    for (int x = 0; x < w; x++) {
        int32_t acc = 0;
        if (x >= r && x + r < w) {
            const int16_t* p = in_q8 + (x - r);
            acc = (int32_t)k[r] * p[r];
            for (int j = 0; j < r; j++) {
                acc += (int32_t)k[j] * (p[j] + p[2 * r - j]);
            }
        } else {
            for (int j = 0; j <= 2 * r; j++) {
                int xi = clamp_i32(x + j - r, 0, w - 1);
                acc += (int32_t)k[j] * in_q8[xi];
            }
        }
        out_q8[x] = (int16_t)epi_round_shift(acc, 15);
    }
}

void epi_blur_v(const epi_panel_tables_t* tables, const int16_t* const* rows, int16_t* out_q8, uint16_t width) {
    const int r = tables->kernel_radius;
    const uint16_t* k = tables->kernel;
    for (uint16_t x = 0; x < width; x++) {
        int32_t acc = (int32_t)k[r] * rows[r][x];
        for (int j = 0; j < r; j++) {
            acc += (int32_t)k[j] * (rows[j][x] + rows[2 * r - j][x]);
        }
        out_q8[x] = (int16_t)epi_round_shift(acc, 15);
    }
}

static inline void intent_pixel(
    const epi_intent_ctx_t* ctx,
    const epi_rgb_t* src,
    int32_t base_q8,
    int32_t target[3],
    unsigned* hint,
    epi_intent_stages_t* st
) {
    const epi_colour_tables_t* t = epi_colour_tables(ctx->tables);
    int32_t lin[3];
    int32_t lab[3], xyz[3];

    /* S1 */
    srgb_lin(src, lin);
    int32_t lsrc = lstar_src(lin);
    int32_t d = adapt_degree(ctx, lsrc);
    panel_lab(ctx, lin, d, lab);
    int32_t share = EPI_Q16_ONE;
    if (ctx->lift_q8 > 0) {
        share = low_chroma_share(lab);
        if (lift_colour(ctx, lsrc, share, lin)) {
            panel_lab(ctx, lin, d, lab);
        }
    }
    if (st != NULL) {
        st->lsrc = lsrc;
        st->d = d;
        copy3(st->lab_panel, lab);
    }

    /* S3 */
    lab[0] = tone_detail(ctx->tone_t_q8, ctx->tone_g_q16, base_q8, lsrc);
    if (share < EPI_Q16_ONE) {
        int32_t l0 = tone_detail(ctx->unlifted_t_q8, ctx->unlifted_g_q16, base_q8, lsrc);
        lab[0] = l0 + epi_mul_q16(share, lab[0] - l0);
    }
    if (st != NULL) {
        copy3(st->lab_tone, lab);
    }

    /* S4 */
    if (ctx->vibrance_q16 != EPI_Q16_ONE) {
        vibrance(ctx, lab);
    }
    lab[0] = veil_lstar(ctx, lab[0]);
    if (st != NULL) {
        copy3(st->lab_colour, lab);
    }

    /* Neutral cast */
    if (t->has_neutral) {
        int32_t w = low_chroma_share(lab);
        if (w != 0) {
            neutral_cast(t, w, lab);
        }
    }
    if (st != NULL) {
        copy3(st->lab_neutral, lab);
    }

    /* S5 */
    lab_to_xyz(t, lab, xyz);
    int moved = !hull_inside(t, xyz, hint);
    if (moved) {
        cusp_move(t, lab, xyz, hint);
    }
    if (st != NULL) {
        st->moved = (uint8_t)moved;
        copy3(st->xyz, xyz);
    }

    /* S6: target linear sRGB, unclamped. */
    epi_mat3_q16(t->xyz_to_lin, xyz, target);
}

void epi_intent_pixel_stages(
    const epi_intent_ctx_t* ctx,
    const epi_rgb_t* src,
    int32_t base_q8,
    int32_t target[3],
    epi_intent_stages_t* st
) {
    unsigned hint = 0;
    intent_pixel(ctx, src, base_q8, target, &hint, st);
}

void epi_intent_row(
    const epi_intent_ctx_t* ctx,
    const epi_rgb_t* src,
    const int16_t* base_q8,
    int32_t (*target_lin_q16)[3],
    uint16_t width
) {
    unsigned hint = 0;
    for (uint16_t x = 0; x < width; x++) {
        intent_pixel(ctx, &src[x], base_q8[x], target_lin_q16[x], &hint, NULL);
    }
}

/* Grey S3–S6 (design.md "Grey panels"). Y relative to the lightest level from L* (Q8,
 * clamped to [L*min, 100]): fy = (L + 16)/116 exactly in Q24, f⁻¹ in Q24,
 * rounded once to Q16. L* = 100 gives exactly 65536. */
void epi_intent_grey_row(
    const epi_intent_ctx_t* ctx,
    const int16_t* lsrc_q8,
    const int16_t* base_q8,
    int32_t* target_y_q16,
    uint16_t width
) {
    const int32_t lmin = ctx->grey_lstar_min_q8;
    for (uint16_t x = 0; x < width; x++) {
        int32_t l = veil_lstar(ctx, tone_detail(ctx->tone_t_q8, ctx->tone_g_q16, base_q8[x], lsrc_q8[x]));
        l = clamp_i32(l, lmin, 100 * 256);
        target_y_q16[x] = (int32_t)epi_round_shift(lab_finv_q24(lab_fy_q24(l)), 8);
    }
}
