/*
 * Tests for the fixed-point maths primitives (src/epi_math.h).
 *
 * Oracles use libm double (tests only). Each test prints its measured
 * worst-case error so regressions in accuracy are visible in the log.
 *
 * Determinism fingerprints (test_math_fingerprints): FNV-1a 64 over the
 * little-endian bytes of each primitive's outputs on a fixed input sequence
 * generated with integer arithmetic (the exp kernel arguments: double, under
 * the rules of epi_fp.h). The expected values (FP_* below)
 * were recorded on macOS arm64 (Apple clang 21) at -O0 and -O3 (identical)
 * and must match on every target (ESP32, Emscripten/Node).
 */
#include "epi_math.h"
#include "test_util.h"
#include "unity.h"

#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---- helpers ---- */

/* uint32 → int32 by two's-complement wrap, without implementation-defined
 * conversion. */
static int32_t wrap_i32(uint32_t u) {
    return u <= (uint32_t)INT32_MAX ? (int32_t)u : -(int32_t)(~u) - 1;
}

static int64_t wrap_i64(uint64_t u) {
    return u <= (uint64_t)INT64_MAX ? (int64_t)u : -(int64_t)(~u) - 1;
}

static uint64_t xs64_state;

static double circ_diff_deg(double a, double b) {
    double d = fmod(a - b, 360.0);
    if (d > 180.0) {
        d -= 360.0;
    }
    if (d < -180.0) {
        d += 360.0;
    }
    return fabs(d);
}

static void fail_msg(const char* fmt, long long a, long long b, long long c) {
    char buf[200];
    snprintf(buf, sizeof buf, fmt, a, b, c);
    TEST_FAIL_MESSAGE(buf);
}

/* ---- epi_round_shift ---- */

static int64_t round_shift_oracle(int64_t x, unsigned s) {
    uint64_t m = x < 0 ? UINT64_C(0) - (uint64_t)x : (uint64_t)x;
    uint64_t q = m >> s;
    uint64_t rem = m - (q << s);
    if (rem >= (UINT64_C(1) << (s - 1))) {
        q++;
    }
    return x < 0 ? -(int64_t)q : (int64_t)q;
}

/* 64-bit checks use TEST_ASSERT_TRUE so they need no UNITY_SUPPORT_64 (ESP32). */
void test_math_round_shift(void) {
    /* Exact rational rounding for small values, against libm round(). */
    for (int64_t x = -5000; x <= 5000; x++) {
        for (unsigned s = 1; s <= 14; s++) {
            double exact = round((double)x / (double)(1u << s));
            int64_t got = epi_round_shift(x, s);
            if ((double)got != exact) {
                fail_msg("round_shift(%lld, %lld) = %lld", x, s, got);
            }
        }
    }
    /* Ties go away from zero. */
    TEST_ASSERT_TRUE(1 == epi_round_shift(1, 1));
    TEST_ASSERT_TRUE(-1 == epi_round_shift(-1, 1));
    TEST_ASSERT_TRUE(2 == epi_round_shift(3, 1));
    TEST_ASSERT_TRUE(-2 == epi_round_shift(-3, 1));
    TEST_ASSERT_TRUE(1 == epi_round_shift(32768, 16));
    TEST_ASSERT_TRUE(-1 == epi_round_shift(-32768, 16));
    TEST_ASSERT_TRUE(0 == epi_round_shift(32767, 16));
    TEST_ASSERT_TRUE(0 == epi_round_shift(-32767, 16));
    /* Limits. */
    TEST_ASSERT_TRUE(INT64_C(1) << 62 == epi_round_shift(INT64_MAX, 1));
    TEST_ASSERT_TRUE(-(INT64_C(1) << 62) == epi_round_shift(INT64_MIN, 1));
    TEST_ASSERT_TRUE(1 == epi_round_shift(INT64_MAX, 63));
    TEST_ASSERT_TRUE(-1 == epi_round_shift(INT64_MIN, 63));
    TEST_ASSERT_TRUE(0 == epi_round_shift(INT64_MIN + 1 + (INT64_C(1) << 62), 63));
    TEST_ASSERT_TRUE(2 == epi_round_shift(INT64_MAX, 62));
    TEST_ASSERT_TRUE(-2 == epi_round_shift(INT64_MIN, 62));
    TEST_ASSERT_TRUE(0 == epi_round_shift(0, 63));
    /* Random 64-bit values, every shift. */
    xs64_state = UINT64_C(0x9E3779B97F4A7C15);
    for (int i = 0; i < 200000; i++) {
        int64_t x = wrap_i64(xorshift64(&xs64_state)) >> (i % 61); /* arithmetic shift of a test input only */
        for (unsigned s = 1; s <= 63; s++) {
            int64_t got = epi_round_shift(x, s);
            if (got != round_shift_oracle(x, s)) {
                fail_msg("round_shift(%lld, %lld) = %lld", x, s, got);
            }
            if (x != INT64_MIN && epi_round_shift(-x, s) != -got) {
                fail_msg("round_shift odd symmetry x=%lld s=%lld got=%lld", x, s, got);
            }
        }
    }
}

/* ---- epi_cbrt_q16 ---- */

static double cbrt_max_err_lsb;   /* worst |got − exact| in LSB */
static double cbrt_max_err_ratio; /* worst |got − exact| / tolerance */
static double cbrt_max_rel;       /* worst |got − exact| / |exact| where |exact| ≥ 2^20 */

static void check_cbrt(int32_t x) {
    int32_t got = epi_cbrt_q16(x);
    double exact = cbrt((double)x / 65536.0) * 65536.0;
    double err = fabs((double)got - exact);
    double tol = fabs(exact) / 65536.0;
    if (tol < 1.0) {
        tol = 1.0;
    }
    if (err > cbrt_max_err_lsb) {
        cbrt_max_err_lsb = err;
    }
    if (err / tol > cbrt_max_err_ratio) {
        cbrt_max_err_ratio = err / tol;
    }
    if (fabs(exact) >= 1048576.0 && err / fabs(exact) > cbrt_max_rel) {
        cbrt_max_rel = err / fabs(exact);
    }
    if (err > tol) {
        fail_msg("cbrt_q16(%lld) = %lld, exact*1000 = %lld", x, got, (long long)(exact * 1000.0));
    }
    if (x != INT32_MIN && epi_cbrt_q16(-x) != -got) {
        fail_msg("cbrt_q16 odd symmetry x=%lld got=%lld neg=%lld", x, got, epi_cbrt_q16(-x));
    }
}

