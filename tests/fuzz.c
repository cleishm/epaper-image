/*
 * Randomised API harness for the sanitizer CI job.
 *
 * Drives the resampler and the converter with random, deliberately hostile
 * configurations (degenerate and edge-hugging source rectangles, extreme
 * placements, random grey panels, 1x1 up to 65535-wide sources), with every
 * buffer allocated at exactly the size the API reports so AddressSanitizer
 * sees any overrun. A separate pass checks the source size limit with sizes
 * and limits at and beyond its edges. Each pipeline must emit exactly as many
 * rows as its configuration promises. The preview reducer and the row packer
 * run the same way on random sizes and codes, including codes they must reject.
 * Random colour ink sets, plausible and extreme, go through
 * epi_colour_panel_init, and each panel it accepts converts a small image with
 * random intents: init must only accept what the converter can run without
 * undefined behaviour.
 *
 * Every check exits nonzero with a message in every build type; the
 * sanitizers report the memory and undefined-behaviour errors.
 *
 *   epaper_image_fuzz [iterations [seed [colour-iterations]]]
 *
 * iterations (default 3000) is the resampler count; the source-limit check
 * runs as many, and the converter, the reducer and the packer a third as many
 * each. An accepted colour panel costs a million in-gamut tests, so the colour count
 * is its own, by default a hundredth of iterations. The default seed is fixed,
 * so a run is reproducible; a seed, if given, is mixed into it. Built with
 * -DEPAPER_IMAGE_BUILD_FUZZ=ON;
 * run under ctest, ideally with -fsanitize=address,undefined.
 */
#include "epaper_image.h"

#include <float.h>
#include <math.h>
#include <stdbool.h>
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
            p, ru(4) ? &in : NULL, (uint16_t)(1 + ru(ru(8) ? 120 : 1500)), (uint16_t)(1 + ru(ru(8) ? 80 : 300))
        };
        size_t need = epi_converter_workspace_size(&c);
        CHECK(need != 0, "workspace size rejected a valid config");
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
        uint8_t* out = malloc(c.width);
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
    printf("converter: ok=%ld (random grey %ld)\n", cv_ok, grey_rand);
}

/* A built-in panel, or sometimes a random grey one (codes 0..level_count - 1). */
static const epi_panel_t* rand_panel(uint8_t* store, size_t cap) {
    switch (ru(4)) {
    case 0:
        return &epi_panel_el133uf1;
    case 1:
        return &epi_panel_gdey075t7_mono;
    case 2:
        return &epi_panel_gdey075t7_grey4;
    default: {
        const epi_panel_t* p = rand_grey(store, cap);
        return p ? p : &epi_panel_el133uf1;
    }
    }
}

/* A code that is not one of the panel's inks. */
static uint8_t foreign_code(const epi_panel_t* p) {
    for (uint32_t code = ru(256);; code = (code + 1) & 255) {
        size_t i = 0;
        while (i < p->ink_count && p->inks[i].code != code) {
            i++;
        }
        if (i == p->ink_count) {
            return (uint8_t)code;
        }
    }
}

/* Random sizes (sometimes invalid), one workspace byte short and then exact, at
 * any alignment; rows of random codes, with rejected rows among them. The row
 * count must be right, and a block of one ink must come out as that ink. */
