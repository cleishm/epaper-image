/*
 * Internal fixed-point maths primitives (docs/design.md, "Fixed-point arithmetic and determinism").
 *
 * Every function here except the init-time ones in double (epi_cbrt_d,
 * epi_sqrt_d, epi_exp_d, epi_sin_d and epi_gaussian_kernel_q15) is integer-only, so
 * its output is bit-identical on every target (host, ESP32, WASM). Right
 * shifts are only ever applied to unsigned values; signed results are
 * formed by negating an unsigned magnitude, so nothing depends on
 * implementation-defined behaviour. The ones in double use only
 * IEEE-754 correctly-rounded double + − × ÷ and exact bit manipulation, and
 * so are deterministic as long as double arithmetic is evaluated in double
 * (FLT_EVAL_METHOD 0: x86-64 SSE, ARM, Xtensa soft-float, WASM; not 32-bit
 * x87) and contraction is off (-ffp-contract=off, /fp:precise). epi_fp.h
 * turns a build that breaks either condition into a compile error.
 *
 * Qn = n fractional bits. Conversions round half away from zero.
 */
#ifndef EPI_MATH_H
#define EPI_MATH_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Q16 one, and hue range in Q8 degrees. */
#define EPI_Q16_ONE 65536
#define EPI_DEG360_Q8 (360 * 256)

/*
 * x / 2^s rounded to nearest, ties away from zero. Defined for every int64
 * x (including INT64_MIN and INT64_MAX) and s in [1, 63]; s = 0 or s > 63
 * is undefined. Works on the unsigned magnitude, so the intermediate sum
 * (≤ 2^63 + 2^62) cannot overflow and no signed value is right-shifted.
 */
static inline int64_t epi_round_shift(int64_t x, unsigned s) {
    uint64_t half = UINT64_C(1) << (s - 1);
    if (x >= 0) {
        return (int64_t)(((uint64_t)x + half) >> s);
    }
    uint64_t m = UINT64_C(0) - (uint64_t)x; /* |x|, exact for INT64_MIN */
    uint64_t q = (m + half) >> s;           /* ≤ 2^62 + 1, fits int64 */
    return -(int64_t)q;
}

/* a · b / 2^16, rounded as above: a Q16 factor times a Qn value is Qn. The
 * result must fit int32. */
static inline int32_t epi_mul_q16(int32_t a, int32_t b) {
    return (int32_t)epi_round_shift((int64_t)a * b, 16);
}

/* |v| as uint32, exact for INT32_MIN. */
static inline uint32_t epi_abs32(int32_t v) {
    return v < 0 ? 0u - (uint32_t)v : (uint32_t)v;
}

/*
 * Signed cube root, Q16 in → Q16 out, for every int32 input.
 * cbrt(x / 2^16) · 2^16 = cbrt(|x| · 2^32), so the result magnitude is at
 * most cbrt(2^63) = 2^21. |error| ≤ max(1 LSB, 2^-16·|exact|); odd and
 * monotone non-decreasing by construction; cbrt(0) = 0.
 */
int32_t epi_cbrt_q16(int32_t x);

/* floor(sqrt(x)), exact for every uint64 x. */
uint32_t epi_isqrt64(uint64_t x);

/* round(sqrt(x)) for every uint64 x (up to 2^32, so uint64): the floor root s,
 * then +1 when x − s² > s, since (s + 1/2)² = s² + s + 1/4 and x is an integer
 * (ties cannot occur). */
static inline uint64_t epi_isqrt64_round(uint64_t x) {
    uint64_t s = epi_isqrt64(x);
    return x - s * s > s ? s + 1 : s;
}

/*
 * sqrt(a² + b²) in the Q of the inputs, rounded to nearest (ties cannot
 * occur). Any int32 a, b; a result that would exceed INT32_MAX (only when
 * both magnitudes are large, e.g. a, b near ±2^31) saturates to INT32_MAX.
 */
int32_t epi_hypot_q(int32_t a, int32_t b);

/*
 * atan2(y, x) in degrees, Q8, in [0, 360·256). x and y may be in any common
 * Q (the ratio is all that matters); any int32 values. atan2(0, 0) = 0.
 * |error| ≤ 0.01° vs exact (contract: 0.05°). Exact symmetries away from
 * (0, 0), mod 360·256: h(y, x) + h(−y, x) ≡ 0, h(y, x) + h(y, −x) ≡ 180·256
 * and h(x, y) + h(y, x) ≡ 90·256 (INT32_MIN is handled by magnitude 2^31).
 */
int32_t epi_atan2_deg_q8(int32_t y, int32_t x);

/*
 * smoothstep: t = clip((x − lo) / (hi − lo), 0, 1), result 3t² − 2t³ in
 * Q16. x, lo and hi are in any common scale (any int32; differences are
 * taken in 64 bits). Exactly 0 for x ≤ lo and 65536 for x ≥ hi;
 * |error| ≤ 1 LSB in between. Requires lo < hi; if lo ≥ hi the result is a
 * step: 0 for x ≤ lo, otherwise 65536.
 */