/* Contiguous window [a, b] (non-negative): oracle and monotonicity. */
static void check_cbrt_window(int64_t a, int64_t b) {
    if (a < 0) {
        a = 0;
    }
    if (b > INT32_MAX) {
        b = INT32_MAX;
    }
    int32_t prev = a > 0 ? epi_cbrt_q16((int32_t)(a - 1)) : epi_cbrt_q16(-1);
    for (int64_t v = a; v <= b; v++) {
        int32_t x = (int32_t)v;
        int32_t got = epi_cbrt_q16(x);
        if (got < prev) {
            fail_msg("cbrt_q16 not monotone at %lld: %lld < %lld", x, got, prev);
        }
        prev = got;
        check_cbrt(x);
    }
}

void test_math_cbrt_q16_table(void) {
    TEST_ASSERT_EQUAL_UINT32(UINT32_C(1) << 30, epi_cbrt_table[0]);
    TEST_ASSERT_EQUAL_UINT32(UINT32_C(1) << 31, epi_cbrt_table[EPI_CBRT_TABLE_LEN - 1]);
    for (int i = 0; i < EPI_CBRT_TABLE_LEN; i++) {
        int r = i >> EPI_CBRT_TABLE_BITS;
        int k = i & ((1 << EPI_CBRT_TABLE_BITS) - 1);
        double u = ldexp(1.0 + ldexp((double)k, -EPI_CBRT_TABLE_BITS), r);
        double exact = cbrt(u) * 1073741824.0;
        if (fabs((double)epi_cbrt_table[i] - exact) > 0.5 + 1e-6) {
            fail_msg("cbrt table[%lld] = %lld, exact = %lld", i, epi_cbrt_table[i], (long long)exact);
        }
        if (i > 0 && epi_cbrt_table[i] <= epi_cbrt_table[i - 1]) {
            fail_msg("cbrt table not increasing at %lld (%lld, %lld)", i, epi_cbrt_table[i], epi_cbrt_table[i - 1]);
        }
    }
}

void test_math_cbrt_q16(void) {
    cbrt_max_err_lsb = cbrt_max_err_ratio = cbrt_max_rel = 0.0;
    TEST_ASSERT_EQUAL_INT32(0, epi_cbrt_q16(0));
    TEST_ASSERT_EQUAL_INT32(65536, epi_cbrt_q16(65536));
    TEST_ASSERT_EQUAL_INT32(-65536, epi_cbrt_q16(-65536));
    TEST_ASSERT_EQUAL_INT32(131072, epi_cbrt_q16(8 * 65536));
    TEST_ASSERT_EQUAL_INT32(-131072, epi_cbrt_q16(-8 * 65536));
    TEST_ASSERT_EQUAL_INT32(196608, epi_cbrt_q16(27 * 65536));
    TEST_ASSERT_EQUAL_INT32(32768, epi_cbrt_q16(8192)); /* cbrt(1/8) = 1/2 */
    TEST_ASSERT_EQUAL_INT32(-(INT32_C(1) << 21), epi_cbrt_q16(INT32_MIN));
    TEST_ASSERT_EQUAL_INT32(1625, epi_cbrt_q16(1)); /* cbrt(2^-16)·2^16 = 1625.499 */
    TEST_ASSERT_EQUAL_INT32(-1625, epi_cbrt_q16(-1));
    check_cbrt(INT32_MAX);
    check_cbrt(INT32_MIN + 1);

    /* Exhaustive over |x| ≤ 2^22 (Q16 values up to ±64), with monotonicity. */
    check_cbrt_window(0, INT64_C(1) << 22);
    /* Every knot of every octave of the table: windows around
     * 2^e·(1 + k/128) for e ≥ 7, which covers octave boundaries (k = 0, 128). */
    for (int e = 7; e <= 30; e++) {
        for (int k = 0; k <= 128; k++) {
            int64_t knot = (INT64_C(1) << e) + ((int64_t)k << (e - 7));
            check_cbrt_window(knot - 300, knot + 300);
        }
    }
    check_cbrt_window(INT32_MAX - 100000, INT32_MAX);
    /* Dense stride sweep over the whole positive range (monotone between
     * samples), mirrored by the odd-symmetry check inside check_cbrt. */
    int32_t prev = 0;
    for (int64_t v = 0; v <= INT32_MAX; v += 211) {
        int32_t got = epi_cbrt_q16((int32_t)v);
        if (got < prev) {
            fail_msg("cbrt_q16 not monotone at %lld: %lld < %lld", v, got, prev);
        }
        prev = got;
        check_cbrt((int32_t)v);
    }
    printf(
        "cbrt_q16: max err %.4f LSB, max err/tolerance %.4f, max rel err (|exact| >= 16.0) "
        "%.3g (2^%.2f)\n",
        cbrt_max_err_lsb,
        cbrt_max_err_ratio,
        cbrt_max_rel,
        log2(cbrt_max_rel)
    );
}

/* Full monotonicity over every non-negative int32 (oracle-free; odd
 * symmetry extends it to negatives). ~2^31 calls: about 1.5 s at -O3 and
 * 10 s at -O0 on a desktop, far longer on a device, so other runners
 * (ESP-IDF, Node) can build with -DEPI_TEST_EXHAUSTIVE=0 to skip it. */
#ifndef EPI_TEST_EXHAUSTIVE
#define EPI_TEST_EXHAUSTIVE 1
#endif
void test_math_cbrt_q16_monotone_exhaustive(void) {
#if !EPI_TEST_EXHAUSTIVE
    TEST_IGNORE_MESSAGE("EPI_TEST_EXHAUSTIVE=0");
#endif
    int32_t prev = epi_cbrt_q16(0);
    uint32_t bad = 0;
    for (uint32_t v = 1; v <= (uint32_t)INT32_MAX; v++) {
        int32_t got = epi_cbrt_q16((int32_t)v);
        bad += got < prev;
        prev = got;
    }
    TEST_ASSERT_EQUAL_UINT32(0, bad);
    TEST_ASSERT_TRUE(epi_cbrt_q16(INT32_MIN) <= epi_cbrt_q16(INT32_MIN + 1));
}

/* ---- epi_isqrt64 ---- */