static void fuzz_preview(int iters) {
    static uint8_t store[1 << 16];
    long ok = 0, invalid = 0, rejected = 0, rows = 0;
    for (int it = 0; it < iters; it++) {
        const epi_panel_t* p = rand_panel(store, sizeof store - 16);
        epi_preview_config_t c;
        memset(&c, 0, sizeof c);
        c.panel = p;
        c.src_width = rdim(1);
        c.src_height = rdim(1);
        if ((uint64_t)c.src_width * c.src_height > 3000000u) {
            c.src_height = (uint16_t)(1 + ru(40));
        }
        c.dst_width = (uint16_t)(1 + ru(c.src_width));
        c.dst_height = (uint16_t)(1 + ru(c.src_height));
        c.nominal = ru(2) != 0;
        const int broken = ru(16) == 0;
        if (broken) {
            switch (ru(4)) {
            case 0:
                c.dst_width = (uint16_t)(c.src_width + 1u); /* wraps to 0 from 65535: also invalid */
                break;
            case 1:
                c.dst_height = 0;
                break;
            case 2:
                c.src_height = 0;
                break;
            default:
                c.panel = NULL;
                break;
            }
        }
        const size_t need = epi_preview_workspace_size(&c);
        epi_preview_t* r = NULL;
        if (broken) {
            char b[8];
            CHECK(need == 0, "an invalid config was sized");
            CHECK(epi_preview_init(&c, b, sizeof b, &r) == EPI_ERR_INVALID_ARG && r == NULL, "invalid config accepted");
            invalid++;
            continue;
        }
        CHECK(need != 0, "a valid config got size 0");
        unsigned mis = ru(32);
        uint8_t* raw = malloc(need + mis);
        CHECK(raw != NULL, "out of memory");
        CHECK(
            epi_preview_init(&c, raw + mis, need - 1, &r) == EPI_ERR_WORKSPACE_TOO_SMALL,
            "init accepted a workspace one byte short"
        );
        epi_err_t e = epi_preview_init(&c, raw + mis, need, &r);
        CHECK(e == EPI_OK, "preview init: %s", epi_strerror(e));
        uint8_t* in = malloc(c.src_width);
        epi_rgb_t* out = malloc((size_t)c.dst_width * sizeof *out);
        CHECK(in != NULL && out != NULL, "out of memory");
        const size_t ink = ru((uint32_t)p->ink_count);
        const int one_ink = ru(4) == 0;
        const epi_rgb_t want = c.nominal ? p->inks[ink].nominal : p->inks[ink].measured;
        uint32_t got = 0;
        for (uint32_t y = 0; y < c.src_height; y++) {
            for (uint32_t x = 0; x < c.src_width; x++) {
                in[x] = p->inks[one_ink ? ink : ru((uint32_t)p->ink_count)].code;
            }
            if (ru(8) == 0) {
                const uint32_t pos = ru(c.src_width);
                const uint8_t keep = in[pos];
                in[pos] = foreign_code(p);
                CHECK(epi_preview_push(r, in) == EPI_ERR_INVALID_ARG, "foreign code accepted");
                in[pos] = keep;
                rejected++;
            }
            CHECK(epi_preview_push(r, in) == EPI_OK, "push of row %u failed", (unsigned)y);
            while (epi_preview_pop(r, out) == EPI_OK) {
                got++;
                rows++;
                for (uint32_t x = 0; one_ink && x < c.dst_width; x++) {
                    CHECK(
                        out[x].r == want.r && out[x].g == want.g && out[x].b == want.b,
                        "one ink changed colour (src=%ux%u dst=%ux%u)",
                        (unsigned)c.src_width,
                        (unsigned)c.src_height,
                        (unsigned)c.dst_width,
                        (unsigned)c.dst_height
                    );
                }
            }
        }
        CHECK(
            got == c.dst_height,
            "reducer emitted %u rows, want %u (src=%ux%u dst=%ux%u)",
            (unsigned)got,
            (unsigned)c.dst_height,
            (unsigned)c.src_width,
            (unsigned)c.src_height,
            (unsigned)c.dst_width,
            (unsigned)c.dst_height
        );
        CHECK(epi_preview_pop(r, out) == EPI_ERR_FINISHED, "pop past the last row");
        CHECK(epi_preview_push(r, in) == EPI_ERR_FINISHED, "push past the last row");
        free(in);
        free(out);
        free(raw);
        ok++;
    }
    printf("reducer: ok=%ld invalid=%ld rejected rows=%ld rows=%ld\n", ok, invalid, rejected, rows);
}

/* Random widths and codes in an exactly sized row: the codes must unpack again, a
 * partial byte must carry the lightest ink's code, and a panel with a code too
 * wide for the format must be refused. */
