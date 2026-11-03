/*
 * Fixed-point maths primitives (docs/design.md, "Fixed-point arithmetic and determinism"). See epi_math.h for the
 * contracts; this file documents the methods.
 *
 * Integer code only shifts unsigned values right and forms signed results
 * by negating unsigned magnitudes, so nothing here is implementation-defined.
 * The only floating point is in epi_cbrt_d, epi_exp_d and epi_sin_d (init-time).
 */
#include "epi_math.h"

#include "epi_fp.h"

#include <limits.h>
#include <string.h>

/* Count leading zeros of a non-zero 32-bit value. */
static inline unsigned epi_clz32(uint32_t v) {
#if (defined(__GNUC__) || defined(__clang__)) && UINT_MAX == 0xFFFFFFFFu
    return (unsigned)__builtin_clz(v);
#else
    unsigned n = 0;
    if (v < 0x10000u) {
        n += 16;
        v <<= 16;
    }
    if (v < 0x1000000u) {
        n += 8;
        v <<= 8;
    }
    if (v < 0x10000000u) {
        n += 4;
        v <<= 4;
    }
    if (v < 0x40000000u) {
        n += 2;
        v <<= 2;
    }
    if (v < 0x80000000u) {
        n += 1;
    }
    return n;
#endif
}

/*
 * cbrt (Q16): cbrt(x / 2^16) · 2^16 = cbrt(n · 2^32) with n = |x|.
 * Write n = 2^e · (1 + f), f in [0, 1). Then n · 2^32 = 2^(3q + r) · (1 + f)
 * with e + 32 = 3q + r, r in {0, 1, 2}, so the result is
 * 2^q · cbrt(2^r · (1 + f)). The mantissa u = 2^r · (1 + f) lies in [1, 8)
 * and is looked up in one contiguous table, epi_cbrt_table, that has 128
 * uniform intervals per octave of u (384 intervals, 385 knots):
 *   epi_cbrt_table[128·r + k] = round(cbrt(2^r · (1 + k/128)) · 2^30).
 * The first and last knots are exact (2^30 and 2^31 = cbrt(1), cbrt(8)).
 * Linear interpolation uses the 25 fraction bits below the index; its
 * relative error is ≤ (1/128)² · (2/9) / 8 ≈ 1.7e-6 (≈ 2^-19.2). The result
 * is rounded once, from the Q30 interpolant, by a shift of 30 − q (9..20).
 *
 * Monotone by construction: the table is strictly increasing, the
 * interpolant is non-decreasing in f and never exceeds the next knot, and
 * at an octave wrap (r = 2 → 0, q → q + 1) the last knot 2^31 at shift s
 * rounds to the same value as the first knot 2^30 at shift s − 1.
 * Odd symmetry is exact because the magnitude is computed and then negated.
 */
int32_t epi_cbrt_q16(int32_t x) {
    if (x == 0) {
        return 0;
    }
    uint32_t n = epi_abs32(x);
    unsigned e = 31u - epi_clz32(n);       /* n in [2^e, 2^(e+1)) */
    uint32_t frac = (n << (31u - e)) << 1; /* f as a 0.32 fraction */
    unsigned sum = e + 32u;
    unsigned r = sum % 3u;
    unsigned q = sum / 3u; /* 10..21 */
    unsigned idx = (r << EPI_CBRT_TABLE_BITS) + (frac >> (32 - EPI_CBRT_TABLE_BITS));
    uint32_t w = frac & ((UINT32_C(1) << (32 - EPI_CBRT_TABLE_BITS)) - 1u);
    uint32_t lo = epi_cbrt_table[idx];
    uint32_t d = epi_cbrt_table[idx + 1] - lo; /* < 2^22 */
    uint32_t v = lo + (uint32_t)(((uint64_t)d * w) >> (32 - EPI_CBRT_TABLE_BITS));
    unsigned s = 30u - q;                               /* 9..20 */
    uint32_t mag = (v + (UINT32_C(1) << (s - 1))) >> s; /* v ≤ 2^31: no overflow */
    return x < 0 ? -(int32_t)mag : (int32_t)mag;
}