static void check_isqrt(uint64_t x) {
    uint64_t r = epi_isqrt64(x);
    /* r² ≤ x < (r + 1)², written without overflow. */
    if (r > UINT32_MAX || r * r > x || x - r * r > 2 * r) {
        char buf[100];
        snprintf(buf, sizeof buf, "isqrt64(%" PRIu64 ") = %" PRIu64, x, r);
        TEST_FAIL_MESSAGE(buf);
    }
}

void test_math_isqrt64(void) {
    TEST_ASSERT_EQUAL_UINT32(0, epi_isqrt64(0));
    TEST_ASSERT_EQUAL_UINT32(1, epi_isqrt64(1));
    TEST_ASSERT_EQUAL_UINT32(1, epi_isqrt64(3));
    TEST_ASSERT_EQUAL_UINT32(2, epi_isqrt64(4));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, epi_isqrt64(UINT64_MAX));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, epi_isqrt64((uint64_t)UINT32_MAX * UINT32_MAX));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX - 1, epi_isqrt64((uint64_t)UINT32_MAX * UINT32_MAX - 1));
    TEST_ASSERT_EQUAL_UINT32(65535, epi_isqrt64(UINT32_MAX));
    TEST_ASSERT_EQUAL_UINT32(65536, epi_isqrt64((uint64_t)UINT32_MAX + 1));
    TEST_ASSERT_EQUAL_UINT32(UINT32_C(1) << 31, epi_isqrt64(UINT64_C(1) << 62));
    for (uint64_t x = 0; x <= (UINT64_C(1) << 22); x++) {
        check_isqrt(x);
    }
    /* Perfect squares and their neighbours, across the range. */
    for (uint64_t k = 1; k <= UINT32_MAX; k += (k >> 6) + 1) {
        uint64_t sq = k * k;
        TEST_ASSERT_EQUAL_UINT32((uint32_t)k, epi_isqrt64(sq));
        TEST_ASSERT_EQUAL_UINT32((uint32_t)(k - 1), epi_isqrt64(sq - 1));
        TEST_ASSERT_EQUAL_UINT32((uint32_t)k, epi_isqrt64(sq + 1));
        TEST_ASSERT_EQUAL_UINT32((uint32_t)k, epi_isqrt64(sq + 2 * k)); /* (k+1)² − 1 */
    }
    for (unsigned b = 0; b < 64; b++) {
        uint64_t p = UINT64_C(1) << b;
        check_isqrt(p);
        check_isqrt(p - 1);
        check_isqrt(p + 1);
    }
    xs64_state = UINT64_C(0x0123456789ABCDEF);
    for (int i = 0; i < 1000000; i++) {
        uint64_t x = xorshift64(&xs64_state) >> (i % 64);
        check_isqrt(x);
    }
}

/* ---- epi_hypot_q ---- */

static void check_hypot(int32_t a, int32_t b) {
    int32_t got = epi_hypot_q(a, b);
    uint64_t ua = a < 0 ? UINT64_C(0) - (uint64_t)(int64_t)a : (uint64_t)a;
    uint64_t ub = b < 0 ? UINT64_C(0) - (uint64_t)(int64_t)b : (uint64_t)b;
    uint64_t n = ua * ua + ub * ub;
    uint64_t r = (uint64_t)got;
    int ok;
    if (got == INT32_MAX) {
        /* Either the nearest value or saturated: (INT32_MAX + ½)² − ¼ ≥ n is
         * the nearest case; otherwise the true value is larger. */
        ok = n > r * r - r;
    } else {
        /* Nearest: r² − r < n ≤ r² + r. */
        ok = got >= 0 && n > r * r - r && n <= r * r + r;
        if (r == 0) {
            ok = n == 0;
        }
    }
    if (!ok) {
        fail_msg("hypot(%lld, %lld) = %lld", a, b, got);
    }
    if (got != epi_hypot_q(b, a) || (a != INT32_MIN && got != epi_hypot_q(-a, b))) {
        fail_msg("hypot symmetry (%lld, %lld) = %lld", a, b, got);
    }
}

void test_math_hypot_q(void) {
    TEST_ASSERT_EQUAL_INT32(0, epi_hypot_q(0, 0));
    TEST_ASSERT_EQUAL_INT32(5, epi_hypot_q(3, 4));
    TEST_ASSERT_EQUAL_INT32(5, epi_hypot_q(-3, -4));
    TEST_ASSERT_EQUAL_INT32(1, epi_hypot_q(1, 1)); /* 1.414 */
    TEST_ASSERT_EQUAL_INT32(4, epi_hypot_q(2, 3)); /* 3.606 */
    TEST_ASSERT_EQUAL_INT32(2, epi_hypot_q(1, 2)); /* 2.236 */
    TEST_ASSERT_EQUAL_INT32(INT32_MAX, epi_hypot_q(INT32_MAX, 0));
    TEST_ASSERT_EQUAL_INT32(INT32_MAX, epi_hypot_q(INT32_MIN, 0)); /* 2^31 saturates */
    TEST_ASSERT_EQUAL_INT32(INT32_MAX, epi_hypot_q(INT32_MIN, INT32_MIN));
    TEST_ASSERT_EQUAL_INT32(INT32_MAX, epi_hypot_q(INT32_MAX, INT32_MAX));
    TEST_ASSERT_EQUAL_INT32(1518500250, epi_hypot_q(1073741824, 1073741824)); /* 2^30·√2 */
    for (int32_t a = -300; a <= 300; a++) {
        for (int32_t b = -300; b <= 300; b++) {
            check_hypot(a, b);
        }
    }
    xs64_state = UINT64_C(0xDEADBEEFCAFEF00D);
    for (int i = 0; i < 1000000; i++) {
        uint64_t u = xorshift64(&xs64_state);
        int32_t a = wrap_i32((uint32_t)u) >> (i % 32); /* arithmetic shift of a test input only */
        int32_t b = wrap_i32((uint32_t)(u >> 32)) >> ((i / 32) % 32);
        check_hypot(a, b);
    }
}

/* ---- epi_atan2_deg_q8 ---- */

static double atan_max_err;