static void fuzz_pack(int iters) {
    static uint8_t store[1 << 16];
    static const epi_pack_format_t formats[] = {EPI_PACK_4BPP, EPI_PACK_2BPP, EPI_PACK_1BPP};
    long ok = 0, refused = 0, wide = 0;
    for (int it = 0; it < iters; it++) {
        const epi_panel_t* p = rand_panel(store, sizeof store - 16);
        const epi_pack_format_t format = formats[ru(3)];
        const unsigned bits = format == EPI_PACK_4BPP ? 4u : format == EPI_PACK_2BPP ? 2u : 1u;
        const uint16_t w = (uint16_t)(ru(16) ? 1 + ru(ru(4) ? 64 : 4000) : 65535);
        const size_t rs = epi_pack_row_size(format, w);
        CHECK(rs == ((size_t)w * bits + 7u) / 8u, "row size %zu at width %u", rs, (unsigned)w);
        uint8_t* in = malloc(w);
        uint8_t* out = malloc(rs);
        CHECK(in != NULL && out != NULL, "out of memory");
        for (uint32_t x = 0; x < w; x++) {
            in[x] = p->inks[ru((uint32_t)p->ink_count)].code;
        }
        bool fits = true;
        for (size_t i = 0; i < p->ink_count; i++) {
            fits = fits && (p->inks[i].code >> bits) == 0;
        }
        if (!fits) {
            CHECK(epi_pack_row(p, format, in, w, out) == EPI_ERR_INVALID_ARG, "a panel too wide for the format passed");
            refused++;
        } else {
            CHECK(epi_pack_row(p, format, in, w, out) == EPI_OK, "pack failed at width %u", (unsigned)w);
            const unsigned mask = (1u << bits) - 1u, per = 8u / bits;
            for (uint32_t x = 0; x < w; x++) {
                const unsigned code = (out[x / per] >> (8u - bits - (x % per) * bits)) & mask;
                CHECK(code == in[x], "code %u at x=%u unpacked as %u", (unsigned)in[x], (unsigned)x, code);
            }
            /* EL133UF1's white is code 1; a grey panel lists its levels darkest first. */
            const unsigned pad = p == &epi_panel_el133uf1 ? 1u : p->inks[p->ink_count - 1].code;
            for (uint32_t x = w; x % per != 0; x++) {
                const unsigned code = (out[x / per] >> (8u - bits - (x % per) * bits)) & mask;
                CHECK(code == pad, "padding %u, want %u at width %u", code, pad, (unsigned)w);
            }
            /* A code too wide is refused, anywhere in the row. */
            in[ru(w)] = (uint8_t)((1u << bits) + ru(256u - (1u << bits)));
            CHECK(epi_pack_row(p, format, in, w, out) == EPI_ERR_INVALID_ARG, "a code too wide passed");
            wide++;
            ok++;
        }
        free(in);
        free(out);
    }
    printf("packer: ok=%ld refused=%ld too-wide=%ld\n", ok, refused, wide);
}

/* One channel of a random ink: near an end of the range, or anywhere in it. */
static uint8_t rchannel(uint32_t lo, uint32_t hi) {
    switch (ru(5)) {
    case 0:
        return (uint8_t)(lo + ru(3));
    case 1:
        return (uint8_t)(hi - ru(3));
    default:
        return (uint8_t)(lo + ru(hi - lo + 1u));
    }
}

/* A random ink set: a shipped one perturbed, near-neutral white and black among
 * saturated inks (with channels close to 0 and 255 included), near-corners of the
 * cube, a white ink of any tint (a channel of it may be 1) over darker inks in
 * each channel, or anything at all. */
