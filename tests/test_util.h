/* Helpers shared by the Unity test files. Header-only (static inline), so the
 * test source lists need no entry for it. */
#ifndef EPI_TEST_UTIL_H
#define EPI_TEST_UTIL_H

#include "epaper_image.h"
#include "epi_panel_tables.h"
#include "unity.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(epi_rgb_t) == 3, "fnv1a64 over pixels hashes r, g, b with no padding");

#define TEST_ASSERT_EQUAL_RGB(want, got)                                                                               \
    do {                                                                                                               \
        const epi_rgb_t want_ = (want), got_ = (got);                                                                  \
        TEST_ASSERT_EQUAL_UINT8(want_.r, got_.r);                                                                      \
        TEST_ASSERT_EQUAL_UINT8(want_.g, got_.g);                                                                      \
        TEST_ASSERT_EQUAL_UINT8(want_.b, got_.b);                                                                      \
    } while (0)

static inline void* xmalloc(size_t n) {
    void* p = malloc(n ? n : 1);
    TEST_ASSERT_NOT_NULL(p);
    return p;
}

static inline uint32_t xorshift32(uint32_t* s) {
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *s = x;
}

static inline uint64_t xorshift64(uint64_t* s) {
    *s ^= *s << 13;
    *s ^= *s >> 7;
    *s ^= *s << 17;
    return *s;
}

static inline float bits_f(uint32_t u) {
    float f;
    memcpy(&f, &u, sizeof f);
    return f;
}

static inline uint32_t f_bits(float f) {
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    return u;
}

static inline uint64_t fnv1a64(const uint8_t* p, size_t n) {
    uint64_t h = UINT64_C(0xcbf29ce484222325);
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= UINT64_C(0x100000001b3);
    }
    return h;
}

/* ---- deterministic inputs ------------------------------------------------ */

static inline void fill_noise(epi_rgb_t* img, size_t n, uint32_t seed) {
    uint32_t s = seed ? seed : 1u;
    for (size_t i = 0; i < n; i++) {
        uint32_t v = xorshift32(&s);
        img[i].r = (uint8_t)v;
        img[i].g = (uint8_t)(v >> 8);
        img[i].b = (uint8_t)(v >> 16);
    }
}

static inline void fill_gradient(epi_rgb_t* img, int w, int h) {
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            epi_rgb_t* p = &img[(size_t)y * w + x];
            p->r = (uint8_t)(w > 1 ? x * 255 / (w - 1) : 0);
            p->g = (uint8_t)(h > 1 ? y * 255 / (h - 1) : 0);
            p->b = (uint8_t)(255 - (w + h > 2 ? (x + y) * 255 / (w + h - 2) : 0));
        }
    }
}

/* Fully saturated hue sweep along x (6 sectors), value falling along y. */
static inline void fill_hue_ramp(epi_rgb_t* img, int w, int h) {
    for (int y = 0; y < h; y++) {
        int v = 255 - (h > 1 ? y * 200 / (h - 1) : 0);
        for (int x = 0; x < w; x++) {
            int hh = w > 1 ? x * 1530 / (w - 1) : 0; /* 0..1530 = 6 × 255 */
            int sector = hh / 255 < 6 ? hh / 255 : 5, f = hh - sector * 255;
            int c[3];
            switch (sector) {
            case 0:
                c[0] = 255;
                c[1] = f;
                c[2] = 0;
                break;
            case 1:
                c[0] = 255 - f;
                c[1] = 255;
                c[2] = 0;
                break;
            case 2:
                c[0] = 0;
                c[1] = 255;
                c[2] = f;
                break;
            case 3:
                c[0] = 0;
                c[1] = 255 - f;
                c[2] = 255;
                break;
            case 4:
                c[0] = f;
                c[1] = 0;
                c[2] = 255;
                break;
            default:
                c[0] = 255;
                c[1] = 0;
                c[2] = 255 - f;
                break;
            }
            epi_rgb_t* p = &img[(size_t)y * w + x];
            p->r = (uint8_t)(c[0] * v / 255);
            p->g = (uint8_t)(c[1] * v / 255);
            p->b = (uint8_t)(c[2] * v / 255);
        }
    }
}

/* ---- converter drivers -------------------------------------------------------- */

static inline epi_config_t config(const epi_panel_t* p, uint16_t w, uint16_t h, epi_output_t out) {
    epi_config_t c;
    memset(&c, 0, sizeof c);
    c.panel = p;
    c.intent = NULL;
    c.width = w;
    c.height = h;
    c.output = out;
    return c;
}

