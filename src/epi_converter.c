/* Converter: rings, push/pop and output formats (docs/design.md, "Rendering intent", "Streaming and output").
 *
 * Everything lives in the caller's workspace, carved in this order (each
 * block rounded up to EPI_WS_ALIGN; the converter is placed at the first
 * aligned address, so the size includes EPI_WS_ALIGN − 1 bytes of slack):
 *
 *   struct epi_converter   state, incl. the intent and dither contexts
 *   err                    dither error row, epi_dither_err_size(tables, W)
 *   target                 one row of target linear sRGB, W × 3 int32
 *                          (grey: target Y, W int32)
 *   hblur ring             2r+1 rows of horizontally blurred L*src, int16
 *   scratch                one int16 row: L*src at push (colour), base at pop
 *   source ring            r+1 rows of source sRGB8 (S1 is recomputed)
 *                          (grey: r+1 rows of L*src, int16, kept from the push,
 *                          since S1 is all grey needs from the source)
 *   idx                    one row of palette indices, uint8
 *
 * Input row i goes to source slot i % (r+1) and hblur slot i % (2r+1).
 * Output row y needs source row y and hblur rows y−r..y+r, clamped to
 * [0, H−1] (the replicated edges are repeated row pointers), so it is ready
 * once min(y+r, H−1) has been pushed. A push is accepted only while
 * pushed − popped ≤ r: that keeps every row a pending output still needs in
 * both rings. Integer-only after init.
 */
#include "epaper_image.h"
#include "epi_dither.h"
#include "epi_intent.h"
#include "epi_math.h"
#include "epi_panel_tables.h"
#include "epi_srgb.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct epi_converter {
    const epi_panel_tables_t* tables;
    uint16_t width, height;
    epi_output_t output;
    uint32_t pushed; /* input rows accepted */
    uint32_t popped; /* output rows written */
    uint8_t pad_code;
    uint8_t code[EPI_MAX_INKS];
    uint8_t rgb[EPI_MAX_INKS][3]; /* preview colour per index */
    void* target;                 /* colour: int32_t[W][3]; grey: int32_t[W] */
    int16_t* hblur;
    int16_t* scratch;
    void* source; /* colour: epi_rgb_t rows; grey: int16_t rows of L*src */
    uint8_t* idx;
    epi_intent_ctx_t intent;
    epi_dither_ctx_t dither;
};

/* Tables as panelgen or epi_grey_panel_init builds them, so the intent and the
 * ditherer can rely on them.
 * The kind must be one of the two and is trusted to match the object: it
 * selects the struct, so a relabelled table cannot be told from a real one.
 * Grey: 2..16 levels, level_y non-negative, strictly increasing and ending at
 * exactly 65536, and thresholds between consecutive levels.
 * Colour: the hull's face count, the neutral interval, and white above black.
 * Both: a midtone curve with a monotone knee (Fritsch-Carlson). */
static bool tables_valid(const epi_panel_tables_t* base) {
    int32_t lmin;
    if (base->kind == EPI_PANEL_GREY) {
        const epi_grey_tables_t* t = epi_grey_tables(base);
        if (base->ink_count < 2 || base->ink_count > EPI_MAX_INKS || t->level_y[0] < 0 ||
            t->level_y[base->ink_count - 1] != EPI_Q16_ONE) {
            return false;
        }
        for (unsigned i = 0; i + 1 < base->ink_count; i++) {
            if (t->level_y[i + 1] <= t->level_y[i] || t->level_thr[i] < t->level_y[i] ||
                t->level_thr[i] >= t->level_y[i + 1]) {
                return false;
            }
        }
        lmin = epi_grey_lstar_q8(t->level_y[0]);
    } else if (base->kind == EPI_PANEL_COLOUR) {
        const epi_colour_tables_t* t = epi_colour_tables(base);
        if (t->hull_face_count > EPI_MAX_HULL_FACES || t->lstar_min <= 0 || t->lstar_min >= t->lstar_max ||
            t->lstar_max > 100 * 256 || t->black_xyz[1] < 0 || t->black_xyz[1] >= t->white_xyz[1]) {
            return false;
        }
        for (int i = 0; i < 3; i++) {
            /* white ≥ 513 keeps the reciprocal in int32. */
            if (t->white_xyz[i] <= 512) {
                return false;
            }
        }
        lmin = t->lstar_min;
    } else {
        return false;
    }
    return epi_midtone_monotone(lmin);
}

/* Bits per pixel of an output format (24 for the previews), 0 if invalid. */
static unsigned output_bits(epi_output_t o) {
    static const uint8_t bits[] = {
        [EPI_OUT_CODES8] = 8,
        [EPI_OUT_PACKED4] = 4,
        [EPI_OUT_PACKED2] = 2,
        [EPI_OUT_PACKED1] = 1,
        [EPI_OUT_PREVIEW_MEASURED] = 24,
        [EPI_OUT_PREVIEW_NOMINAL] = 24,
    };
    return (unsigned)o < sizeof bits ? bits[o] : 0u;
}