static void check_atan2(int32_t y, int32_t x) {
    int32_t got = epi_atan2_deg_q8(y, x);
    if (got < 0 || got >= EPI_DEG360_Q8) {
        fail_msg("atan2(%lld, %lld) = %lld out of range", y, x, got);
    }
    if (x == 0 && y == 0) {
        TEST_ASSERT_EQUAL_INT32(0, got);
        return;
    }
    double exact = atan2((double)y, (double)x) * (180.0 / 3.14159265358979323846);
    double err = circ_diff_deg((double)got / 256.0, exact);
    if (err > atan_max_err) {
        atan_max_err = err;
    }
    if (err > 0.05) {
        fail_msg("atan2(%lld, %lld) = %lld", y, x, got);
    }
    /* Exact symmetries (mod 360°). */
    if (y != INT32_MIN) {
        int32_t g = epi_atan2_deg_q8(-y, x);
        if ((got + g) % EPI_DEG360_Q8 != 0) {
            fail_msg("atan2 y-mirror (%lld, %lld) -> %lld", y, x, g);
        }
    }
    if (x != INT32_MIN) {
        int32_t g = epi_atan2_deg_q8(y, -x);
        if ((got + g) % EPI_DEG360_Q8 != 180 * 256) {
            fail_msg("atan2 x-mirror (%lld, %lld) -> %lld", y, x, g);
        }
    }
    {
        int32_t g = epi_atan2_deg_q8(x, y);
        if ((got + g) % EPI_DEG360_Q8 != 90 * 256) {
            fail_msg("atan2 swap (%lld, %lld) -> %lld", y, x, g);
        }
    }
}

void test_math_atan2_deg_q8(void) {
    atan_max_err = 0.0;
    TEST_ASSERT_EQUAL_INT32(0, epi_atan2_deg_q8(0, 0));
    TEST_ASSERT_EQUAL_INT32(0, epi_atan2_deg_q8(0, 1));
    TEST_ASSERT_EQUAL_INT32(90 * 256, epi_atan2_deg_q8(1, 0));
    TEST_ASSERT_EQUAL_INT32(180 * 256, epi_atan2_deg_q8(0, -1));
    TEST_ASSERT_EQUAL_INT32(270 * 256, epi_atan2_deg_q8(-1, 0));
    TEST_ASSERT_EQUAL_INT32(45 * 256, epi_atan2_deg_q8(7, 7));
    TEST_ASSERT_EQUAL_INT32(135 * 256, epi_atan2_deg_q8(7, -7));
    TEST_ASSERT_EQUAL_INT32(225 * 256, epi_atan2_deg_q8(-7, -7));
    TEST_ASSERT_EQUAL_INT32(315 * 256, epi_atan2_deg_q8(-7, 7));
    TEST_ASSERT_EQUAL_INT32(45 * 256, epi_atan2_deg_q8(INT32_MIN, INT32_MIN) - 180 * 256);
    TEST_ASSERT_EQUAL_INT32(0, epi_atan2_deg_q8(-1, INT32_MAX)); /* −2.7e-8° wraps to 0 */
    TEST_ASSERT_EQUAL_INT32(180 * 256, epi_atan2_deg_q8(0, INT32_MIN));
    TEST_ASSERT_EQUAL_INT32(270 * 256, epi_atan2_deg_q8(INT32_MIN, 0));
    /* Exhaustive small grid. */
    for (int32_t y = -200; y <= 200; y++) {
        for (int32_t x = -200; x <= 200; x++) {
            check_atan2(y, x);
        }
    }
    /* Dense angle sweep (0.01°) at several radii. */
    const double radii[] = {50.0, 1000.0, 30000.0, 1e6, 1.5e9};
    for (unsigned ri = 0; ri < sizeof radii / sizeof radii[0]; ri++) {
        for (int i = 0; i < 36000; i++) {
            double th = (double)i * (3.14159265358979323846 / 18000.0);
            check_atan2((int32_t)lround(radii[ri] * sin(th)), (int32_t)lround(radii[ri] * cos(th)));
        }
    }
    /* Random pairs of every magnitude, including the int32 limits. */
    xs64_state = UINT64_C(0xA5A5A5A55A5A5A5A);
    for (int i = 0; i < 1000000; i++) {
        uint64_t u = xorshift64(&xs64_state);
        int32_t y = wrap_i32((uint32_t)u) >> (i % 32);
        int32_t x = wrap_i32((uint32_t)(u >> 32)) >> ((i / 32) % 32);
        check_atan2(y, x);
    }
    const int32_t lim[] = {INT32_MIN, INT32_MIN + 1, -1, 0, 1, INT32_MAX - 1, INT32_MAX};
    for (unsigned i = 0; i < 7; i++) {
        for (unsigned j = 0; j < 7; j++) {
            check_atan2(lim[i], lim[j]);
        }
    }
    printf("atan2_deg_q8: max err %.5f deg\n", atan_max_err);
}

/* ---- epi_smoothstep_q16 ---- */

static double ss_max_err;

static void check_ss(int32_t x, int32_t lo, int32_t hi) {
    int32_t got = epi_smoothstep_q16(x, lo, hi);
    double t = ((double)x - (double)lo) / ((double)hi - (double)lo);
    if (t < 0.0) {
        t = 0.0;
    }
    if (t > 1.0) {
        t = 1.0;
    }
    double exact = t * t * (3.0 - 2.0 * t) * 65536.0;
    double err = fabs((double)got - exact);
    if (err > ss_max_err) {
        ss_max_err = err;
    }
    if (err > 1.0) {
        fail_msg("smoothstep(%lld, %lld, %lld)", x, lo, hi);
    }
    if (x <= lo && got != 0) {
        fail_msg("smoothstep(%lld, %lld, %lld) not 0", x, lo, hi);
    }
    if (x >= hi && got != 65536) {
        fail_msg("smoothstep(%lld, %lld, %lld) not 1", x, lo, hi);
    }
}

static void check_ss_range(int32_t lo, int32_t hi, int64_t step) {
    int32_t prev = 0;
    for (int64_t v = (int64_t)lo - 3; v <= (int64_t)hi + 3; v += step) {
        if (v < INT32_MIN || v > INT32_MAX) {
            continue;
        }
        int32_t got = epi_smoothstep_q16((int32_t)v, lo, hi);
        if (got < prev) {
            fail_msg("smoothstep not monotone at %lld (%lld, %lld)", v, lo, hi);
        }
        prev = got;
        check_ss((int32_t)v, lo, hi);
    }
}