static size_t rand_inks_any(epi_ink_t* inks) {
    const size_t n = 5 + ru(12);
    memset(inks, 0, 16 * sizeof inks[0]);
    for (size_t i = 0; i < n; i++) {
        inks[i].code = (uint8_t)rnd();
    }
    switch (ru(6)) {
    case 0: {
        const uint32_t amount = 1u + ru(40);
        for (size_t i = 0; i < 6; i++) {
            const epi_rgb_t m = epi_panel_el133uf1.inks[i].measured;
            const int d[3] = {
                (int)ru(2 * amount + 1u) - (int)amount,
                (int)ru(2 * amount + 1u) - (int)amount,
                (int)ru(2 * amount + 1u) - (int)amount,
            };
            const int v[3] = {m.r + d[0], m.g + d[1], m.b + d[2]};
            inks[i].measured.r = (uint8_t)(v[0] < 0 ? 0 : v[0] > 255 ? 255 : v[0]);
            inks[i].measured.g = (uint8_t)(v[1] < 0 ? 0 : v[1] > 255 ? 255 : v[1]);
            inks[i].measured.b = (uint8_t)(v[2] < 0 ? 0 : v[2] > 255 ? 255 : v[2]);
        }
        return 6;
    }
    case 1: {
        const uint32_t white = 150u + ru(106), black = ru(90);
        inks[0].measured =
            (epi_rgb_t){rchannel(white - 3u, white), rchannel(white - 3u, white), rchannel(white - 3u, white)};
        inks[1].measured =
            (epi_rgb_t){rchannel(black, black + 3u), rchannel(black, black + 3u), rchannel(black, black + 3u)};
        for (size_t i = 2; i < n; i++) {
            inks[i].measured = (epi_rgb_t){rchannel(0, white), rchannel(0, white), rchannel(0, white)};
        }
        return n;
    }
    case 2:
        for (size_t i = 0; i < n; i++) {
            const uint32_t corner = rnd();
            inks[i].measured.r = (uint8_t)(corner & 1 ? 255 - ru(3) : ru(3));
            inks[i].measured.g = (uint8_t)(corner & 2 ? 255 - ru(3) : ru(3));
            inks[i].measured.b = (uint8_t)(corner & 4 ? 255 - ru(3) : ru(3));
        }
        return n;
    case 4: {
        const uint32_t r = 1u + ru(ru(2) ? 255 : 8), g = 1u + ru(ru(2) ? 255 : 8), b = 1u + ru(ru(2) ? 255 : 8);
        inks[0].measured = (epi_rgb_t){(uint8_t)r, (uint8_t)g, (uint8_t)b};
        for (size_t i = 1; i < n; i++) {
            inks[i].measured = (epi_rgb_t){(uint8_t)ru(r + 1u), (uint8_t)ru(g + 1u), (uint8_t)ru(b + 1u)};
        }
        return n;
    }
    case 3: {
        const uint32_t range = 1u << (1 + ru(8));
        for (size_t i = 0; i < n; i++) {
            inks[i].measured = (epi_rgb_t){(uint8_t)ru(range), (uint8_t)ru(range), (uint8_t)ru(range)};
        }
        return n;
    }
    default:
        for (size_t i = 0; i < n; i++) {
            inks[i].measured = (epi_rgb_t){(uint8_t)rnd(), (uint8_t)rnd(), (uint8_t)rnd()};
        }
        return n;
    }
}

/* rand_inks_any, with the darkest ink set to exactly (0, 0, 0) in every eighth
 * set. The choice counts calls rather than drawing, so the other sets are the
 * ones rand_inks_any alone would give. */
static size_t rand_inks(epi_ink_t* inks) {
    static unsigned calls;
    const size_t n = rand_inks_any(inks);
    if (calls++ % 8 == 7) {
        size_t dark = 0;
        unsigned best = ~0u;
        for (size_t i = 0; i < n; i++) {
            const unsigned l = 2126u * inks[i].measured.r + 7152u * inks[i].measured.g + 722u * inks[i].measured.b;
            if (l < best) {
                best = l;
                dark = i;
            }
        }
        inks[dark].measured = (epi_rgb_t){0, 0, 0};
    }
    return n;
}

/* Checks what the progress callback is promised: done rises by 1 from 1, total is
 * constant, the ctx is the one given; returns false at cancel_at (0: never). */
typedef struct {
    unsigned calls;
    unsigned total;
    unsigned cancel_at;
    bool bad;
} progress_check_t;

static bool check_progress(void* ctx, unsigned done, unsigned total) {
    progress_check_t* pc = ctx;
    pc->bad = pc->bad || done != pc->calls + 1 || total == 0 || done > total || (pc->calls != 0 && total != pc->total);
    pc->total = total;
    pc->calls++;
    return done != pc->cancel_at;
}