/* Structural validation shared by workspace_size and init. The intent's
 * ranges are checked by epi_intent_init. */
static bool valid_config(const epi_config_t* cfg) {
    if (cfg == NULL || cfg->panel == NULL || cfg->width < 1 || cfg->height < 1) {
        return false;
    }
    const epi_panel_t* p = cfg->panel;
    const epi_panel_tables_t* t = p->tables;
    if (t == NULL || !tables_valid(t) || p->inks == NULL || p->ink_count < 1 || p->ink_count > EPI_MAX_INKS ||
        p->ink_count != t->ink_count || t->kernel_radius > EPI_MAX_KERNEL_RADIUS) {
        return false;
    }
    if (cfg->intent == NULL && (p->presets == NULL || p->preset_count < 1 || p->presets[0].intent == NULL)) {
        return false;
    }
    const unsigned bits = output_bits(cfg->output);
    if (bits == 0) {
        return false;
    }
    const unsigned code_limit = bits >= 8 ? 256u : 1u << bits;
    for (size_t i = 0; i < p->ink_count; i++) {
        if (p->inks[i].code >= code_limit) {
            return false;
        }
    }
    return true;
}

size_t epi_output_row_size(epi_output_t output, uint16_t width) {
    return ((size_t)width * output_bits(output) + 7u) / 8u;
}

/* Block sizes, in carving order. */
typedef struct {
    size_t conv, err, target, hblur, scratch, source, idx;
} ws_layout_t;

static ws_layout_t layout(const epi_config_t* cfg) {
    const size_t w = cfg->width, r = cfg->panel->tables->kernel_radius;
    const bool grey = cfg->panel->tables->kind == EPI_PANEL_GREY;
    ws_layout_t l;
    l.conv = epi_align_up(sizeof(struct epi_converter));
    l.err = epi_align_up(epi_dither_err_size(cfg->panel->tables, cfg->width));
    l.target = epi_align_up(w * (grey ? 1 : 3) * sizeof(int32_t));
    l.hblur = epi_align_up((2 * r + 1) * w * sizeof(int16_t));
    l.scratch = epi_align_up(w * sizeof(int16_t));
    l.source = epi_align_up((r + 1) * w * (grey ? sizeof(int16_t) : sizeof(epi_rgb_t)));
    l.idx = epi_align_up(w);
    return l;
}

static size_t layout_size(const ws_layout_t* l) {
    return EPI_WS_ALIGN - 1 + l->conv + l->err + l->target + l->hblur + l->scratch + l->source + l->idx;
}

size_t epi_converter_workspace_size(const epi_config_t* cfg) {
    if (!valid_config(cfg)) {
        return 0;
    }
    ws_layout_t l = layout(cfg);
    return layout_size(&l);
}

/* Index of the ink with the largest measured luminance (Y of the measured
 * sRGB8, Q32); ties go to the lowest index. */
static size_t lightest_ink(const epi_panel_t* p) {
    size_t best = 0;
    int64_t best_y = INT64_MIN;
    for (size_t i = 0; i < p->ink_count; i++) {
        const epi_rgb_t m = p->inks[i].measured;
        int64_t y = epi_srgb_y_q32(m);
        if (y > best_y) {
            best_y = y;
            best = i;
        }
    }
    return best;
}

epi_err_t epi_converter_init(const epi_config_t* cfg, void* workspace, size_t size, epi_converter_t** out) {
    if (out != NULL) {
        *out = NULL;
    }
    if (workspace == NULL || out == NULL || !valid_config(cfg)) {
        return EPI_ERR_INVALID_ARG;
    }
    ws_layout_t l = layout(cfg);
    if (size < layout_size(&l)) {
        return EPI_ERR_WORKSPACE_TOO_SMALL;
    }
    const epi_panel_t* p = cfg->panel;
    const epi_intent_t* intent = cfg->intent != NULL ? cfg->intent : p->presets[0].intent;

    uint8_t* mem = epi_align_ptr(workspace, EPI_WS_ALIGN);
    struct epi_converter* c = (struct epi_converter*)(void*)mem;
    memset(c, 0, sizeof *c);

    epi_err_t e = epi_intent_init(&c->intent, p->tables, intent);
    if (e != EPI_OK) {
        return e;
    }

    mem += l.conv;
    void* err_mem = mem;
    mem += l.err;
    c->target = mem;
    mem += l.target;
    c->hblur = (int16_t*)(void*)mem;
    mem += l.hblur;
    c->scratch = (int16_t*)(void*)mem;
    mem += l.scratch;
    c->source = mem;
    mem += l.source;
    c->idx = mem;

    c->tables = p->tables;
    c->width = cfg->width;
    c->height = cfg->height;
    c->output = cfg->output;
    for (size_t i = 0; i < p->ink_count; i++) {
        const epi_ink_t* ink = &p->inks[i];
        const epi_rgb_t col = cfg->output == EPI_OUT_PREVIEW_NOMINAL ? ink->nominal : ink->measured;
        c->code[i] = ink->code;
        c->rgb[i][0] = col.r;
        c->rgb[i][1] = col.g;
        c->rgb[i][2] = col.b;
    }
    c->pad_code = p->inks[lightest_ink(p)].code;
    epi_dither_init(&c->dither, p->tables, cfg->width, err_mem);

    *out = c;
    return EPI_OK;
}