void test_math_smoothstep_q16(void) {
    ss_max_err = 0.0;
    TEST_ASSERT_EQUAL_INT32(0, epi_smoothstep_q16(0, 0, 100));
    TEST_ASSERT_EQUAL_INT32(65536, epi_smoothstep_q16(100, 0, 100));
    TEST_ASSERT_EQUAL_INT32(32768, epi_smoothstep_q16(50, 0, 100));
    TEST_ASSERT_EQUAL_INT32(0, epi_smoothstep_q16(INT32_MIN, 0, 1));
    TEST_ASSERT_EQUAL_INT32(65536, epi_smoothstep_q16(INT32_MAX, 0, 1));
    TEST_ASSERT_EQUAL_INT32(0, epi_smoothstep_q16(INT32_MIN, INT32_MIN, INT32_MAX));
    TEST_ASSERT_EQUAL_INT32(65536, epi_smoothstep_q16(INT32_MAX, INT32_MIN, INT32_MAX));
    TEST_ASSERT_EQUAL_INT32(32768, epi_smoothstep_q16(0, -1000, 1000));
    /* Step behaviour for lo ≥ hi (documented). */
    TEST_ASSERT_EQUAL_INT32(0, epi_smoothstep_q16(5, 5, 5));
    TEST_ASSERT_EQUAL_INT32(65536, epi_smoothstep_q16(6, 5, 5));
    /* Exhaustive with hi − lo = 2^16 and other spans, monotone. */
    check_ss_range(0, 65536, 1);
    check_ss_range(-65536, 0, 1);
    check_ss_range(4 * 256, 14 * 256, 1);  /* vibrance, C* Q8 */
    check_ss_range(75 * 256, 95 * 256, 1); /* highlight adaptation, L* Q8 */
    check_ss_range(58982, 63570, 1);       /* .90 .. .97 in Q16 */
    check_ss_range(0, 1, 1);
    check_ss_range(-7, 13, 1);
    check_ss_range(INT32_MIN, INT32_MAX, 4093);
    check_ss_range(INT32_MIN, INT32_MIN + 1000000, 7);
    check_ss_range(INT32_MAX - 3000000, INT32_MAX, 11);
    xs64_state = UINT64_C(0x5555AAAA3333CCCC);
    for (int i = 0; i < 1000000; i++) {
        uint64_t u = xorshift64(&xs64_state);
        int32_t a = wrap_i32((uint32_t)u) >> (i % 32);
        int32_t b = wrap_i32((uint32_t)(u >> 32)) >> ((i / 32) % 32);
        if (a == b) {
            continue;
        }
        int32_t lo = a < b ? a : b;
        int32_t hi = a < b ? b : a;
        int64_t span = (int64_t)hi - lo;
        int64_t x = lo + (int64_t)(xorshift64(&xs64_state) % (uint64_t)(span + 3)) - 1;
        if (x < INT32_MIN || x > INT32_MAX) {
            continue;
        }
        check_ss((int32_t)x, lo, hi);
    }
    printf("smoothstep_q16: max err %.4f LSB\n", ss_max_err);
}

/* ---- epi_cbrt_d ----
 * The contract is correct rounding except possibly in near-halfway cases
 * (design.md). The reference is the true root, from an exact residual, not
 * libm: cbrt is only faithful on macOS and is 1.6 ulp off in places in glibc. */

#define CBRT_D_MAX_ERR_ULP 0.501

/* ulp distance between two finite doubles of the same sign. */
static uint64_t ulp_dist(double a, double b) {
    uint64_t ua = epi_dbits(a) & ~(UINT64_C(1) << 63);
    uint64_t ub = epi_dbits(b) & ~(UINT64_C(1) << 63);
    if ((epi_dbits(a) >> 63) != (epi_dbits(b) >> 63)) {
        return UINT64_MAX;
    }
    return ua > ub ? ua - ub : ub - ua;
}

/* a·b == *p + *e exactly (Dekker), with no reliance on a correct libm fma. */
static void two_prod(double a, double b, double* p, double* e) {
    const double split = 134217729.0; /* 2^27 + 1 */
    double t = split * a;
    double ah = t - (t - a), al = a - ah;
    t = split * b;
    double bh = t - (t - b), bl = b - bh;
    *p = a * b;
    *e = ((ah * bh - *p) + ah * bl + al * bh) + al * bl;
}

/* |y − cbrt(x)| in ulps of y, for y close to the root: one Newton step from y,
 * with y³ − x carried exactly enough to resolve far less than an ulp. */
static double cbrt_err_ulp(double x, double y) {
    double y2, y2_err, y3, y3_err;
    two_prod(y, y, &y2, &y2_err);
    two_prod(y2, y, &y3, &y3_err);
    double residual = (y3 - x) + y3_err + y2_err * y;
    double ulp = epi_dfrom(epi_dbits(y) + 1) - y;
    return fabs(residual / (3.0 * y2)) / ulp;
}

static double cbrt_d_max_err;
static uint64_t cbrt_d_count;

static void check_cbrt_d(double x) {
    double got = epi_cbrt_d(x);
    double err = cbrt_err_ulp(x, got);
    cbrt_d_count++;
    if (err > cbrt_d_max_err) {
        cbrt_d_max_err = err;
    }
    if (err > CBRT_D_MAX_ERR_ULP) {
        char buf[120];
        snprintf(buf, sizeof buf, "cbrt_d(%a) = %a, %.3f ulp from the true root", x, got, err);
        TEST_FAIL_MESSAGE(buf);
    }
}

