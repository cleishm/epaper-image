/*
 * Randomised API harness for the sanitizer CI job.
 *
 * Drives the resampler and the converter with random, deliberately hostile
 * configurations (degenerate and edge-hugging source rectangles, extreme
 * placements, random grey panels, 1x1 up to 65535-wide sources), with every
 * buffer allocated at exactly the size the API reports so AddressSanitizer
 * sees any overrun. A separate pass checks the source size limit with sizes
 * and limits at and beyond its edges. Each pipeline must emit exactly as many
 * rows as its configuration promises.
 *
 * Every check exits nonzero with a message in every build type; the
 * sanitizers report the memory and undefined-behaviour errors.
 *
 *   epaper_image_fuzz [iterations [seed]]
 *
 * iterations (default 3000) is the resampler count; the source-limit check
 * runs as many, and the converter a third as many. The default seed is fixed,
 * so a run is reproducible; a seed, if given, is mixed into it. Built with
 * -DEPAPER_IMAGE_BUILD_FUZZ=ON;
 * run under ctest, ideally with -fsanitize=address,undefined.
 */
#include "epaper_image.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                                       \
            fprintf(stderr, __VA_ARGS__);                                                                              \
            fputc('\n', stderr);                                                                                       \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

static uint64_t st = 0x9E3779B97F4A7C15ull;

static uint32_t rnd(void) {
    st ^= st << 13;
    st ^= st >> 7;
    st ^= st << 17;
    return (uint32_t)(st >> 16);
}

static uint32_t ru(uint32_t n) {
    return n ? rnd() % n : 0;
}

static float rf01(void) {
    return (float)rnd() / 4294967296.0f;
}

static uint16_t rdim(int big) {
    static const uint16_t s[] = {1,  1,   2,   2,   3,   4,   5,   7,    8,    16,   17,  31,
                                 64, 100, 101, 255, 256, 333, 640, 1000, 1200, 1600, 4000};
    if (big && ru(10) == 0) {
        return (uint16_t)(ru(2) ? 65535 : 32768 + ru(32768));
    }
    return s[ru(sizeof s / sizeof s[0])];
}

/* A placement offset along one axis: an extreme, inside or beyond the canvas,
 * or cur unchanged. */
static int32_t rplace(int32_t cur, uint32_t len, uint32_t canvas) {
    switch (ru(7)) {
    case 0:
        return INT32_MIN;
    case 1:
        return INT32_MAX;
    case 2:
        return -(int32_t)ru(len + 2u);
    case 3:
        return (int32_t)ru(canvas + 2u);
    default:
        return cur;
    }
}

static long rs_ok;
static long rs_invalid;
static long rs_skip;
static long rs_rows;

