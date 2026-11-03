/*
 * Stage S7: see epi_dither.h and docs/design.md, "Error diffusion".
 *
 * Integer only. Linear RGB, the working value, the error and OKLab
 * are Q16 in int32; products are formed in int64 and narrowed with
 * epi_round_shift (round half away from zero).
 *
 * Error memory: one row of (width + 2·EPI_DITHER_ERR_PAD) Q16 triples plus a
 * carry. A cell ahead of the scan holds the current row, one behind it the
 * row below; the taps of the pixel at x, direction d, are
 *
 *   E[x + d] += p0;  E[x − d] += p1;  E[x] = carry + p2;  carry = p3;
 *
 * Taps that fall outside the image land in the padding, which is never read
 * as a pixel and is zeroed at the end of the row, or stay in the last carry,
 * so out-of-image error is dropped without branches.
 */
#include "epi_dither.h"

#include "epi_math.h"

#include <string.h>

#if defined(__GNUC__) || defined(__clang__)
#define EPI_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#define EPI_NOINLINE __declspec(noinline)
#else
#define EPI_NOINLINE
#endif

/* Q24 scale of the damping coefficients: 0.05·x for x in Q16 is x·64/5 in
 * Q24, so the .05 factor is applied exactly up to one rounding. */
#define DAMP_Q 24

/* Smallest a² + b² whose rounded root (epi_hypot_q) is ≥ 2622:
 * ceil(2621.5²) = ceil(6872262.25). */
#define WC_SAT_N UINT64_C(6872263)

size_t epi_dither_err_size(const epi_panel_tables_t* tables, uint16_t width) {
    const size_t channels = tables->kind == EPI_PANEL_GREY ? 1u : 3u;
    return ((size_t)width + 2u * EPI_DITHER_ERR_PAD) * channels * sizeof(int32_t);
}

/* Ink-relative OKLab (Q16) of linear RGB (Q16): M2·cbrt(M1·(x ⊘ w)). */
static inline void oklab_q16(const epi_colour_tables_t* tb, const int32_t lin[3], int32_t lab[3]) {
    int32_t lms[3];
    epi_mat3_q16(tb->lin_to_lms, lin, lms);
    lms[0] = epi_cbrt_q16(lms[0]);
    lms[1] = epi_cbrt_q16(lms[1]);
    lms[2] = epi_cbrt_q16(lms[2]);
    epi_mat3_q16(tb->lms_to_oklab, lms, lab);
}

/* |a − b| for |a|, |b| < 2^30 (so the difference fits int32). */
static inline uint32_t absdiff30(int32_t a, int32_t b) {
    return a >= b ? (uint32_t)(a - b) : (uint32_t)(b - a);
}

/* Nearest ink by squared OKLab distance; ties go to the lowest index.
 *
 * Precondition: every |ink_oklab| < 2^30 (true of generated tables; tested).
 * lab is always far below that: epi_cbrt_q16 peaks at 2^21 and each OK_M2
 * row sums to at most 318320/65536 in magnitude, so |lab| <= 10,186,240.
 * Each difference is then below 2^31, each square below 2^62 and the
 * distance below 3·2^62 < 2^64, formed exactly in uint64. A partial sum
 * that already reaches best_d means the full distance does too, so the ink
 * cannot be taken (the test is strict) and the rest of its sum is skipped. */