void test_math_cbrt_d(void) {
    cbrt_d_max_err = 0;
    cbrt_d_count = 0;
    TEST_ASSERT_TRUE(epi_dbits(epi_cbrt_d(0.0)) == epi_dbits(0.0));
    TEST_ASSERT_TRUE(epi_cbrt_d(1.0) == 1.0);
    TEST_ASSERT_TRUE(epi_cbrt_d(0.125) == 0.5);
    /* Exact cubes (k/2^16)³ across the domain. */
    for (int64_t k = 1626; k <= 82570; k++) {
        double c = (double)(k * k * k) * 0x1p-48;
        if (epi_cbrt_d(c) != (double)k * 0x1p-16) {
            fail_msg("cbrt_d((%lld/2^16)^3) != %lld/2^16", k, k, 0);
        }
    }
    /* Powers of two 2^(3j). */
    for (int j = -5; j <= 0; j++) {
        TEST_ASSERT_TRUE(epi_cbrt_d(ldexp(1.0, 3 * j)) == ldexp(1.0, j));
    }
    /* Dense geometric sweep of the domain, ratio 1 + 2^-15. */
    for (double x = 0x1p-16; x < 2.0; x *= 1.0 + 0x1p-15) {
        check_cbrt_d(x);
    }
    /* Every level_y / 2^16. */
    for (int32_t y = 1; y <= EPI_Q16_ONE; y++) {
        check_cbrt_d(y / 65536.0);
    }
    /* Random mantissas in every octave of the domain. */
    xs64_state = UINT64_C(0x1F2E3D4C5B6A7988);
    for (int i = 0; i < 1000000; i++) {
        uint64_t u = xorshift64(&xs64_state);
        check_cbrt_d(epi_dfrom((u >> 12) | ((uint64_t)(1007 + i % 17) << 52)));
    }
    check_cbrt_d(0x1p-16);                                 /* lower end */
    check_cbrt_d(epi_dfrom(UINT64_C(0x3FFFFFFFFFFFFFFF))); /* upper end, just below 2 */
    printf("cbrt_d: max %.6f ulp from the true root over %" PRIu64 " values\n", cbrt_d_max_err, cbrt_d_count);
}

/* ---- epi_exp_d ----
 * The contract is ≤ 1 ulp where the result is normal. epi_exp_d is
 * faithful, not correctly rounded: it differs from macOS libm by 1 ulp in
 * ~10% of values, and an offline exact (Python Decimal) check of 1000 such
 * differences in [−64, 0] found libm the correctly rounded one in 993.
 * The Gaussian kernel's −i²/2σ² is checked densely over [−64, 0]; the sweep
 * covers every normal result. */

static uint64_t exp_d_max_ulp;
static uint64_t exp_d_diff_count;
static uint64_t exp_d_count;

static void check_exp_d(double x) {
    double ref = exp(x);
    if (ref < 0x1p-1022) {
        return; /* subnormal results are rounded twice; tested below */
    }
    double got = epi_exp_d(x);
    uint64_t d = ulp_dist(got, ref);
    exp_d_count++;
    if (d > 0) {
        exp_d_diff_count++;
    }
    if (d > exp_d_max_ulp) {
        exp_d_max_ulp = d;
    }
    if (d > 1) {
        char buf[120];
        snprintf(buf, sizeof buf, "exp_d(%a) = %a, libm %a", x, got, ref);
        TEST_FAIL_MESSAGE(buf);
    }
}

void test_math_exp_d(void) {
    exp_d_max_ulp = exp_d_diff_count = exp_d_count = 0;
    TEST_ASSERT_TRUE(epi_exp_d(0.0) == 1.0);
    TEST_ASSERT_TRUE(epi_exp_d(-0.0) == 1.0);
    TEST_ASSERT_TRUE(epi_exp_d(-0x1.fffffffffffffp+1023) == 0.0);
    TEST_ASSERT_TRUE(epi_exp_d(-0x1p297) == 0.0); /* kernel, smallest float σ */
    TEST_ASSERT_TRUE(epi_exp_d(-746.5) == 0.0);
    /* The subnormal tail stays non-negative. */
    TEST_ASSERT_TRUE(epi_exp_d(-745.1) == 0x1p-1074);
    TEST_ASSERT_TRUE(epi_exp_d(-740.0) > 0.0 && epi_exp_d(-740.0) < 0x1p-1022);
    /* Monotone over a fine grid across many reduction intervals. */
    double prev = 0.0;
    for (double x = -20.0; x <= 0.0; x += 0x1p-12) {
        double y = epi_exp_d(x);
        TEST_ASSERT_TRUE(y >= prev);
        prev = y;
    }
    /* Kernel domain, dense: [−64, 0] in steps of 2^-14. */
    for (double x = -64.0; x <= 0.0; x += 0x1p-14) {
        check_exp_d(x);
    }
    uint64_t dense_count = exp_d_count, dense_diff = exp_d_diff_count;
    /* Every normal result: [−746, 0] in steps of 2^-10, plus random. */
    for (double x = -746.0; x <= 0.0; x += 0x1p-10) {
        check_exp_d(x);
    }
    xs64_state = UINT64_C(0x3F84D5B5B5470917);
    for (int i = 0; i < 1 << 18; i++) {
        /* uniform in [−745, 0] from 53 random bits */
        double u = (double)(xorshift64(&xs64_state) >> 11) * 0x1p-53;
        check_exp_d(-745.0 * u);
    }
    printf(
        "exp_d: max %" PRIu64 " ulp vs libm; differs from libm in %" PRIu64 " of %" PRIu64
        " kernel-domain values, %" PRIu64 " of %" PRIu64 " overall\n",
        exp_d_max_ulp,
        dense_diff,
        dense_count,
        exp_d_diff_count,
        exp_d_count
    );
}

/* ---- epi_sin_d ---- */

static double ulps(double got, double ref) {
    if (got == ref) {
        return 0.0;
    }
    double a = fabs(ref);
    double u = nextafter(a, INFINITY) - a;
    return fabs(got - ref) / u;
}

void test_math_sin(void) {
    uint64_t s = 0x9E3779B97F4A7C15u;
    double worst = 0.0;
    unsigned long differ = 0, n = 0;
    /* Every Lanczos argument is x·π with |x| < 3 (and x/3·π); sweep that
     * range densely, then random arguments over the whole domain. */
    for (long i = -3000000; i <= 3000000; i++) {
        double x = ((double)i / 1000000.0) * 0x1.921fb54442d18p+1;
        double e = ulps(epi_sin_d(x), sin(x));
        differ += e != 0.0;
        worst = e > worst ? e : worst;
        n++;
    }
    for (int i = 0; i < 2000000; i++) {
        double x = ((double)(xorshift64(&s) >> 11) / 9007199254740992.0 * 2.0 - 1.0) * 823549.0;
        if (i & 1) {
            x *= 1e-5;
        }
        double e = ulps(epi_sin_d(x), sin(x));
        differ += e != 0.0;
        worst = e > worst ? e : worst;
        n++;
    }
    printf("  sin: worst %.3f ulp vs libm; %lu of %lu differ\n", worst, differ, n);
    TEST_ASSERT_TRUE(worst <= 1.0);
    /* Exact properties. */
    TEST_ASSERT_TRUE(epi_sin_d(0.0) == 0.0 && !signbit(epi_sin_d(0.0)));
    TEST_ASSERT_TRUE(epi_sin_d(-0.0) == 0.0 && signbit(epi_sin_d(-0.0)));
    for (int i = 0; i < 1000; i++) {
        double x = (double)(xorshift64(&s) >> 11) / 9007199254740992.0 * 20.0;
        TEST_ASSERT_TRUE(epi_sin_d(-x) == -epi_sin_d(x));
    }
    TEST_ASSERT_TRUE(isnan(epi_sin_d(1e7)));
    TEST_ASSERT_TRUE(isnan(epi_sin_d(INFINITY)));
    TEST_ASSERT_TRUE(isnan(epi_sin_d(NAN)));
}