int32_t epi_smoothstep_q16(int32_t x, int32_t lo, int32_t hi);

/*
 * Init-time cube root in double, without libm: + − × ÷ and exact
 * exponent/bit manipulation only. Domain: +0 (returned unchanged) and
 * 2^-16 ≤ x < 2, which holds every value reaching it: level_y / 65536
 * (0 or at least 2^-16, at most 1), epi_lab_f_d's t (above (6/29)³, up to about
 * 1.00003 at the tone table's top slope sample) and a colour ink's cone
 * responses (src/epi_colour_panel.c rejects inks outside it). The result is
 * unspecified elsewhere. Newton iterations followed by one correction step whose
 * residual is computed exactly (Dekker two-product), so the result is the
 * correctly rounded cube root except possibly in near-halfway cases.
 */
double epi_cbrt_d(double x);

/*
 * Init-time square root in double, without libm, under the same rules as
 * epi_cbrt_d. Domain: +0 (returned unchanged) and positive normal x; the result
 * is unspecified elsewhere. Newton iterations followed by one correction step
 * whose residual is computed exactly (Dekker two-product), so the result is
 * within 1 ulp and correctly rounded except possibly in near-halfway cases.
 */
double epi_sqrt_d(double x);

/*
 * Init-time exp in double, without libm, under the same rules as
 * epi_cbrt_d (+ − × ÷ and exact power-of-two scaling only). Domain: finite
 * x ≤ 0, −0 included (epi_gaussian_kernel_q15's −i²/2σ²); undefined elsewhere. ≤ 1 ulp
 * from the exact value wherever the result is normal; x < −746 gives +0
 * (results in the subnormal range are rounded twice, still within 1
 * subnormal ulp).
 */
double epi_exp_d(double x);

/*
 * Init-time sin in double, without libm, under the same rules as
 * epi_cbrt_d. sin(x) for |x| ≤ 2^19·π/2 (≈ 8.2e5), |error| < 1 ulp:
 * fdlibm's medium argument reduction by π/2 (three-part Cody–Waite) and its
 * kernel polynomials. Odd: sin(−x) = −sin(x) exactly; sin(±0) = ±0. Outside
 * the domain (or for NaN / ±inf) the result is NaN.
 */
double epi_sin_d(double x);

/*
 * Init-time Gaussian detail kernel for sigma in (0, 10.5]: exp(−i²/2σ²),
 * i = −r..r, r = ⌈3σ⌉ ≤ 32, normalised and rounded to Q15 (half away from zero),
 * with the centre tap absorbing the residual so the sum is exactly 2^15. Writes
 * the 2r + 1 taps and returns r.
 */
int epi_gaussian_kernel_q15(float sigma, uint16_t* taps);

/* p rounded up to a multiple of a. */
static inline void* epi_align_ptr(void* p, size_t a) {
    return (uint8_t*)p + (a - (uintptr_t)p % a) % a;
}

/* What max_align_t covers; MSVC's C mode does not define it. */
typedef union {
    long long ll;
    long double ld;
    void* p;
} ws_align_t;

#define EPI_WS_ALIGN ((size_t)_Alignof(ws_align_t))

/* n rounded up to a multiple of EPI_WS_ALIGN. */
static inline size_t epi_align_up(size_t n) {
    return (n + EPI_WS_ALIGN - 1) & ~(EPI_WS_ALIGN - 1);
}

/* Exact bit access to a double (the bit pattern as uint64, and back). */
static inline uint64_t epi_dbits(double d) {
    uint64_t u;
    memcpy(&u, &d, sizeof u);
    return u;
}

static inline double epi_dfrom(uint64_t u) {
    double d;
    memcpy(&d, &u, sizeof d);
    return d;
}

/* x saturated to the int32 range. */
static inline int32_t epi_sat_i32(int64_t x) {
    return (int32_t)(x > INT32_MAX ? INT32_MAX : (x < INT32_MIN ? INT32_MIN : x));
}

/* a · b of two int32 triples, exact in int64 (Q is the sum of the inputs'). */
static inline int64_t epi_dot3(const int32_t a[3], const int32_t b[3]) {
    return (int64_t)a[0] * b[0] + (int64_t)a[1] * b[1] + (int64_t)a[2] * b[2];
}

/* m (Q16) · x (Qn) → Qn, rounded once per row and saturated to int32. */
static inline void epi_mat3_q16(const int32_t m[3][3], const int32_t x[3], int32_t out[3]) {
    for (int r = 0; r < 3; r++) {
        out[r] = epi_sat_i32(epi_round_shift(epi_dot3(m[r], x), 16));
    }
}

/* Table (exposed for the tests). Generated offline with exact arithmetic;
 * see the comments in epi_math.c. */
#define EPI_CBRT_TABLE_BITS 7 /* intervals per octave = 2^7 */
#define EPI_CBRT_TABLE_LEN (3 * (1 << EPI_CBRT_TABLE_BITS) + 1)
extern const uint32_t epi_cbrt_table[EPI_CBRT_TABLE_LEN];

#endif /* EPI_MATH_H */