static inline unsigned nearest_ink(const epi_dither_ctx_t* ctx, const int32_t lab[3]) {
    const epi_colour_tables_t* tb = epi_colour_tables(ctx->tables);
    unsigned best = 0;
    uint64_t best_d = UINT64_MAX;
    for (unsigned i = 0; i < tb->base.ink_count; i++) {
        const int32_t* ink = tb->ink_oklab[i];
        uint32_t u = absdiff30(lab[0], ink[0]);
        uint64_t d = (uint64_t)u * u;
        if (d >= best_d) {
            continue;
        }
        u = absdiff30(lab[1], ink[1]);
        d += (uint64_t)u * u;
        if (d >= best_d) {
            continue;
        }
        u = absdiff30(lab[2], ink[2]);
        d += (uint64_t)u * u;
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return best;
}

/* .05·x in Q24 of x in Q16 (x ≥ 0), rounded. */
static inline int32_t q24_05(uint32_t x) {
    return (int32_t)((x * 64u + 2u) / 5u);
}

/* cutl = .05·(1 − ss(L_ok, .90, .97)), Q24, from L_ok in Q16: ss on L·100
 * against 90 and 97 (exact thresholds). */
static inline int32_t cutl_q24(int32_t l_ok) {
    int64_t l100 = (int64_t)l_ok * 100;
    int32_t ss = epi_smoothstep_q16(epi_sat_i32(l100), 90 * EPI_Q16_ONE, 97 * EPI_Q16_ONE);
    return q24_05((uint32_t)(EPI_Q16_ONE - ss));
}

/*
 * Damping coefficients from the target (Q24):
 *   kl = cutl = .05·(1 − ss(L_ok, .90, .97)),  kc = .05·wc,  wc = min(1, C_ok/.04).
 * Exported for the host tests (declared in epi_dither.h).
 */
void epi_dither_damp_coeffs(const epi_colour_tables_t* tb, const int32_t t[3], int32_t* kl, int32_t* kc) {
    int32_t lab[3];
    oklab_q16(tb, t, lab);
    /* wc = C/.04 = 25·C, exact in Q16, saturating at 1 once C ≥ 2622
     * ((65536 + 24)/25). epi_hypot_q rounds sqrt(n), n = a² + b², to
     * nearest, so C ≥ 2622 iff n > 2621.5², i.e. n ≥ WC_SAT_N: the root is
     * only needed below that. */
    uint64_t ua = epi_abs32(lab[1]);
    uint64_t ub = epi_abs32(lab[2]);
    int32_t wc;
    if (ua * ua + ub * ub >= WC_SAT_N) {
        wc = EPI_Q16_ONE;
    } else {
        wc = epi_hypot_q(lab[1], lab[2]) * 25; /* C ≤ 2621 */
    }
    *kl = cutl_q24(lab[0]);
    *kc = q24_05((uint32_t)wc);
}

/*
 * e′ = e − kl·lum − kc·(e − lum), lum = (e·n̂)·n̂; one rounding per channel.
 * Exported for the host tests (declared in epi_dither.h).
 */
void epi_dither_damp(const epi_colour_tables_t* tb, const int32_t e[3], int32_t kl, int32_t kc, int32_t out[3]) {
    const int32_t* n = tb->nhat;
    int64_t dot = epi_dot3(e, n); /* Q32 */
    for (int c = 0; c < 3; c++) {
        int64_t lum = epi_round_shift(dot * n[c], 32); /* Q16 */
        /* kl·lum + kc·(e − lum), regrouped exactly (integers, no rounding). */
        int64_t cut = (int64_t)kc * e[c] + (int64_t)(kl - kc) * lum;
        out[c] = epi_sat_i32((int64_t)e[c] - epi_round_shift(cut, DAMP_Q));
    }
}

/*
 * Floyd–Steinberg split of one error component: 7/16 ahead, 3/16
 * behind-below, 5/16 below (each rounded), and the remainder ahead-below,
 * so the four parts sum to e exactly. Exported for the host tests (declared
 * in epi_dither.h). Out of line on purpose: inlined into epi_dither_row's
 * loop it costs more than the call (register pressure; measured on Xtensa).
 */
EPI_NOINLINE void epi_dither_fs_split(int32_t e, int32_t parts[4]) {
    const uint32_t m = epi_abs32(e);
    if (m < (UINT32_C(1) << 28)) {
        /* epi_round_shift(k·e, 4) is ±((k·|e| + 8) >> 4) with the sign of
         * e; for |e| < 2^28 every term fits 32 bits (7·|e| + 8 < 2^31), so
         * these are the same values as the 64-bit path below. */
        const int32_t q7 = (int32_t)((7u * m + 8u) >> 4);
        const int32_t q3 = (int32_t)((3u * m + 8u) >> 4);
        const int32_t q5 = (int32_t)((5u * m + 8u) >> 4);
        const int32_t p7 = e < 0 ? -q7 : q7, p3 = e < 0 ? -q3 : q3, p5 = e < 0 ? -q5 : q5;
        parts[0] = p7;
        parts[1] = p3;
        parts[2] = p5;
        parts[3] = e - p7 - p3 - p5; /* every partial sum is below 2^29 */
        return;
    }
    int32_t p7 = (int32_t)epi_round_shift((int64_t)e * 7, 4);
    int32_t p3 = (int32_t)epi_round_shift((int64_t)e * 3, 4);
    int32_t p5 = (int32_t)epi_round_shift((int64_t)e * 5, 4);
    parts[0] = p7;
    parts[1] = p3;
    parts[2] = p5;
    parts[3] = (int32_t)((int64_t)e - p7 - p3 - p5);
}

void epi_dither_row(epi_dither_ctx_t* ctx, const int32_t (*target_lin_q16)[3], uint8_t* idx_out) {
    const epi_colour_tables_t* tb = epi_colour_tables(ctx->tables);
    const int w = ctx->width;
    int32_t (*err)[3] = ctx->err;
    err += EPI_DITHER_ERR_PAD;
    int32_t carry[3] = {0, 0, 0};
    const int rtl = (int)(ctx->row & 1u);
    const int d = rtl ? -1 : 1;

    for (int k = 0; k < w; k++) {
        const int x = rtl ? w - 1 - k : k;
        const int32_t* t = target_lin_q16[x];

        int32_t kl, kc;
        epi_dither_damp_coeffs(tb, t, &kl, &kc);

        int32_t v[3], lab[3];
        for (int c = 0; c < 3; c++) {
            v[c] = t[c] + err[x][c];
        }
        oklab_q16(tb, v, lab);
        const unsigned i = nearest_ink(ctx, lab);
        idx_out[x] = (uint8_t)i;

        int32_t e[3], ed[3];
        for (int c = 0; c < 3; c++) {
            e[c] = v[c] - tb->ink_lin[i][c];
        }
        epi_dither_damp(tb, e, kl, kc, ed);

        for (int c = 0; c < 3; c++) {
            int32_t p[4];
            epi_dither_fs_split(ed[c], p);
            err[x + d][c] += p[0];
            err[x - d][c] += p[1];
            err[x][c] = carry[c] + p[2];
            carry[c] = p[3];
        }
    }

    memset(err[-1], 0, sizeof err[-1]);
    memset(err[w], 0, sizeof err[w]);
    ctx->row++;
}

/* ---- grey (design.md "Grey panels") ---------------------------------------
 *
 * One channel: target, working value v and error are Y relative to the
 * lightest level, Q16. The error memory is one row of
 * (width + 2·EPI_DITHER_ERR_PAD) int32 and a carry, used as above.
 */

/* Band of t (Q16) outside which ss(cbrt(t), .90, .97) is exactly 0 or 1:
 * cbrt(.72) = .8963 < .90 and cbrt(.92) = .9726 > .97, with margins of 170+
 * LSB against epi_cbrt_q16's ≤ 1 LSB error (the exact bounds are .729 and
 * .9127). Outside it the cbrt is skipped; the result is unchanged. */
#define GREY_SS_LO 47185 /* floor(.72 · 2^16) */
#define GREY_SS_HI 60294 /* ceil(.92 · 2^16) */

/*
 * Grey damping coefficient (Q24): cutl = .05·(1 − ss(L_ok, .90, .97)) with
 * L_ok = cbrt(t), the OKLab L of a neutral relative to the lightest level.
 * Exported for the host tests (declared in epi_dither.h).
 */
int32_t epi_dither_grey_cutl(int32_t t) {
    if (t <= GREY_SS_LO) {
        return q24_05(EPI_Q16_ONE);
    }
    if (t >= GREY_SS_HI) {
        return 0;
    }
    return cutl_q24(epi_cbrt_q16(t));
}

/* Nearest level in cbrt(Y): the first i with v ≤ level_thr[i], else the
 * lightest (binary search; thresholds increase). Equal to argmin over i of
 * |cbrt(v) − cbrt(y_i)| with ties to the darker level, because cbrt is
 * monotone and each threshold is the floored midpoint (epi_panel_tables.h);
 * v < 0 lands on the darkest and v > 1 on the lightest with no special case. */
static inline unsigned nearest_level(const epi_grey_tables_t* tb, int32_t v) {
    unsigned lo = 0, hi = tb->base.ink_count - 1u;
    while (lo < hi) {
        unsigned mid = (lo + hi) >> 1;
        if (v > tb->level_thr[mid]) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

/*
 * Coverage toe (design.md "Grey panels"). In the gap y_g <= t < y_{g+1} the target's
 * coverage c = (t − y_g)/gap is remapped, c′ = toe(c) below ½ and
 * 1 − toe(1 − c) above, where toe = 0 below D, a cubic Hermite from
 * (D, 0, slope 0) to (C0, C0, slope 1), and the identity from C0. Targets
 * outside [y_0, y_last) are unchanged. All Q16 integers:
 *
 *   D_Q16 = round(.03·2^16) = 1966, C0_Q16 = round(.11·2^16) = 7209 (the
 *   nearest integers; neither is a tie), W = C0_Q16 − D_Q16 = 5243.
 *
 *   c = ((t − y_g)·2^16 + ⌊gap/2⌋) / gap, uint32 division rounded to
 *   nearest (ties up). The numerator is at most 65535·2^16 + 32768 < 2^32,
 *   and c ≤ 65535. Rounding (not flooring) makes the identity band exact:
 *   c·gap lies in (k·2^16 − gap/2, k·2^16 + gap/2] for k = t − y_g, which
 *   for gap < 2^16 is inside (k·2^16 − 2^15, k·2^16 + 2^15), and gap = 2^16
 *   gives c = k; so t′ = t wherever c′ = c. With a floored c, gaps over 2^15
 *   (every 2-level panel) would give t′ = t − 1 for about half the band.
 *
 *   t′ = y_g + epi_round_shift(c′·gap, 16).
 */
#define TOE_D_Q16 1966  /* round(.03 · 2^16) = round(1966.08) */
#define TOE_C0_Q16 7209 /* round(.11 · 2^16) = round(7208.96) */
#define TOE_W_Q16 (TOE_C0_Q16 - TOE_D_Q16)

/*
 * toe(c), c in [0, 2^16] (Q16). For D <= c < C0:
 *   s = ((c − D)·2^16 + ⌊W/2⌋) / W, uint32 division rounded to nearest
 *       (ties up); (c − D)·2^16 < 5243·2^16 < 2^29, and s < 2^16;
 *   h = C0·(3s² − 2s³) + W·(s³ − s²) with s = s/2^16, which is
 *       s²·(C0·(3·2^16 − 2s) + W·(s − 2^16)) / 2^48 in integers, formed
 *       exactly in int64 and rounded once (epi_round_shift, 48). The inner
 *       factor is positive (linear in s, positive at both ends) and below
 *       C0·3·2^16 < 1.42·10^9; s² < 2^32; the product is below 6.1·10^18
 *       < 2^63.
 * h(D) = 0 and h(C0 − 1) = C0 − 1 exactly. Monotone non-decreasing over
 * [0, 2^16] (checked exhaustively in tests/test_grey.c). Exported for the
 * host tests (declared in epi_dither.h).
 */
int32_t epi_dither_grey_toe_q16(int32_t c) {
    if (c < TOE_D_Q16) {
        return 0;
    }
    if (c >= TOE_C0_Q16) {
        return c;
    }
    const int64_t s = (int64_t)((((uint32_t)(c - TOE_D_Q16) << 16) + TOE_W_Q16 / 2u) / (uint32_t)TOE_W_Q16);
    const int64_t inner = (int64_t)TOE_C0_Q16 * (3 * EPI_Q16_ONE - 2 * s) + (int64_t)TOE_W_Q16 * (s - EPI_Q16_ONE);
    return (int32_t)epi_round_shift(s * s * inner, 48);
}

/* Gap of t: the g with y_g <= t < y_{g+1}, binary search on level_y; the
 * caller ensures y_0 <= t < y_last. */
static inline unsigned toe_gap(const epi_grey_tables_t* tb, int32_t t) {
    unsigned lo = 0, hi = tb->base.ink_count - 2u;
    while (lo < hi) {
        unsigned mid = (lo + hi + 1u) >> 1;
        if (t >= tb->level_y[mid]) {
            lo = mid;
        } else {
            hi = mid - 1u;
        }
    }
    return lo;
}

/* t′ in gap g by the full formula (no identity shortcut). */
static inline int32_t toe_in_gap(const epi_grey_tables_t* tb, unsigned g, int32_t t) {
    const int32_t yg = tb->level_y[g];
    const uint32_t gap = (uint32_t)(tb->level_y[g + 1] - yg);
    const int32_t c = (int32_t)((((uint32_t)(t - yg) << 16) + gap / 2u) / gap);
    const int32_t cp =
        c < EPI_Q16_ONE / 2 ? epi_dither_grey_toe_q16(c) : EPI_Q16_ONE - epi_dither_grey_toe_q16(EPI_Q16_ONE - c);
    return yg + (int32_t)epi_round_shift((int64_t)cp * gap, 16);
}

/* The toe by the full formula, from the tables alone. Exported for the host
 * tests (declared in epi_dither.h). */
int32_t epi_dither_grey_toe_full(const epi_grey_tables_t* tb, int32_t t) {
    if (t < tb->level_y[0] || t >= tb->level_y[tb->base.ink_count - 1u]) {
        return t;
    }
    return toe_in_gap(tb, toe_gap(tb, t), t);
}

/* The toe as the ditherer applies it: the full formula outside the gap's
 * identity band, t itself inside it (equal, see epi_dither_init). */
static inline int32_t toe_fast(const epi_dither_ctx_t* ctx, int32_t t) {
    const epi_grey_tables_t* tb = epi_grey_tables(ctx->tables);
    if (t < tb->level_y[0] || t >= tb->level_y[tb->base.ink_count - 1u]) {
        return t;
    }
    const unsigned g = toe_gap(tb, t);
    if (t >= ctx->toe_id_lo[g] && t <= ctx->toe_id_hi[g]) {
        return t;
    }
    return toe_in_gap(tb, g, t);
}

/* Exported for the host tests (declared in epi_dither.h). */
int32_t epi_dither_grey_toe(const epi_dither_ctx_t* ctx, int32_t t) {
    return toe_fast(ctx, t);
}

void epi_dither_init(epi_dither_ctx_t* ctx, const epi_panel_tables_t* tables, uint16_t width, void* err_mem) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->tables = tables;
    ctx->width = width;
    ctx->err = err_mem;
    memset(err_mem, 0, epi_dither_err_size(tables, width));
    if (tables->kind != EPI_PANEL_GREY) {
        return;
    }
    /*
     * Identity band of each gap: the k = t − y_g in [0, gap) with
     * C0 <= c(k) <= 2^16 − C0, where toe_q16 (or its mirror) returns c and
     * so t′ = t (above). c(k) = ⌊(k·2^16 + h)/gap⌋, h = ⌊gap/2⌋, is
     * non-decreasing in k, so the band is an interval:
     *   c(k) >= C0      iff k·2^16 >= C0·gap − h,       k_lo = ⌈(C0·gap − h)/2^16⌉;
     *   c(k) <= M       iff k·2^16 <= (M + 1)·gap − h − 1, k_hi = ⌊that/2^16⌋,
     * M = 2^16 − C0 (k_hi < gap since M + 1 <= 2^16). Both numerators are
     * positive. An empty band has k_lo > k_hi and never matches. Checked
     * against the full formula for every t in tests/test_grey.c.
     */
    const epi_grey_tables_t* tb = epi_grey_tables(tables);
    for (unsigned g = 0; g + 1u < tb->base.ink_count; g++) {
        const int64_t gap = (int64_t)tb->level_y[g + 1] - tb->level_y[g];
        const int64_t h = gap / 2;
        const int64_t klo = ((int64_t)TOE_C0_Q16 * gap - h + EPI_Q16_ONE - 1) >> 16;
        const int64_t khi = ((int64_t)(EPI_Q16_ONE - TOE_C0_Q16 + 1) * gap - h - 1) >> 16;
        ctx->toe_id_lo[g] = tb->level_y[g] + (int32_t)klo;
        ctx->toe_id_hi[g] = tb->level_y[g] + (int32_t)khi;
    }
}

void epi_dither_grey_row(epi_dither_ctx_t* ctx, const int32_t* target_y_q16, uint8_t* idx_out) {
    const epi_grey_tables_t* tb = epi_grey_tables(ctx->tables);
    const int w = ctx->width;
    int32_t* err = ctx->err;
    err += EPI_DITHER_ERR_PAD;
    int32_t carry = 0;
    const int rtl = (int)(ctx->row & 1u);
    const int d = rtl ? -1 : 1;

    for (int k = 0; k < w; k++) {
        const int x = rtl ? w - 1 - k : k;
        const int32_t t = toe_fast(ctx, target_y_q16[x]); /* everything below uses t′ */
        const int32_t kl = epi_dither_grey_cutl(t);
        const int32_t v = t + err[x];
        const unsigned i = nearest_level(tb, v);
        idx_out[x] = (uint8_t)i;

        /* All grey error is luminance: e′ = e − cutl·e (no chroma term). */
        const int32_t e = v - tb->level_y[i];
        const int32_t ed = epi_sat_i32((int64_t)e - epi_round_shift((int64_t)kl * e, DAMP_Q));
        int32_t p[4];
        epi_dither_fs_split(ed, p);
        err[x + d] += p[0];
        err[x - d] += p[1];
        err[x] = carry + p[2];
        carry = p[3];
    }

    err[-1] = 0;
    err[w] = 0;
    ctx->row++;
}