/* Streams img through a converter in ws (size bytes), popping until not OK
 * after every push and checking the lag (rows out = max(0, y − r + 1), all
 * after the last push). out receives H rows of epi_output_row_size bytes.
 * first_ready (optional) is the input row after whose push the first output
 * row appeared. Returns the number of rows popped. */
static inline int
run_converter_in(const epi_config_t* cfg, const epi_rgb_t* img, void* ws, size_t size, uint8_t* out, int* first_ready) {
    epi_converter_t* c = NULL;
    TEST_ASSERT_EQUAL_INT(EPI_OK, epi_converter_init(cfg, ws, size, &c));
    TEST_ASSERT_NOT_NULL(c);
    const size_t rs = epi_output_row_size(cfg->output, cfg->width);
    const int r = cfg->panel->tables->kernel_radius, h = cfg->height;
    int n = 0;
    if (first_ready != NULL) {
        *first_ready = -1;
    }
    uint8_t* spare = xmalloc(rs);
    TEST_ASSERT_EQUAL_INT(EPI_ERR_NOT_READY, epi_converter_pop(c, spare)); /* before any push */
    for (int y = 0; y < h; y++) {
        TEST_ASSERT_EQUAL_INT(EPI_OK, epi_converter_push(c, img + (size_t)y * cfg->width));
        while (n < h && epi_converter_pop(c, out + (size_t)n * rs) == EPI_OK) {
            if (n == 0 && first_ready != NULL) {
                *first_ready = y;
            }
            n++;
        }
        int expect = y == h - 1 ? h : (y >= r ? y - r + 1 : 0);
        TEST_ASSERT_EQUAL_INT(expect, n);
        if (n < h) {
            TEST_ASSERT_EQUAL_INT(EPI_ERR_NOT_READY, epi_converter_pop(c, spare));
        }
    }
    /* Nothing more: further pops and pushes are finished. */
    TEST_ASSERT_EQUAL_INT(EPI_ERR_FINISHED, epi_converter_pop(c, spare));
    TEST_ASSERT_EQUAL_INT(EPI_ERR_FINISHED, epi_converter_push(c, img));
    TEST_ASSERT_EQUAL_INT(EPI_ERR_FINISHED, epi_converter_pop(c, spare));
    free(spare);
    return n;
}

static inline int run_converter(const epi_config_t* cfg, const epi_rgb_t* img, uint8_t* out, int* first_ready) {
    size_t size = epi_converter_workspace_size(cfg);
    TEST_ASSERT_NOT_EQUAL(0, size);
    void* ws = xmalloc(size);
    int n = run_converter_in(cfg, img, ws, size, out, first_ready);
    free(ws);
    return n;
}

/* A CODES8 run in a zeroed workspace is the reference; runs in workspaces
 * filled with other bytes must reproduce it. a and b are width × height. */
static inline void
check_dirty_workspace(const epi_config_t* cfg, const epi_rgb_t* img, void* ws, size_t size, uint8_t* a, uint8_t* b) {
    const size_t n = (size_t)cfg->width * cfg->height;
    memset(ws, 0x00, size);
    run_converter_in(cfg, img, ws, size, a, NULL);
    const uint8_t fills[] = {0xff, 0xa5, 0x5a, 0x80};
    for (size_t i = 0; i < sizeof fills; i++) {
        memset(ws, fills[i], size);
        run_converter_in(cfg, img, ws, size, b, NULL);
        TEST_ASSERT_EQUAL_MEMORY(a, b, n);
    }
}

/* CODES8 output → ink index, by code (the test panels' codes are distinct). */
static inline int code_to_index(const epi_panel_t* p, uint8_t code) {
    size_t i = 0;
    while (i < p->ink_count && p->inks[i].code != code) {
        i++;
    }
    if (i == p->ink_count) {
        TEST_FAIL_MESSAGE("unknown code");
    }
    return (int)i;
}

/* Rows y − r .. y + r of the horizontally blurred image hb (w × h), edge rows
 * replicated by clamping, for epi_blur_v in the whole-image references. */
static inline void clamp_rows(const int16_t* hb, int w, int h, int y, int r, const int16_t** rows) {
    for (int j = 0; j <= 2 * r; j++) {
        int yy = y + j - r;
        yy = yy < 0 ? 0 : (yy > h - 1 ? h - 1 : yy);
        rows[j] = hb + (size_t)yy * w;
    }
}

#endif