/*
 * isqrt: the classic digit-by-digit (bit-pair) method, exact, with a
 * 32-bit fast path. The starting bit is the highest even power of two
 * ≤ x, from clz.
 */
static uint32_t epi_isqrt32(uint32_t x) {
    if (x == 0) {
        return 0;
    }
    uint32_t rem = x;
    uint32_t root = 0;
    uint32_t bit = UINT32_C(1) << ((31u - epi_clz32(x)) & ~1u);
    while (bit != 0) {
        if (rem >= root + bit) {
            rem -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return root;
}

uint32_t epi_isqrt64(uint64_t x) {
    if (x <= UINT32_MAX) {
        return epi_isqrt32((uint32_t)x);
    }
    if (x < (UINT64_C(1) << 60)) {
        /* The same digit-by-digit recurrence, in 32 bits: with m bit pairs
         * below the top, root = isqrt32(x >> 2m) and rem = (x >> 2m) − root²
         * are the exact state after the top bits, then each lower pair
         * gives rem = 4·rem + pair, root = 2·root + (rem ≥ 4·root + 1).
         * x < 2^60 keeps root < 2^30 and rem ≤ 2·root < 2^31 throughout,
         * so 4·rem + 3 < 2^32; the low 2m ≤ 28 bits are in x's low word. */
        unsigned top = 63u - epi_clz32((uint32_t)(x >> 32)); /* 32..59 */
        unsigned m = (top - 30u) / 2u;                       /* x >> 2m < 2^32 */
        uint32_t hi = (uint32_t)(x >> (2u * m));
        uint32_t lo = (uint32_t)x;
        uint32_t root = epi_isqrt32(hi);
        uint32_t rem = hi - root * root;
        for (unsigned j = m; j-- > 0;) {
            rem = (rem << 2) | ((lo >> (2u * j)) & 3u);
            uint32_t trial = (root << 2) | 1u;
            root <<= 1;
            if (rem >= trial) {
                rem -= trial;
                root |= 1u;
            }
        }
        return root;
    }
    unsigned top = 63u - epi_clz32((uint32_t)(x >> 32)); /* x ≥ 2^32 */
    uint64_t rem = x;
    uint64_t root = 0;
    uint64_t bit = UINT64_C(1) << (top & ~1u);
    while (bit != 0) {
        if (rem >= root + bit) {
            rem -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)root;
}

/* hypot: a² + b² ≤ 2^63 in uint64, then its rounded root. */
int32_t epi_hypot_q(int32_t a, int32_t b) {
    uint64_t ua = epi_abs32(a);
    uint64_t ub = epi_abs32(b);
    uint64_t r = epi_isqrt64_round(ua * ua + ub * ub);
    return r > (uint64_t)INT32_MAX ? INT32_MAX : (int32_t)r;
}

/* round(atan(i/128) · 180/π · 2^16), Q16 degrees; 516 bytes. */
#define EPI_ATAN_TABLE_LEN 129
static const uint32_t epi_atan_table[EPI_ATAN_TABLE_LEN] = {
    0,       29335,   58666,   87990,   117304,  146603,  175884,  205144,  234379,  263585,  292760,  321899,  350999,
    380058,  409070,  438034,  466945,  495801,  524598,  553333,  582003,  610605,  639135,  667591,  695970,  724268,
    752484,  780613,  808654,  836604,  864460,  892219,  919879,  947438,  974893,  1002241, 1029481, 1056611, 1083627,
    1110529, 1137313, 1163979, 1190524, 1216947, 1243245, 1269417, 1295461, 1321376, 1347161, 1372813, 1398332, 1423717,
    1448965, 1474076, 1499049, 1523882, 1548575, 1573127, 1597536, 1621803, 1645926, 1669904, 1693738, 1717426, 1740967,
    1764362, 1787610, 1810710, 1833663, 1856467, 1879123, 1901631, 1923990, 1946200, 1968261, 1990173, 2011937, 2033552,
    2055018, 2076336, 2097505, 2118526, 2139399, 2160125, 2180703, 2201134, 2221419, 2241558, 2261551, 2281398, 2301101,
    2320659, 2340074, 2359345, 2378474, 2397460, 2416306, 2435010, 2453574, 2471999, 2490285, 2508433, 2526443, 2544317,
    2562055, 2579658, 2597126, 2614461, 2631664, 2648734, 2665673, 2682482, 2699161, 2715711, 2732134, 2748430, 2764600,
    2780644, 2796564, 2812361, 2828035, 2843587, 2859019, 2874330, 2889523, 2904597, 2919554, 2934395, 2949120,
};

/*
 * atan2: octant reduction. With mn = min(|x|, |y|) and mx = max, the base
 * angle is atan(mn / mx) in [0°, 45°]. Both are left-normalised by the same
 * shift so mx has bit 31 set, and the ratio t = mn / (mx >> 16) is a 32-bit
 * division giving Q16 (high by at most a relative 2^-15, low by at most
 * 2^-16; clamped to 1). atan(t) in degrees is linearly interpolated from
 * epi_atan_table (129 knots, t step 1/128, Q16 degrees):
 *   epi_atan_table[i] = round(atan(i/128) · 180/π · 2^16),
 * interpolation error ≤ 0.65 · (1/128)² / 8 rad ≈ 0.0003°. The base angle is
 * rounded to Q8 once and then folded exactly: 90° − a if |y| > |x|,
 * 180° − a if x < 0, 360° − a if y < 0, with 360° wrapped to 0.
 */
int32_t epi_atan2_deg_q8(int32_t y, int32_t x) {
    uint32_t ax = epi_abs32(x);
    uint32_t ay = epi_abs32(y);
    if ((ax | ay) == 0) {
        return 0;
    }
    int swap = ay > ax;
    uint32_t mn = swap ? ax : ay;
    uint32_t mx = swap ? ay : ax;
    unsigned sh = epi_clz32(mx);
    mx <<= sh;
    mn <<= sh;
    uint32_t t = mn / (mx >> 16); /* divisor in [2^15, 2^16) */
    uint32_t a16;
    if (t >= 65536u) {
        a16 = epi_atan_table[EPI_ATAN_TABLE_LEN - 1];
    } else {
        uint32_t i = t >> 9;
        uint32_t w = t & 511u;
        uint32_t lo = epi_atan_table[i];
        a16 = lo + (((epi_atan_table[i + 1] - lo) * w + 256u) >> 9);
    }
    int32_t a = (int32_t)((a16 + 128u) >> 8); /* Q8, [0, 45·256] */
    if (swap) {
        a = 90 * 256 - a;
    }
    if (x < 0) {
        a = 180 * 256 - a;
    }
    if (y < 0) {
        a = EPI_DEG360_Q8 - a;
    }
    if (a >= EPI_DEG360_Q8) {
        a -= EPI_DEG360_Q8;
    }
    return a;
}

/*
 * smoothstep: t = (x − lo)/(hi − lo) in Q30 (rounded 64-bit division;
 * numerator < 2^62), t² and t³ in Q30 with rounding, s = 3t² − 2t³ in Q30
 * (≥ t² ≥ 0 since t ≤ 1), then one rounding to Q16. Intermediate error is
 * ≈ 2^-28, so the result is within 0.5 + 2^-12 LSB.
 */
int32_t epi_smoothstep_q16(int32_t x, int32_t lo, int32_t hi) {
    if (x <= lo) {
        return 0;
    }
    if (x >= hi) {
        return EPI_Q16_ONE;
    }
    uint64_t num = (uint64_t)((int64_t)x - (int64_t)lo);  /* 0 < num < den */
    uint64_t den = (uint64_t)((int64_t)hi - (int64_t)lo); /* < 2^32 */
    uint64_t t = ((num << 30) + (den >> 1)) / den;        /* ≤ 2^30 */
    uint64_t half = UINT64_C(1) << 29;
    uint64_t t2 = (t * t + half) >> 30;
    uint64_t t3 = (t2 * t + half) >> 30;
    uint64_t s = 3u * t2 - 2u * t3;
    return (int32_t)((s + (UINT64_C(1) << 13)) >> 14);
}

/*
 * cbrt in double, no libm, on the domain in epi_math.h. A non-zero input is
 * decomposed exactly through its bit pattern into m · 2^(3k), m in [1, 8)
 * (its exponent is in [−16, 0]). Six Newton steps y ← y − (y³ − m)/(3y²)
 * from the linear guess 0.8125 + 0.15625·m (≤ 11% off; the error squares
 * each step) reach ~1 ulp; one more step uses the exact residual m − y³
 * (Dekker/Veltkamp two-product, so pure + − ×), which makes the result the
 * correctly rounded cbrt(m) except in near-halfway cases. Scaling by 2^k
 * (k in [−6, 0]) is an exact multiply by a normal power of two. All
 * constants are dyadic, so their literals are exact.
 */
static void epi_two_prod(double a, double b, double* p, double* err) {
    const double split = 134217729.0; /* 2^27 + 1 */
    double prod = a * b;
    double ca = split * a;
    double ah = ca - (ca - a);
    double al = a - ah;
    double cb = split * b;
    double bh = cb - (cb - b);
    double bl = b - bh;
    *p = prod;
    *err = ((ah * bh - prod) + ah * bl + al * bh) + al * bl;
}

static double epi_pow2_d(int k) { /* 2^k, k in [−1022, 1023] */
    return epi_dfrom((uint64_t)(1023 + k) << 52);
}

double epi_cbrt_d(double x) {
    if (x == 0.0) {
        return x;
    }
    uint64_t u = epi_dbits(x);
    const uint64_t mant = (UINT64_C(1) << 52) - 1u;
    int e = (int)(u >> 52) - 1023;
    int r = e % 3;
    if (r < 0) {
        r += 3;
    }
    int k = (e - r) / 3;
    double m = epi_dfrom((u & mant) | ((uint64_t)(1023 + r) << 52)); /* [1, 8) */

    double y = 0.8125 + 0.15625 * m;
    for (int i = 0; i < 6; i++) {
        y = y - (y * y * y - m) / (3.0 * y * y);
    }
    double p, pe, c, ce;
    epi_two_prod(y, y, &p, &pe);
    epi_two_prod(y, p, &c, &ce);
    double res = ((m - c) - ce) - y * pe; /* m − y³; m − c is exact (Sterbenz) */
    y = y + res / (3.0 * y * y);

    return y * epi_pow2_d(k);
}

/*
 * exp in double, no libm (init-time: the grey panels' Gaussian kernel), on
 * the domain in epi_math.h.
 * Cody–Waite reduction x = k·ln2 + r with k = nearest integer to x/ln2 and
 * ln2 split as fdlibm's LN2_HI (42 significant bits, so k·LN2_HI is exact
 * for |k| ≤ 2^11) + LN2_LO; |r| ≤ ln2/2 + tiny. exp(r) is the degree-13
 * Taylor polynomial (truncation ≤ r^14/14! ≈ 4e-18, far below 2^-53),
 * evaluated by Horner as 1 + (r + r²·P(r)) so the leading terms carry no
 * rounding beyond the final add. The result is scaled by 2^k with exact
 * power-of-two multiplies: one when 2^k is normal; two when k < −1022 (the
 * first exact, the second rounding once into the subnormal range). Constants
 * are hex literals, exact by construction (the 1/n! are the correctly rounded
 * doubles). The kernel's outer tap −1/(2σ²) passes −746 for σ below about
 * 0.026, down to −2^297 at the smallest float σ, so the early return also
 * keeps the conversion to int in range.
 */
double epi_exp_d(double x) {
    if (x < -746.0) {
        return 0.0; /* below half the smallest subnormal */
    }
    const double inv_ln2 = 0x1.71547652b82fep+0;
    const double ln2_hi = 0x1.62e42fee00000p-1;
    const double ln2_lo = 0x1.a39ef35793c76p-33;
    double kd = x * inv_ln2;
    int k = (int)(kd - 0.5); /* round half away (kd ≤ 0); k in [−1076, 0] */
    double r = (x - (double)k * ln2_hi) - (double)k * ln2_lo;
    double p = 0x1.6124613a86d09p-33;  /* 1/13! */
    p = p * r + 0x1.1eed8eff8d898p-29; /* 1/12! */
    p = p * r + 0x1.ae64567f544e4p-26; /* 1/11! */
    p = p * r + 0x1.27e4fb7789f5cp-22; /* 1/10! */
    p = p * r + 0x1.71de3a556c734p-19; /* 1/9! */
    p = p * r + 0x1.a01a01a01a01ap-16; /* 1/8! */
    p = p * r + 0x1.a01a01a01a01ap-13; /* 1/7! */
    p = p * r + 0x1.6c16c16c16c17p-10; /* 1/6! */
    p = p * r + 0x1.1111111111111p-7;  /* 1/5! */
    p = p * r + 0x1.5555555555555p-5;  /* 1/4! */
    p = p * r + 0x1.5555555555555p-3;  /* 1/3! */
    p = p * r + 0.5;                   /* 1/2! */
    double y = 1.0 + (r + (r * r) * p);
    if (k < -1022) {
        return (y * epi_pow2_d(k + 54)) * epi_pow2_d(-54);
    }
    return y * epi_pow2_d(k);
}

/* ---- sin (fdlibm k_sin.c, k_cos.c, e_rem_pio2.c medium path) -------------- */

static const double epi_S1 = -0x1.5555555555549p-3;
static const double epi_S2 = 0x1.111111110f8a6p-7;
static const double epi_S3 = -0x1.a01a019c161d5p-13;
static const double epi_S4 = 0x1.71de357b1fe7dp-19;
static const double epi_S5 = -0x1.ae5e68a2b9cebp-26;
static const double epi_S6 = 0x1.5d93a5acfd57cp-33;
static const double epi_C1 = 0x1.555555555554cp-5;
static const double epi_C2 = -0x1.6c16c16c15177p-10;
static const double epi_C3 = 0x1.a01a019cb1590p-16;
static const double epi_C4 = -0x1.27e4f809c52adp-22;
static const double epi_C5 = 0x1.1ee9ebdb4b1c4p-29;
static const double epi_C6 = -0x1.8fae9be8838d4p-37;
static const double epi_invpio2 = 0x1.45f306dc9c883p-1;
static const double epi_pio2_1 = 0x1.921fb54400000p+0;   /* first 33 bits of π/2 */
static const double epi_pio2_1t = 0x1.0b4611a626331p-34; /* π/2 − pio2_1 */
static const double epi_pio2_2 = 0x1.0b4611a600000p-34;  /* next 33 bits */
static const double epi_pio2_2t = 0x1.3198a2e037073p-69;
static const double epi_pio2_3 = 0x1.3198a2e000000p-69; /* next 33 bits */
static const double epi_pio2_3t = 0x1.b839a252049c1p-104;

static inline uint32_t epi_hi_word(double d) {
    return (uint32_t)(epi_dbits(d) >> 32);
}

/* sin(x + y) for |x| ≤ π/4, |y| ≤ ulp(x)/2; iy = 0 means y is 0. */
static double epi_k_sin(double x, double y, int iy) {
    if ((epi_hi_word(x) & 0x7FFFFFFFu) < 0x3E400000u) { /* |x| < 2^-27 */
        return x;
    }
    double z = x * x;
    double v = z * x;
    double r = epi_S2 + z * (epi_S3 + z * (epi_S4 + z * (epi_S5 + z * epi_S6)));
    if (iy == 0) {
        return x + v * (epi_S1 + z * r);
    }
    return x - ((z * (0.5 * y - v * r) - y) - v * epi_S1);
}

/* cos(x + y) for |x| ≤ π/4, |y| ≤ ulp(x)/2. */
static double epi_k_cos(double x, double y) {
    double z = x * x;
    double w = z * z;
    double r = z * (epi_C1 + z * (epi_C2 + z * epi_C3)) + w * w * (epi_C4 + z * (epi_C5 + z * epi_C6));
    double hz = 0.5 * z;
    w = 1.0 - hz;
    return w + (((1.0 - w) - hz) + (z * r - x * y));
}

/* x − n·π/2 = y0 + y1 for π/4 < |x| ≤ 2^19·π/2; returns n. All products
 * fn·pio2_k are exact (fn < 2^20, constants of 33 bits). */
static int32_t epi_rem_pio2(double x, double* y0, double* y1) {
    uint32_t hx = epi_hi_word(x);
    uint32_t ix = hx & 0x7FFFFFFFu;
    double t = x < 0.0 ? -x : x;
    int32_t n = (int32_t)(t * epi_invpio2 + 0.5);
    double fn = (double)n;
    double r = t - fn * epi_pio2_1;
    double w = fn * epi_pio2_1t; /* first round good to 85 bits */
    int32_t j = (int32_t)(ix >> 20);
    double a = r - w;
    int32_t i = j - (int32_t)((epi_hi_word(a) >> 20) & 0x7FFu);
    if (i > 16) { /* second round, good to 118 bits */
        double tt = r;
        w = fn * epi_pio2_2;
        r = tt - w;
        w = fn * epi_pio2_2t - ((tt - r) - w);
        a = r - w;
        i = j - (int32_t)((epi_hi_word(a) >> 20) & 0x7FFu);
        if (i > 49) { /* third round, 151 bits */
            tt = r;
            w = fn * epi_pio2_3;
            r = tt - w;
            w = fn * epi_pio2_3t - ((tt - r) - w);
            a = r - w;
        }
    }
    double b = (r - a) - w;
    if (x < 0.0) {
        *y0 = -a;
        *y1 = -b;
        return -n;
    }
    *y0 = a;
    *y1 = b;
    return n;
}

double epi_sin_d(double x) {
    uint32_t ix = epi_hi_word(x) & 0x7FFFFFFFu;
    if (ix <= 0x3FE921FBu) { /* |x| ≲ π/4 */
        return epi_k_sin(x, 0.0, 0);
    }
    if (ix > 0x413921FBu) { /* outside the domain, NaN or inf */
        return epi_dfrom(UINT64_C(0x7FF8000000000000));
    }
    double y0, y1;
    uint32_t n = (uint32_t)epi_rem_pio2(x, &y0, &y1);
    switch (n & 3u) {
    case 0:
        return epi_k_sin(y0, y1, 1);
    case 1:
        return epi_k_cos(y0, y1);
    case 2:
        return -epi_k_sin(y0, y1, 1);
    default:
        return -epi_k_cos(y0, y1);
    }
}

/* ---- Tables (const, so in flash on ESP32). Generated offline with exact
 * integer / 80-digit decimal arithmetic; every entry rounds to nearest (no
 * entry is within 1e-40 of a tie). The tests re-check each entry against
 * libm. ---- */

/* round(cbrt(2^r · (1 + k/128)) · 2^30) at index 128·r + k; 1540 bytes. */
const uint32_t epi_cbrt_table[385] = {
    1073741824, 1076530776, 1079305352, 1082065736, 1084812108, 1087544643, 1090263516, 1092968896, 1095660949,
    1098339837, 1101005721, 1103658757, 1106299099, 1108926897, 1111542301, 1114145454, 1116736499, 1119315576,
    1121882823, 1124438374, 1126982360, 1129514914, 1132036161, 1134546227, 1137045235, 1139533307, 1142010561,
    1144477115, 1146933082, 1149378575, 1151813707, 1154238585, 1156653317, 1159058008, 1161452763, 1163837683,
    1166212869, 1168578419, 1170934430, 1173280998, 1175618217, 1177946180, 1180264978, 1182574700, 1184875435,
    1187167269, 1189450289, 1191724578, 1193990219, 1196247295, 1198495886, 1200736070, 1202967927, 1205191532,
    1207406963, 1209614293, 1211813597, 1214004946, 1216188413, 1218364068, 1220531980, 1222692218, 1224844850,
    1226989941, 1229127559, 1231257767, 1233380629, 1235496209, 1237604568, 1239705769, 1241799870, 1243886933,
    1245967015, 1248040175, 1250106471, 1252165958, 1254218693, 1256264730, 1258304125, 1260336930, 1262363198,
    1264382983, 1266396335, 1268403305, 1270403945, 1272398303, 1274386428, 1276368370, 1278344176, 1280313893,
    1282277567, 1284235246, 1286186974, 1288132797, 1290072759, 1292006904, 1293935275, 1295857916, 1297774868,
    1299686174, 1301591875, 1303492012, 1305386625, 1307275755, 1309159440, 1311037720, 1312910634, 1314778219,
    1316640514, 1318497555, 1320349380, 1322196025, 1324037526, 1325873918, 1327705238, 1329531520, 1331352798,
    1333169107, 1334980481, 1336786952, 1338588554, 1340385319, 1342177280, 1343964469, 1345746917, 1347524656,
    1349297717, 1351066130, 1352829926, 1356343786, 1359839533, 1363317398, 1366777610, 1370220389, 1373645954,
    1377054519, 1380446293, 1383821480, 1387180284, 1390522900, 1393849522, 1397160341, 1400455543, 1403735310,
    1406999822, 1410249256, 1413483784, 1416703576, 1419908799, 1423099616, 1426276188, 1429438673, 1432587227,
    1435722001, 1438843146, 1441950808, 1445045132, 1448126261, 1451194335, 1454249490, 1457291861, 1460321583,
    1463338785, 1466343596, 1469336142, 1472316548, 1475284936, 1478241427, 1481186139, 1484119188, 1487040690,
    1489950758, 1492849502, 1495737032, 1498613457, 1501478881, 1504333411, 1507177148, 1510010195, 1512832650,
    1515644613, 1518446181, 1521237448, 1524018510, 1526789459, 1529550386, 1532301382, 1535042535, 1537773934,
    1540495663, 1543207809, 1545910455, 1548603684, 1551287578, 1553962217, 1556627681, 1559284047, 1561931394,
    1564569796, 1567199330, 1569820070, 1572432088, 1575035457, 1577630248, 1580216532, 1582794378, 1585363854,
    1587925028, 1590477966, 1593022735, 1595559400, 1598088024, 1600608672, 1603121406, 1605626287, 1608123377,
    1610612736, 1613094424, 1615568499, 1618035019, 1620494043, 1622945626, 1625389825, 1627826695, 1630256291,
    1632678666, 1635093875, 1637501969, 1639903002, 1642297025, 1644684088, 1647064241, 1649437536, 1651804021,
    1654163744, 1656516754, 1658863098, 1661202823, 1663535977, 1665862603, 1668182749, 1670496459, 1672803778,
    1675104749, 1677399416, 1679687821, 1681970009, 1684246020, 1686515896, 1688779678, 1691037408, 1693289125,
    1695534869, 1697774680, 1700008596, 1702236657, 1704458901, 1708886087, 1713290452, 1717672288, 1722031881,
    1726369511, 1730685453, 1734979975, 1739253342, 1743505812, 1747737639, 1751949071, 1756140353, 1760311724,
    1764463418, 1768595665, 1772708693, 1776802723, 1780877973, 1784934657, 1788972984, 1792993162, 1796995392,
    1800979874, 1804946803, 1808896371, 1812828767, 1816744176, 1820642780, 1824524759, 1828390290, 1832239544,
    1836072692, 1839889902, 1843691338, 1847477163, 1851247535, 1855002611, 1858742546, 1862467491, 1866177595,
    1869873006, 1873553868, 1877220323, 1880872512, 1884510572, 1888134640, 1891744849, 1895341330, 1898924215,
    1902493630, 1906049701, 1909592552, 1913122306, 1916639083, 1920143001, 1923634178, 1927112729, 1930578766,
    1934032403, 1937473749, 1940902913, 1944320003, 1947725124, 1951118380, 1954499874, 1957869708, 1961227982,
    1964574794, 1967910241, 1971234420, 1974547426, 1977849351, 1981140287, 1984420327, 1987689559, 1990948072,
    1994195954, 1997433291, 2000660168, 2003876669, 2007082877, 2010278874, 2013464741, 2016640559, 2019806405,
    2022962357, 2026108493, 2029244889, 2032371620, 2035488759, 2038596380, 2041694556, 2044783357, 2047862855,
    2050933119, 2053994217, 2057046219, 2060089191, 2063123201, 2066148312, 2069164591, 2072172102, 2075170908,
    2078161072, 2081142656, 2084115721, 2087080328, 2090036536, 2092984405, 2095923994, 2098855360, 2101778561,
    2104693653, 2107600692, 2110499734, 2113390833, 2116274043, 2119149419, 2122017013, 2124876878, 2127729065,
    2130573626, 2133410612, 2136240072, 2139062057, 2141876616, 2144683797, 2147483648,
};