static void fuzz_resampler(int iters) {
    for (int it = 0; it < iters; it++) {
        epi_resampler_config_t c;
        memset(&c, 0, sizeof c);
        c.src_width = rdim(1);
        c.src_height = rdim(1);
        if (ru(6) == 0) { /* tall path */
            c.src_width = (uint16_t)(1 + ru(4));
            c.src_height = (uint16_t)(101 + ru(3000));
        }
        c.dst_width = rdim(0);
        c.dst_height = rdim(0);
        c.filter = (epi_filter_t)ru(4);
        c.max_src_pixels = ru(8) ? UINT32_MAX : 0; /* mostly lifted, to keep the size coverage */
        c.background = (epi_rgb_t){(uint8_t)rnd(), (uint8_t)rnd(), (uint8_t)rnd()};
        switch (ru(8)) {
        case 0:
            epi_layout_cover(&c);
            break;
        case 1:
            epi_layout_fit(&c);
            break;
        case 2: { /* integer crop the size of the output */
            uint16_t w = (uint16_t)(1 + ru(c.src_width));
            uint16_t h = (uint16_t)(1 + ru(c.src_height));
            c.dst_w = w;
            c.dst_h = h;
            c.src_x = (float)ru(c.src_width - w + 1u);
            c.src_y = (float)ru(c.src_height - h + 1u);
            c.src_w = w;
            c.src_h = h;
            break;
        }
        case 3: /* collapsed: x0 + w == x0, including right at the far edge */
            c.src_x = ru(2) ? (float)c.src_width : (float)ru(c.src_width);
            c.src_y = ru(2) ? (float)c.src_height : (float)ru(c.src_height);
            c.src_w = ru(2) ? FLT_MIN : 1e-30f;
            c.src_h = ru(2) ? FLT_MIN : 1e-30f;
            c.dst_w = (uint16_t)(1 + ru(300));
            c.dst_h = (uint16_t)(1 + ru(300));
            break;
        case 4: { /* hugging the far edge */
            float wf = ru(2) ? 0.001f : 0.5f;
            float hf = ru(2) ? 0.001f : 0.5f;
            c.src_w = wf;
            c.src_h = hf;
            c.src_x = (float)c.src_width - wf;
            c.src_y = (float)c.src_height - hf;
            c.dst_w = (uint16_t)(1 + ru(400));
            c.dst_h = (uint16_t)(1 + ru(400));
            break;
        }
        case 5: /* tiny x0, huge extent: x1 - x0 rounds */
            c.src_x = 1e-6f * (float)ru(1000);
            c.src_y = 1e-6f * (float)ru(1000);
            c.src_w = (float)c.src_width;
            c.src_h = (float)c.src_height;
            c.dst_w = (uint16_t)(1 + ru(2000));
            c.dst_h = (uint16_t)(1 + ru(2000));
            break;
        default: { /* arbitrary float rect */
            float x = rf01() * (float)c.src_width;
            float y = rf01() * (float)c.src_height;
            c.src_x = x;
            c.src_y = y;
            c.src_w = rf01() * ((float)c.src_width - x);
            c.src_h = rf01() * ((float)c.src_height - y);
            c.dst_w = (uint16_t)(1 + ru(ru(4) ? 400 : 65535));
            c.dst_h = (uint16_t)(1 + ru(ru(4) ? 400 : 65535));
            break;
        }
        }
        c.dst_x = rplace(c.dst_x, c.dst_w, c.dst_width);
        c.dst_y = rplace(c.dst_y, c.dst_h, c.dst_height);

        size_t need = epi_resampler_workspace_size(&c);
        if (need == 0) {
            epi_resampler_t* rr = NULL;
            char b[8];
            CHECK(
                epi_resampler_init(&c, b, sizeof b, &rr) != EPI_OK, "init succeeded for a config with workspace size 0"
            );
            rs_invalid++;
            continue;
        }
        if (need > (64u << 20) || (uint64_t)c.src_width * c.src_height > 3000000u) {
            rs_skip++;
            continue;
        }
        unsigned mis = ru(8);
        uint8_t* raw = malloc(need + mis);
        CHECK(raw != NULL, "out of memory");
        epi_resampler_t* r = NULL;
        CHECK(
            epi_resampler_init(&c, raw + mis, need - 1, &r) == EPI_ERR_WORKSPACE_TOO_SMALL,
            "init accepted a workspace one byte short"
        );
        CHECK(epi_resampler_init(&c, raw + mis, need, &r) == EPI_OK, "init failed with the reported workspace size");
        epi_rgb_t* in = malloc((size_t)c.src_width * sizeof *in);
        epi_rgb_t* out = malloc((size_t)c.dst_width * sizeof *out);
        CHECK(in != NULL && out != NULL, "out of memory");
        uint32_t got = 0;
        while (epi_resampler_pop(r, out) == EPI_OK) {
            got++;
        }
        for (uint32_t y = 0; y < c.src_height; y++) {
            for (uint32_t x = 0; x < c.src_width; x++) {
                uint32_t v = rnd();
                in[x] = (epi_rgb_t){(uint8_t)-(v & 1), (uint8_t)-((v >> 1) & 1), (uint8_t)-((v >> 2) & 1)};
            }
            CHECK(epi_resampler_push(r, in) == EPI_OK, "push of row %u failed", (unsigned)y);
            while (epi_resampler_pop(r, out) == EPI_OK) {
                got++;
                rs_rows++;
            }
        }
        CHECK(
            got == c.dst_height,
            "resampler emitted %u rows, want %u (src=%ux%u dst=%ux%u rect=%a,%a,%a,%a place=%d,%d,%u,%u filter=%d)",
            (unsigned)got,
            (unsigned)c.dst_height,
            (unsigned)c.src_width,
            (unsigned)c.src_height,
            (unsigned)c.dst_width,
            (unsigned)c.dst_height,
            (double)c.src_x,
            (double)c.src_y,
            (double)c.src_w,
            (double)c.src_h,
            (int)c.dst_x,
            (int)c.dst_y,
            (unsigned)c.dst_w,
            (unsigned)c.dst_h,
            (int)c.filter
        );
        free(in);
        free(out);
        free(raw);
        rs_ok++;
    }
    printf("resampler: ok=%ld invalid=%ld skipped=%ld rows=%ld\n", rs_ok, rs_invalid, rs_skip, rs_rows);
}