epi_err_t epi_converter_push(epi_converter_t* c, const epi_rgb_t* row) {
    if (c == NULL || row == NULL) {
        return EPI_ERR_INVALID_ARG;
    }
    if (c->pushed >= c->height) {
        return EPI_ERR_FINISHED;
    }
    const uint32_t r = c->tables->kernel_radius;
    /* Caller did not drain: accepting this row would overwrite one a
     * pending output row still needs. */
    if (c->pushed - c->popped > r) {
        assert(!"epi_converter_push: pop until not OK after every push");
        return EPI_ERR_NOT_DRAINED;
    }
    const size_t w = c->width;
    const uint32_t i = c->pushed;
    const size_t slot = (size_t)(i % (r + 1)) * w;
    const bool grey = c->tables->kind == EPI_PANEL_GREY;
    int16_t* lrow = grey ? (int16_t*)c->source + slot : c->scratch;
    if (!grey) {
        memcpy((epi_rgb_t*)c->source + slot, row, w * sizeof *row);
    }
    epi_intent_lstar_row(row, lrow, c->width);
    epi_blur_h(c->tables, lrow, c->hblur + (size_t)(i % (2 * r + 1)) * w, c->width);
    c->pushed = i + 1;
    return EPI_OK;
}

static void format_row(const epi_converter_t* c, uint8_t* out) {
    const uint8_t* idx = c->idx;
    const size_t w = c->width;
    size_t x;
    switch (c->output) {
    case EPI_OUT_CODES8:
        for (x = 0; x < w; x++) {
            out[x] = c->code[idx[x]];
        }
        break;
    case EPI_OUT_PACKED4:
    case EPI_OUT_PACKED2:
    case EPI_OUT_PACKED1: {
        const unsigned bits = output_bits(c->output);
        const unsigned per = 8u / bits;
        const size_t n = epi_output_row_size(c->output, c->width);
        for (size_t b = 0; b < n; b++) {
            unsigned v = 0;
            for (unsigned k = 0; k < per; k++) {
                x = b * per + k;
                v = (v << bits) | (x < w ? c->code[idx[x]] : c->pad_code);
            }
            out[b] = (uint8_t)v;
        }
        break;
    }
    case EPI_OUT_PREVIEW_MEASURED:
    case EPI_OUT_PREVIEW_NOMINAL:
        for (x = 0; x < w; x++) {
            memcpy(out + 3 * x, c->rgb[idx[x]], 3);
        }
        break;
    }
}

epi_err_t epi_converter_pop(epi_converter_t* c, void* out_row) {
    if (c == NULL || out_row == NULL) {
        return EPI_ERR_INVALID_ARG;
    }
    if (c->popped >= c->height) {
        return EPI_ERR_FINISHED;
    }
    const int32_t r = c->tables->kernel_radius, last = (int32_t)c->height - 1;
    const int32_t y = (int32_t)c->popped;
    const int32_t need = y + r < last ? y + r : last;
    if ((int32_t)c->pushed <= need) {
        return EPI_ERR_NOT_READY;
    }
    const size_t w = c->width;
    const uint32_t hslots = (uint32_t)(2 * r + 1);
    const int16_t* rows[2 * EPI_MAX_KERNEL_RADIUS + 1];
    for (int32_t j = 0; j <= 2 * r; j++) {
        int32_t yy = y + j - r;
        yy = yy < 0 ? 0 : (yy > last ? last : yy);
        rows[j] = c->hblur + (size_t)((uint32_t)yy % hslots) * w;
    }
    epi_blur_v(c->tables, rows, c->scratch, c->width);
    const size_t slot = (size_t)(c->popped % (uint32_t)(r + 1)) * w;
    if (c->tables->kind == EPI_PANEL_GREY) {
        epi_intent_grey_row(&c->intent, (int16_t*)c->source + slot, c->scratch, c->target, c->width);
        epi_dither_grey_row(&c->dither, c->target, c->idx);
    } else {
        epi_intent_row(&c->intent, (epi_rgb_t*)c->source + slot, c->scratch, c->target, c->width);
        epi_dither_row(&c->dither, c->target, c->idx);
    }
    format_row(c, (uint8_t*)out_row);
    c->popped++;
    return EPI_OK;
}
