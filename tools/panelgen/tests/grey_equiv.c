/*
 * Equivalence gate for generated grey calibrations (tests/test_grey.py).
 *
 * Linked with the library and a generated src/panels/<glass>.c. The test
 * writes grey_equiv_cases.h: the levels straight from the JSON and the
 * generated panels to compare. For each case this builds the same panel at
 * runtime with epi_grey_panel_init and requires every field of the tables,
 * the levels and the panel fields to be identical and the presets to be the
 * same, then runs
 * both panels through the converter and the preview reducer and requires
 * byte-identical output.
 * Prints one line per case; exit status is the number of mismatches.
 */
#include "epaper_image.h"
#include "epi_panel_tables.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct grey_case {
    const char* ident;
    const epi_panel_t* generated;
    const epi_ink_t* levels;
    size_t level_count;
};

/* GREY_NAME, GREY_WIDTH, GREY_HEIGHT, GREY_SIGMA, the externs, cases[]. */
#include "grey_equiv_cases.h"

static int fails;

static void fail(const char* ident, const char* what) {
    printf("MISMATCH %s: %s\n", ident, what);
    fails++;
}

#define FIELD(a, b, f)                                                                                                 \
    do {                                                                                                               \
        nfields++;                                                                                                     \
        if (memcmp(&(a)->f, &(b)->f, sizeof(a)->f) != 0)                                                               \
            fail(id, #f);                                                                                              \
    } while (0)

static int compare_tables(const char* id, const epi_panel_tables_t* a, const epi_panel_tables_t* b) {
    int nfields = 0;
    FIELD(a, b, kind);
    FIELD(a, b, ink_count);
    FIELD(a, b, kernel_radius);
    FIELD(a, b, kernel);
    const epi_grey_tables_t* ga = epi_grey_tables(a);
    const epi_grey_tables_t* gb = epi_grey_tables(b);
    FIELD(ga, gb, level_y);
    FIELD(ga, gb, level_thr);
    return nfields;
}

/* A deterministic test image: ramps, a checker and pseudo-random noise. */
static void image_row(uint16_t w, uint16_t y, epi_rgb_t* row) {
    uint32_t s = 0x9E3779B9u * (y + 1u);
    for (uint16_t x = 0; x < w; x++) {
        s = s * 1664525u + 1013904223u;
        uint8_t v;
        if (y < 16) {
            v = (uint8_t)(x * 255u / (w - 1u));
        } else if (y < 32) {
            v = ((x / 4 + y / 4) & 1) ? 230 : 25;
        } else {
            v = (uint8_t)(s >> 24);
        }
        row[x].r = v;
        row[x].g = (uint8_t)(v ^ (uint8_t)(x & 7));
        row[x].b = (uint8_t)(255 - v / 2);
    }
}

/* Converts the test image; returns a malloc'd buffer of h rows of ink codes. */
static uint8_t* convert(const epi_panel_t* p, const epi_intent_t* intent, uint16_t w, uint16_t h) {
    epi_config_t cfg = {p, intent, w, h};
    size_t ws = epi_converter_workspace_size(&cfg);
    size_t rs = w;
    void* work = ws ? malloc(ws) : NULL;
    uint8_t* res = calloc(h, rs);
    epi_rgb_t* row = malloc(w * sizeof *row);
    epi_converter_t* c = NULL;
    if (work == NULL || res == NULL || row == NULL || epi_converter_init(&cfg, work, ws, &c) != EPI_OK) {
        free(work);
        free(res);
        free(row);
        return NULL;
    }
    size_t got = 0;
    for (uint16_t y = 0; y < h; y++) {
        image_row(w, y, row);
        epi_converter_push(c, row);
        while (got < h && epi_converter_pop(c, res + got * rs) == EPI_OK) {
            got++;
        }
    }
    free(work);
    free(row);
    return got == h ? res : (free(res), NULL);
}

/* The codes' measured preview at their own size; a malloc'd buffer of h rows. */
static uint8_t* preview(const epi_panel_t* p, const uint8_t* codes, uint16_t w, uint16_t h) {
    epi_preview_config_t cfg = {.panel = p, .src_width = w, .src_height = h, .dst_width = w, .dst_height = h};
    size_t ws = epi_preview_workspace_size(&cfg);
    void* work = ws ? malloc(ws) : NULL;
    epi_rgb_t* res = calloc(h, w * sizeof *res);
    epi_preview_t* r = NULL;
    if (work == NULL || res == NULL || epi_preview_init(&cfg, work, ws, &r) != EPI_OK) {
        free(work);
        free(res);
        return NULL;
    }
    size_t got = 0;
    for (uint16_t y = 0; y < h; y++) {
        epi_preview_push(r, codes + (size_t)y * w);
        while (got < h && epi_preview_pop(r, res + got * w) == EPI_OK) {
            got++;
        }
    }
    free(work);
    return got == h ? (uint8_t*)res : (free(res), NULL);
}

static uint64_t fnv1a(const uint8_t* p, size_t n) {
    uint64_t hsh = 1469598103934665603u;
    for (size_t i = 0; i < n; i++) {
        hsh = (hsh ^ p[i]) * 1099511628211u;
    }
    return hsh;
}

int main(void) {
    for (size_t k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        const struct grey_case* cs = &cases[k];
        const char* id = cs->ident;
        const int before = fails;
        epi_grey_panel_config_t cfg = {GREY_NAME, GREY_WIDTH, GREY_HEIGHT, cs->levels, cs->level_count, GREY_SIGMA};
        size_t size = epi_grey_panel_size(&cfg);
        void* storage = size ? malloc(size) : NULL;
        const epi_panel_t* rt = NULL;
        if (storage == NULL || epi_grey_panel_init(&cfg, storage, size, &rt, NULL) != EPI_OK) {
            fail(id, "epi_grey_panel_init rejected the levels");
            free(storage);
            continue;
        }
        const epi_panel_t* g = cs->generated;

        int nfields = compare_tables(id, g->tables, rt->tables);

        if (strcmp(g->name, rt->name) != 0) {
            fail(id, "name");
        }
        if (g->width != rt->width || g->height != rt->height) {
            fail(id, "width/height");
        }
        if (memcmp(&g->detail_sigma, &rt->detail_sigma, sizeof g->detail_sigma) != 0) {
            fail(id, "detail_sigma");
        }
        if (g->ink_count != rt->ink_count) {
            fail(id, "ink_count");
        } else {
            for (size_t i = 0; i < g->ink_count; i++) {
                const epi_ink_t *x = &g->inks[i], *y = &rt->inks[i];
                if (x->code != y->code || memcmp(&x->nominal, &y->nominal, sizeof x->nominal) != 0 ||
                    memcmp(&x->measured, &y->measured, sizeof x->measured) != 0) {
                    fail(id, "inks");
                }
            }
        }
        if (g->presets != rt->presets || g->preset_count != rt->preset_count) {
            fail(id, "presets");
        }

        /* Both panels through the converter, every preset, codes and preview. */
        uint64_t hsh = 1469598103934665603u;
        for (size_t i = 0; i < g->preset_count && i < rt->preset_count; i++) {
            const size_t codes_bytes = (size_t)97 * 61, preview_bytes = codes_bytes * sizeof(epi_rgb_t);
            uint8_t* a = convert(g, g->presets[i].intent, 97, 61);
            uint8_t* b = convert(rt, rt->presets[i].intent, 97, 61);
            if (a == NULL || b == NULL) {
                fail(id, "converter failed");
            } else if (memcmp(a, b, codes_bytes) != 0) {
                fail(id, "converter output");
            } else {
                hsh ^= fnv1a(a, codes_bytes) + i * 31;
            }
            uint8_t* pa = a != NULL ? preview(g, a, 97, 61) : NULL;
            uint8_t* pb = b != NULL ? preview(rt, b, 97, 61) : NULL;
            if (pa == NULL || pb == NULL) {
                fail(id, "preview failed");
            } else if (memcmp(pa, pb, preview_bytes) != 0) {
                fail(id, "preview output");
            } else {
                hsh ^= fnv1a(pa, preview_bytes) + i * 31 + 1;
            }
            free(pa);
            free(pb);
            free(a);
            free(b);
        }
        printf(
            "%s %s: %d table fields, %zu levels, %zu presets; whole-struct memcmp %s; output 0x%016llx\n",
            fails == before ? "OK" : "FAIL",
            id,
            nfields,
            g->ink_count,
            g->preset_count,
            memcmp(g->tables, rt->tables, sizeof(epi_grey_tables_t)) == 0 ? "equal" : "differs (padding?)",
            (unsigned long long)hsh
        );
        free(storage);
    }
    return fails;
}