static uint64_t seed_arg; /* the seed given on the command line, for failure messages */

static uint32_t rsize32(void) {
    switch (ru(7)) {
    case 0:
        return 0;
    case 1:
        return 1;
    case 2:
        return 2 + ru(200);
    case 3:
        return 65535;
    case 4:
        return 65536;
    case 5:
        return UINT32_MAX;
    default:
        return rnd();
    }
}

/* Source sizes and limits at and beyond the edges, with the oracle: a zero
 * side is invalid; a side over 65535 or an area over the effective limit is
 * too large; anything else may still be rejected by other rules. */
static void fuzz_limits(int iters) {
    long zero = 0, large = 0, other = 0, built = 0;
    for (int it = 0; it < iters; it++) {
        epi_resampler_config_t c;
        memset(&c, 0, sizeof c);
        c.src_width = rsize32();
        c.src_height = rsize32();
        c.dst_width = rdim(0);
        c.dst_height = rdim(0);
        c.filter = (epi_filter_t)ru(4);
        const uint64_t area = (uint64_t)c.src_width * c.src_height;
        const uint32_t area32 = area > UINT32_MAX ? UINT32_MAX : (uint32_t)area;
        switch (ru(6)) {
        case 0:
            c.max_src_pixels = 0;
            break;
        case 1:
            c.max_src_pixels = 1;
            break;
        case 2:
            c.max_src_pixels = rnd();
            break;
        case 3:
            c.max_src_pixels = area32;
            break;
        case 4:
            c.max_src_pixels = area32 ? area32 - 1 : 0;
            break;
        default:
            c.max_src_pixels = UINT32_MAX;
            break;
        }
        if (ru(2)) {
            epi_layout_cover(&c);
        } else {
            epi_layout_fit(&c);
        }
        const uint32_t limit = c.max_src_pixels ? c.max_src_pixels : EPI_DEFAULT_MAX_SRC_PIXELS;
        const size_t need = epi_resampler_workspace_size(&c);
        uint64_t dummy[2];
        epi_resampler_t* r = (epi_resampler_t*)(void*)dummy; /* non-NULL sentinel */
        const epi_err_t e = epi_resampler_init(&c, dummy, sizeof dummy, &r);
#define LIMITS_CHECK(cond, what)                                                                                       \
    CHECK(                                                                                                             \
        cond,                                                                                                          \
        what " (size %zu, init %s) [seed=%llu iteration=%d src=%ux%u limit=%u dst=%ux%u]",                             \
        need,                                                                                                          \
        epi_strerror(e),                                                                                               \
        (unsigned long long)seed_arg,                                                                                  \
        it,                                                                                                            \
        (unsigned)c.src_width,                                                                                         \
        (unsigned)c.src_height,                                                                                        \
        (unsigned)c.max_src_pixels,                                                                                    \
        (unsigned)c.dst_width,                                                                                         \
        (unsigned)c.dst_height                                                                                         \
    )
        if (c.src_width == 0 || c.src_height == 0) {
            LIMITS_CHECK(need == 0, "zero side was sized");
            LIMITS_CHECK(e == EPI_ERR_INVALID_ARG && r == NULL, "zero side not invalid");
            zero++;
        } else if (c.src_width > 65535 || c.src_height > 65535 || area > limit) {
            LIMITS_CHECK(need == 0, "over-limit source was sized");
            LIMITS_CHECK(e == EPI_ERR_SOURCE_TOO_LARGE && r == NULL, "over-limit source not too large");
            large++;
        } else {
            other++;
            if (need == 0) {
                LIMITS_CHECK(e != EPI_OK && r == NULL, "size 0 but init succeeded");
            } else if (need < (1u << 20)) {
                uint8_t* raw = malloc(need);
                CHECK(raw != NULL, "out of memory");
                r = NULL;
                const epi_err_t e2 = epi_resampler_init(&c, raw, need, &r);
                CHECK(
                    e2 == EPI_OK && r != NULL,
                    "init failed with the reported size: %s [seed=%llu iteration=%d]",
                    epi_strerror(e2),
                    (unsigned long long)seed_arg,
                    it
                );
                free(raw);
                built++;
            }
        }
#undef LIMITS_CHECK
    }
    CHECK(
        zero > 0 && large > 0 && other > 0,
        "limits: zero=%ld large=%ld other=%ld [seed=%llu]",
        zero,
        large,
        other,
        (unsigned long long)seed_arg
    );
    printf("limits: zero=%ld too-large=%ld other=%ld built=%ld\n", zero, large, other, built);
}