/* ---- epi_sqrt_d ----
 * The contract is within 1 ulp. libm's sqrt is correctly rounded (IEEE 754), so
 * it is the reference here. */

void test_math_sqrt_d(void) {
    uint64_t s = UINT64_C(0x3C6EF372FE94F82A);
    double worst = 0.0;
    unsigned long differ = 0, n = 0;
    /* Every normal exponent, then the range the hull's normals reach. */
    for (int i = 0; i < 1 << 20; i++) {
        uint64_t u = xorshift64(&s);
        uint64_t exp = i & 1 ? 1 + (u >> 52) % 2046 : 1023 - 100 + (u >> 52) % 120;
        double x = epi_dfrom((u & ((UINT64_C(1) << 52) - 1)) | (exp << 52));
        double e = ulps(epi_sqrt_d(x), sqrt(x));
        differ += e != 0.0;
        worst = e > worst ? e : worst;
        n++;
    }
    printf("  sqrt_d: worst %.3f ulp vs libm; %lu of %lu differ\n", worst, differ, n);
    TEST_ASSERT_TRUE(worst <= 1.0);
    /* Exact results. */
    TEST_ASSERT_TRUE(epi_dbits(epi_sqrt_d(0.0)) == epi_dbits(0.0));
    TEST_ASSERT_TRUE(epi_sqrt_d(1.0) == 1.0);
    TEST_ASSERT_TRUE(epi_sqrt_d(0.25) == 0.5);
    for (int k = 1; k < 1 << 16; k += 3) {
        TEST_ASSERT_TRUE(epi_sqrt_d((double)k * k) == (double)k);
    }
    for (int j = -500; j <= 500; j++) {
        TEST_ASSERT_TRUE(epi_sqrt_d(ldexp(1.0, 2 * j)) == ldexp(1.0, j));
    }
    TEST_ASSERT_TRUE(epi_sqrt_d(0x1p-1022) == 0x1p-511);
}

/* ---- determinism fingerprints ---- */

typedef struct {
    uint64_t h;
} fnv_t;

static void fnv_init(fnv_t* f) {
    f->h = UINT64_C(0xCBF29CE484222325);
}

static void fnv_u64(fnv_t* f, uint64_t v, unsigned bytes) {
    for (unsigned i = 0; i < bytes; i++) {
        f->h ^= (v >> (8 * i)) & 0xFFu;
        f->h *= UINT64_C(0x100000001B3);
    }
}

static void fnv_i32(fnv_t* f, int32_t v) {
    fnv_u64(f, (uint64_t)(uint32_t)v, 4);
}

#define FP_ROUND_SHIFT UINT64_C(0x0152e98d5863b6d4)
#define FP_CBRT_Q16 UINT64_C(0x2db8c2ac7b4e6115)
#define FP_ISQRT64 UINT64_C(0xd8452e7d01f4fda7)
#define FP_HYPOT_Q UINT64_C(0x8df70783eca2ad3a)
#define FP_ATAN2 UINT64_C(0x753f1be5fb696004)
#define FP_SMOOTHSTEP UINT64_C(0xc89e46305aeffc0c)
#define FP_CBRT_D UINT64_C(0x0406eb45fadcc85c)
#define FP_EXP_D UINT64_C(0x5e3bd3558603da42)
#define FP_SQRT_D UINT64_C(0x4822015173b03f12)

static int fp_report(const char* name, uint64_t got, uint64_t want) {
    printf("fingerprint %-14s 0x%016" PRIx64 "%s\n", name, got, got == want ? "" : "  MISMATCH");
    return got == want;
}