static void fuzz_colour(int iters) {
    static const float sig[] = {FLT_TRUE_MIN, 0.01f, 0.3f, 1.f, 6.f, 10.5f};
    long ok = 0, rejected = 0, cancelled = 0, rows = 0, zero_black = 0;
    for (int it = 0; it < iters; it++) {
        epi_ink_t inks[16];
        epi_colour_panel_config_t g;
        memset(&g, 0, sizeof g);
        g.name = "c";
        g.width = 100;
        g.height = 100;
        g.inks = inks;
        g.ink_count = rand_inks(inks);
        g.detail_sigma = sig[ru(6)];
        progress_check_t pc = {0};
        g.progress = check_progress;
        g.progress_ctx = &pc;
        const size_t need = epi_colour_panel_size(&g);
        CHECK(need != 0, "a valid structure got size 0 [seed=%llu iteration=%d]", (unsigned long long)seed_arg, it);
        unsigned mis = ru(32);
        uint8_t* store = malloc(need + mis);
        CHECK(store != NULL, "out of memory");
        const epi_panel_t* p = NULL;
        const char* why = "stale";
        CHECK(
            epi_colour_panel_init(&g, store + mis, need - 1, &p, &why) == EPI_ERR_WORKSPACE_TOO_SMALL && p == NULL &&
                why == NULL,
            "init accepted storage one byte short"
        );
        CHECK(pc.calls == 0, "init reported progress with storage one byte short");
        /* Every third iteration first cancels an init, so that the real one reuses the
         * storage. The cancel point comes from the iteration, not the RNG, so the inks
         * and conversions drawn are the same as without it. */
        if (it % 3 == 0) {
            pc.cancel_at = 1 + (unsigned)it * 97u % 360u;
            const epi_err_t c = epi_colour_panel_init(&g, store + mis, need, &p, &why);
            CHECK(!pc.bad, "progress callback saw a wrong done, total or ctx");
            CHECK(
                c == EPI_ERR_CANCELLED ? pc.calls == pc.cancel_at && p == NULL && why == NULL
                                       : c == EPI_ERR_INVALID_ARG && pc.calls < pc.cancel_at,
                "cancel at %u gave %s after %u calls",
                pc.cancel_at,
                epi_strerror(c),
                pc.calls
            );
            cancelled += c == EPI_ERR_CANCELLED;
            pc = (progress_check_t){0};
        }
        const epi_err_t e = epi_colour_panel_init(&g, store + mis, need, &p, &why);
        CHECK(!pc.bad, "progress callback saw a wrong done, total or ctx");
        if (e != EPI_OK) {
            CHECK(
                e == EPI_ERR_INVALID_ARG && p == NULL && why != NULL && why[0] != '\0',
                "rejected inks: %s",
                epi_strerror(e)
            );
            free(store);
            rejected++;
            continue;
        }
        CHECK(pc.calls != 0 && pc.calls == pc.total, "built a panel without all of the progress");
        CHECK(why == NULL, "a built panel left a reason");
        ok++;
        for (size_t i = 0; i < g.ink_count; i++) {
            zero_black += inks[i].measured.r == 0 && inks[i].measured.g == 0 && inks[i].measured.b == 0;
        }
        for (int conv = 0; conv < 3; conv++) {
            epi_intent_t in = {rf01(), rf01(), rf01(), rf01() * 3.0f, rf01() * 3.0f};
            if (ru(2) == 0) {
                in = (epi_intent_t){(float)ru(2), (float)ru(2), (float)ru(2), (float)ru(4), (float)ru(4)};
            }
            epi_config_t c = {p, ru(4) ? &in : NULL, (uint16_t)(1 + ru(64)), (uint16_t)(1 + ru(40))};
            size_t cneed = epi_converter_workspace_size(&c);
            CHECK(
                cneed != 0,
                "the converter refused an accepted panel [seed=%llu iteration=%d]",
                (unsigned long long)seed_arg,
                it
            );
            uint8_t* raw = malloc(cneed);
            uint8_t* out = malloc(c.width);
            epi_rgb_t* row = malloc((size_t)c.width * sizeof *row);
            CHECK(raw != NULL && out != NULL && row != NULL, "out of memory");
            epi_converter_t* cv = NULL;
            CHECK(epi_converter_init(&c, raw, cneed, &cv) == EPI_OK, "converter init failed on an accepted panel");
            int mode = (int)ru(8);
            uint32_t got = 0;
            for (uint32_t y = 0; y < c.height; y++) {
                if (ru(4) == 0) {
                    mode = (int)ru(8);
                }
                fill(row, c.width, mode, y);
                CHECK(epi_converter_push(cv, row) == EPI_OK, "push of row %u failed", (unsigned)y);
                while (epi_converter_pop(cv, out) == EPI_OK) {
                    got++;
                    rows++;
                    for (uint32_t x = 0; x < c.width; x++) {
                        size_t i = 0;
                        while (i < p->ink_count && p->inks[i].code != out[x]) {
                            i++;
                        }
                        CHECK(i < p->ink_count, "code %u is not an ink", (unsigned)out[x]);
                    }
                }
            }
            CHECK(got == c.height, "converter emitted %u rows, want %u", (unsigned)got, (unsigned)c.height);
            free(row);
            free(out);
            free(raw);
        }
        free(store);
    }
    printf(
        "colour: accepted=%ld rejected=%ld cancelled=%ld rows=%ld zero-black=%ld\n",
        ok,
        rejected,
        cancelled,
        rows,
        zero_black
    );
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
    fuzz_preview(n / 3);
    fuzz_pack(n / 3);
    fuzz_colour(argc > 3 ? atoi(argv[3]) : n / 100);
    return 0;
}