static void fill(epi_rgb_t* row, uint32_t w, int mode, uint32_t y) {
    static const epi_rgb_t prim[] = {
        {255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 0}, {0, 255, 255}, {255, 0, 255}, {255, 255, 255}, {0, 0, 0}
    };
    for (uint32_t x = 0; x < w; x++) {
        switch (mode) {
        case 0:
            row[x] = (epi_rgb_t){(uint8_t)rnd(), (uint8_t)rnd(), (uint8_t)rnd()};
            break;
        case 1:
            row[x] = prim[ru(8)];
            break;
        case 2:
            row[x] = ((x ^ y) & 1) ? prim[6] : prim[7];
            break;
        case 3:
            row[x] = prim[y % 8];
            break;
        case 4:
            row[x] = (x * 2 < w) ? prim[0] : prim[2];
            break;
        case 5:
            row[x] = prim[1];
            break;
        case 6:
            row[x] = ((x ^ y) & 1) ? prim[0] : prim[1];
            break;
        default:
            row[x] = (epi_rgb_t){(uint8_t)(x * 255 / (w ? w : 1)), (uint8_t)y, (uint8_t)(255 - x)};
            break;
        }
    }
}

static const epi_panel_t* rand_grey(uint8_t* store, size_t cap) {
    epi_ink_t lv[16];
    memset(lv, 0, sizeof lv);
    epi_grey_panel_config_t g;
    memset(&g, 0, sizeof g);
    g.name = "g";
    g.width = 100;
    g.height = 100;
    g.levels = lv;
    g.level_count = 2 + ru(15);
    uint32_t v = ru(40);
    for (size_t i = 0; i < g.level_count; i++) {
        v += 1 + ru(255 / (uint32_t)g.level_count);
        if (v > 255) {
            v = 255;
        }
        lv[i].code = (uint8_t)i;
        lv[i].measured = (epi_rgb_t){(uint8_t)v, (uint8_t)v, (uint8_t)v};
    }
    if (ru(2)) {
        lv[0].measured = (epi_rgb_t){0, 0, 0};
    }
    static const float sig[] = {FLT_TRUE_MIN, 1e-30f, 0.01f, 0.3f, 1.f, 5.f, 10.5f};
    g.detail_sigma = sig[ru(7)];
    const epi_panel_t* p = NULL;
    size_t need = epi_grey_panel_size(&g);
    if (!need || need > cap) {
        return NULL;
    }
    if (epi_grey_panel_init(&g, store + ru(16), need, &p) != EPI_OK) {
        return NULL;
    }
    return p;
}