void test_math_fingerprints(void) {
    int ok = 1;
    fnv_t f;

    fnv_init(&f);
    xs64_state = UINT64_C(0x243F6A8885A308D3);
    for (int i = 0; i < 65536; i++) {
        int64_t x = wrap_i64(xorshift64(&xs64_state));
        for (unsigned s = 1; s <= 63; s++) {
            fnv_u64(&f, (uint64_t)epi_round_shift(x, s), 8);
        }
    }
    ok &= fp_report("round_shift", f.h, FP_ROUND_SHIFT);

    fnv_init(&f);
    for (int32_t x = -(1 << 20); x <= (1 << 20); x++) {
        fnv_i32(&f, epi_cbrt_q16(x));
    }
    xs64_state = UINT64_C(0x13198A2E03707344);
    for (int i = 0; i < 1 << 20; i++) {
        fnv_i32(&f, epi_cbrt_q16(wrap_i32((uint32_t)xorshift64(&xs64_state))));
    }
    fnv_i32(&f, epi_cbrt_q16(INT32_MIN));
    fnv_i32(&f, epi_cbrt_q16(INT32_MAX));
    ok &= fp_report("cbrt_q16", f.h, FP_CBRT_Q16);

    fnv_init(&f);
    xs64_state = UINT64_C(0xA4093822299F31D0);
    for (int i = 0; i < 1 << 20; i++) {
        fnv_u64(&f, epi_isqrt64(xorshift64(&xs64_state) >> (i % 64)), 4);
    }
    fnv_u64(&f, epi_isqrt64(UINT64_MAX), 4);
    ok &= fp_report("isqrt64", f.h, FP_ISQRT64);

    fnv_init(&f);
    xs64_state = UINT64_C(0x082EFA98EC4E6C89);
    for (int i = 0; i < 1 << 20; i++) {
        uint64_t u = xorshift64(&xs64_state);
        unsigned sh = (unsigned)(i % 32);
        fnv_i32(&f, epi_hypot_q(wrap_i32((uint32_t)u >> sh), wrap_i32((uint32_t)(u >> 32) >> sh)));
    }
    ok &= fp_report("hypot_q", f.h, FP_HYPOT_Q);

    fnv_init(&f);
    for (int32_t y = -300; y <= 300; y++) {
        for (int32_t x = -300; x <= 300; x++) {
            fnv_i32(&f, epi_atan2_deg_q8(y, x));
        }
    }
    xs64_state = UINT64_C(0x452821E638D01377);
    for (int i = 0; i < 1 << 20; i++) {
        uint64_t u = xorshift64(&xs64_state);
        unsigned sh = (unsigned)(i % 32);
        fnv_i32(&f, epi_atan2_deg_q8(wrap_i32((uint32_t)u >> sh), wrap_i32((uint32_t)(u >> 32) >> sh)));
    }
    ok &= fp_report("atan2_deg_q8", f.h, FP_ATAN2);

    fnv_init(&f);
    xs64_state = UINT64_C(0xBE5466CF34E90C6C);
    for (int i = 0; i < 1 << 20; i++) {
        uint64_t u = xorshift64(&xs64_state);
        unsigned sh = (unsigned)(i % 32);
        int32_t a = wrap_i32((uint32_t)u >> sh);
        int32_t b = wrap_i32((uint32_t)(u >> 32) >> sh);
        int32_t lo = a < b ? a : b;
        int32_t hi = a < b ? b : a;
        int32_t x = wrap_i32((uint32_t)xorshift64(&xs64_state) >> sh);
        fnv_i32(&f, epi_smoothstep_q16(x, lo, hi));
        int64_t near = (int64_t)lo + i % 1000; /* points just above lo, ≤ hi */
        fnv_i32(&f, epi_smoothstep_q16(near <= hi ? (int32_t)near : hi, lo, hi));
    }
    for (int32_t x = -10; x <= 65546; x++) {
        fnv_i32(&f, epi_smoothstep_q16(x, 0, 65536));
    }
    ok &= fp_report("smoothstep", f.h, FP_SMOOTHSTEP);

    /* cbrt_d: inputs built only from exact operations on integers and bit
     * patterns and a correctly rounded multiply: every level_y / 2^16, a
     * geometric sweep of the domain, random mantissas in each of its octaves,
     * and its upper end. */
    fnv_init(&f);
    for (int32_t y = 0; y <= EPI_Q16_ONE; y++) {
        fnv_u64(&f, epi_dbits(epi_cbrt_d(y / 65536.0)), 8);
    }
    for (double x = 0x1p-16; x < 2.0; x *= 1.000030517578125) {
        fnv_u64(&f, epi_dbits(epi_cbrt_d(x)), 8);
    }
    xs64_state = UINT64_C(0xC0AC29B7C97C50DD);
    for (int i = 0; i < 1 << 18; i++) {
        uint64_t u = xorshift64(&xs64_state);
        fnv_u64(&f, epi_dbits(epi_cbrt_d(epi_dfrom((u >> 12) | ((uint64_t)(1007 + i % 17) << 52)))), 8);
    }
    fnv_u64(&f, epi_dbits(epi_cbrt_d(epi_dfrom(UINT64_C(0x3FFFFFFFFFFFFFFF)))), 8);
    ok &= fp_report("cbrt_d", f.h, FP_CBRT_D);

    /* sqrt_d: random mantissas over every normal exponent, built only from bit
     * patterns, and the ends. */
    fnv_init(&f);
    xs64_state = UINT64_C(0xA54FF53A5F1D36F1);
    for (int i = 0; i < 1 << 18; i++) {
        uint64_t u = xorshift64(&xs64_state);
        fnv_u64(&f, epi_dbits(epi_sqrt_d(epi_dfrom((u >> 12) | ((uint64_t)(1 + i % 2046) << 52)))), 8);
    }
    fnv_u64(&f, epi_dbits(epi_sqrt_d(0x1p-1022)), 8);
    fnv_u64(&f, epi_dbits(epi_sqrt_d(0x1.fffffffffffffp+1023)), 8);
    ok &= fp_report("sqrt_d", f.h, FP_SQRT_D);

    /* exp_d: dyadic inputs (exact sums of powers of two) on a grid through the
     * underflow to 0, densely over [−64, 0], and random 53-bit ones in
     * [−1024, 0] (normal and subnormal results and the underflow); the kernel's
     * own arguments for every σ = k/100; random bit patterns over every finite
     * x < 0; and the ends. */
    fnv_init(&f);
    for (double x = -750.0; x <= 0.0; x += 0x1p-8) {
        fnv_u64(&f, epi_dbits(epi_exp_d(x)), 8);
    }
    for (double x = -64.0; x <= 0.0; x += 0x1p-13) {
        fnv_u64(&f, epi_dbits(epi_exp_d(x)), 8);
    }
    xs64_state = UINT64_C(0x9216D5D98979FB1B);
    for (int i = 0; i < 1 << 18; i++) {
        fnv_u64(&f, epi_dbits(epi_exp_d(-(double)(xorshift64(&xs64_state) >> 11) * 0x1p-43)), 8);
    }
    for (int k = 1; k <= 1050; k++) {
        const double sigma = (float)k / 100.0f;
        const double den = 2.0 * sigma * sigma;
        const int r = (3 * k + 99) / 100;
        for (int i = 1; i <= r; i++) {
            const double di = (double)i;
            fnv_u64(&f, epi_dbits(epi_exp_d(-(di * di) / den)), 8);
        }
    }
    for (int i = 0; i < 1 << 16; i++) {
        uint64_t u = xorshift64(&xs64_state);
        if (((u >> 52) & 0x7FFu) == 0x7FFu) {
            continue; /* skip −inf/NaN */
        }
        fnv_u64(&f, epi_dbits(epi_exp_d(epi_dfrom(u | (UINT64_C(1) << 63)))), 8);
    }
    fnv_u64(&f, epi_dbits(epi_exp_d(-0.0)), 8);
    fnv_u64(&f, epi_dbits(epi_exp_d(0.0)), 8);
    fnv_u64(&f, epi_dbits(epi_exp_d(-0x1p297)), 8);
    fnv_u64(&f, epi_dbits(epi_exp_d(-0x1.fffffffffffffp+1023)), 8);
    ok &= fp_report("exp_d", f.h, FP_EXP_D);

    TEST_ASSERT_TRUE_MESSAGE(ok, "determinism fingerprint mismatch (see log)");
}