static long cv_ok;
static long cv_invalid;
static long grey_rand;

static void fuzz_converter(int iters) {
    static uint8_t store[1 << 16];
    for (int it = 0; it < iters; it++) {
        const epi_panel_t* p;
        switch (ru(4)) {
        case 0:
            p = &epi_panel_el133uf1;
            break;
        case 1:
            p = &epi_panel_gdey075t7_mono;
            break;
        case 2:
            p = &epi_panel_gdey075t7_grey4;
            break;
        default:
            p = rand_grey(store, sizeof store - 16);
            if (p) {
                grey_rand++;
            } else {
                p = &epi_panel_el133uf1;
            }
            break;
        }
        epi_intent_t in = {rf01(), rf01(), rf01(), rf01() * 3.0f, rf01() * 3.0f};
        if (ru(3) == 0) {
            in = (epi_intent_t){(float)ru(2), (float)ru(2), (float)ru(2), (float)ru(4), (float)ru(4)};
        }
        epi_config_t c = {
            p,
            ru(4) ? &in : NULL,
            (uint16_t)(1 + ru(ru(8) ? 120 : 1500)),
            (uint16_t)(1 + ru(ru(8) ? 80 : 300)),
            (epi_output_t)ru(6)
        };
        size_t need = epi_converter_workspace_size(&c);
        if (!need) {
            cv_invalid++;
            continue;
        }
        unsigned mis = ru(32);
        uint8_t* raw = malloc(need + mis);
        CHECK(raw != NULL, "out of memory");
        epi_converter_t* cv = NULL;
        CHECK(
            epi_converter_init(&c, raw + mis, need - 1, &cv) == EPI_ERR_WORKSPACE_TOO_SMALL,
            "init accepted a workspace one byte short"
        );
        epi_err_t e = epi_converter_init(&c, raw + mis, need, &cv);
        CHECK(e == EPI_OK, "converter init: %s", epi_strerror(e));
        size_t rs = epi_output_row_size(c.output, c.width);
        uint8_t* out = malloc(rs);
        epi_rgb_t* row = malloc((size_t)c.width * sizeof *row);
        CHECK(out != NULL && row != NULL, "out of memory");
        int mode = (int)ru(8);
        uint32_t got = 0;
        for (uint32_t y = 0; y < c.height; y++) {
            if (ru(16) == 0) {
                mode = (int)ru(8);
            }
            fill(row, c.width, mode, y);
            CHECK(epi_converter_push(cv, row) == EPI_OK, "push of row %u failed", (unsigned)y);
            while (epi_converter_pop(cv, out) == EPI_OK) {
                got++;
            }
        }
        CHECK(got == c.height, "converter emitted %u rows, want %u", (unsigned)got, (unsigned)c.height);
        free(out);
        free(row);
        free(raw);
        cv_ok++;
    }
    printf("converter: ok=%ld invalid=%ld (random grey %ld)\n", cv_ok, cv_invalid, grey_rand);
}

int main(int argc, char** argv) {
    int n = argc > 1 ? atoi(argv[1]) : 3000;
    if (argc > 2) {
        seed_arg = strtoull(argv[2], NULL, 0);
        st ^= seed_arg * 0x2545F4914F6CDD1Dull;
    }
    fuzz_resampler(n);
    fuzz_limits(n);
    fuzz_converter(n / 3);
    return 0;
}
