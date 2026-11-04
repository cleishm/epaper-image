/* Preview reducer and row packer (docs/design.md, "Preview reduction").
 *
 * The reducer lives in the caller's workspace, carved in this order (each block
 * rounded up to EPI_WS_ALIGN; the state is placed at the first aligned address,
 * so the size includes EPI_WS_ALIGN − 1 bytes of slack):
 *
 *   struct epi_preview     state, incl. the code table
 *   acc                    the dst row being summed, dst_width × 3 uint64
 *   row                    the finished dst row, dst_width × 3 bytes
 *
 * On an axis, source pixel i covers [i·dst, (i+1)·dst) and dst pixel j covers
 * [j·src, (j+1)·src), so a source pixel overlaps at most two dst pixels and the
 * overlaps of one dst pixel sum to src. Each acc entry sums
 * linear × xoverlap × yoverlap, at most 2^16 × 65535², and its mean divides by
 * src_width × src_height. Integer-only throughout.
 */
#include "epaper_image.h"
#include "epi_math.h"
#include "epi_srgb.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct epi_preview {
    uint32_t src_width, src_height;
    uint32_t dst_width, dst_height;
    uint32_t pushed;  /* source rows accepted */
    uint32_t filling; /* dst row being summed; ahead of popped while a finished row waits */
    uint32_t popped;  /* dst rows written */
    uint64_t* acc;
    uint8_t* row;
    uint32_t lin[256][3]; /* each ink's linear colour, Q16, by code */
    uint8_t valid[256];   /* whether the code is one of the panel's inks */
};

static bool valid_panel(const epi_panel_t* p) {
    return p != NULL && p->inks != NULL && p->ink_count >= 1;
}

static bool valid_config(const epi_preview_config_t* cfg) {
    return cfg != NULL && valid_panel(cfg->panel) && cfg->src_width >= 1 && cfg->src_height >= 1 &&
        cfg->dst_width >= 1 && cfg->dst_height >= 1 && cfg->dst_width <= cfg->src_width &&
        cfg->dst_height <= cfg->src_height &&
        (cfg->output == EPI_OUT_PREVIEW_MEASURED || cfg->output == EPI_OUT_PREVIEW_NOMINAL);
}

size_t epi_preview_workspace_size(const epi_preview_config_t* cfg) {
    if (!valid_config(cfg)) {
        return 0;
    }
    const size_t w = cfg->dst_width;
    return EPI_WS_ALIGN - 1 + epi_align_up(sizeof(struct epi_preview)) + epi_align_up(w * 3 * sizeof(uint64_t)) +
        epi_align_up(w * 3);
}

epi_err_t epi_preview_init(const epi_preview_config_t* cfg, void* workspace, size_t size, epi_preview_t** out) {
    if (out != NULL) {
        *out = NULL;
    }
    if (workspace == NULL || out == NULL || !valid_config(cfg)) {
        return EPI_ERR_INVALID_ARG;
    }
    if (size < epi_preview_workspace_size(cfg)) {
        return EPI_ERR_WORKSPACE_TOO_SMALL;
    }

    uint8_t* mem = epi_align_ptr(workspace, EPI_WS_ALIGN);
    struct epi_preview* p = (struct epi_preview*)(void*)mem;
    memset(p, 0, sizeof *p);
    mem += epi_align_up(sizeof *p);
    p->acc = (uint64_t*)(void*)mem;
    memset(p->acc, 0, (size_t)cfg->dst_width * 3 * sizeof(uint64_t));
    mem += epi_align_up((size_t)cfg->dst_width * 3 * sizeof(uint64_t));
    p->row = mem;

    p->src_width = cfg->src_width;
    p->src_height = cfg->src_height;
    p->dst_width = cfg->dst_width;
    p->dst_height = cfg->dst_height;
    for (size_t i = 0; i < cfg->panel->ink_count; i++) {
        const epi_ink_t* ink = &cfg->panel->inks[i];
        const epi_rgb_t c = cfg->output == EPI_OUT_PREVIEW_NOMINAL ? ink->nominal : ink->measured;
        p->lin[ink->code][0] = epi_srgb_to_lin_q16[c.r];
        p->lin[ink->code][1] = epi_srgb_to_lin_q16[c.g];
        p->lin[ink->code][2] = epi_srgb_to_lin_q16[c.b];
        p->valid[ink->code] = 1;
    }

    *out = p;
    return EPI_OK;
}

/* Add a source row to the dst row being summed, with yover its vertical overlap. */
static void accumulate(epi_preview_t* p, const uint8_t* codes, uint32_t yover) {
    const uint32_t src = p->src_width, dst = p->dst_width;
    uint64_t* acc = p->acc;
    uint32_t left = 0, end = src; /* this source pixel's left edge; the dst pixel's right edge */
    for (uint32_t x = 0; x < src; x++) {
        const uint32_t* lin = p->lin[codes[x]];
        const uint32_t right = left + dst;
        const uint32_t xover = (right < end ? right : end) - left;
        const uint64_t w = (uint64_t)xover * yover;
        acc[0] += lin[0] * w;
        acc[1] += lin[1] * w;
        acc[2] += lin[2] * w;
        if (right >= end) {
            acc += 3;
            if (right > end) {
                const uint64_t rest = (uint64_t)(right - end) * yover;
                acc[0] += lin[0] * rest;
                acc[1] += lin[1] * rest;
                acc[2] += lin[2] * rest;
            }
            end += src;
        }
        left = right;
    }
}

/* The sRGB8 value whose linear value is nearest to lin (at most 65536). An exact
 * midpoint between two values goes to the lower one. */
static uint8_t nearest_srgb8(uint32_t lin) {
    unsigned lo = 0, hi = 255; /* the first entry not below lin is in [lo, hi] */
    while (lo < hi) {
        const unsigned mid = (lo + hi) / 2;
        if (epi_srgb_to_lin_q16[mid] < lin) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo > 0 && lin - epi_srgb_to_lin_q16[lo - 1] <= epi_srgb_to_lin_q16[lo] - lin) {
        lo--;
    }
    return (uint8_t)lo;
}

/* Turn the sums into the finished row and clear them for the next. A mean exactly
 * halfway between two integers rounds up. */
static void finish_row(epi_preview_t* p) {
    const uint64_t total = (uint64_t)p->src_width * p->src_height;
    const size_t n = (size_t)p->dst_width * 3;
    for (size_t i = 0; i < n; i++) {
        p->row[i] = nearest_srgb8((uint32_t)((p->acc[i] + total / 2) / total));
        p->acc[i] = 0;
    }
    p->filling++;
}

epi_err_t epi_preview_push(epi_preview_t* p, const uint8_t* codes) {
    if (p == NULL || codes == NULL) {
        return EPI_ERR_INVALID_ARG;
    }
    if (p->pushed >= p->src_height) {
        return EPI_ERR_FINISHED;
    }
    /* Caller did not drain: accepting this row could overwrite the finished row. */
    if (p->filling != p->popped) {
        assert(!"epi_preview_push: pop until not OK after every push");
        return EPI_ERR_NOT_DRAINED;
    }
    for (uint32_t x = 0; x < p->src_width; x++) {
        if (!p->valid[codes[x]]) {
            return EPI_ERR_INVALID_ARG;
        }
    }
    const uint32_t top = p->pushed * p->dst_height;
    const uint32_t bottom = top + p->dst_height;
    const uint32_t end = (p->filling + 1) * p->src_height;
    if (bottom < end) {
        accumulate(p, codes, p->dst_height);
    } else {
        accumulate(p, codes, end - top);
        finish_row(p);
        if (bottom > end) {
            accumulate(p, codes, bottom - end);
        }
    }
    p->pushed++;
    return EPI_OK;
}

epi_err_t epi_preview_pop(epi_preview_t* p, epi_rgb_t* dst_row) {
    if (p == NULL || dst_row == NULL) {
        return EPI_ERR_INVALID_ARG;
    }
    if (p->popped >= p->dst_height) {
        return EPI_ERR_FINISHED;
    }
    if (p->filling == p->popped) {
        return EPI_ERR_NOT_READY;
    }
    memcpy(dst_row, p->row, (size_t)p->dst_width * 3);
    p->popped++;
    return EPI_OK;
}

/* The code of the ink with the largest measured luminance, as the converter
 * pads; ties go to the lowest index. */
static uint8_t lightest_code(const epi_panel_t* p) {
    size_t best = 0;
    int64_t best_y = INT64_MIN;
    for (size_t i = 0; i < p->ink_count; i++) {
        const int64_t y = epi_srgb_y_q32(p->inks[i].measured);
        if (y > best_y) {
            best_y = y;
            best = i;
        }
    }
    return p->inks[best].code;
}

epi_err_t
epi_pack_row(const epi_panel_t* panel, epi_output_t output, const uint8_t* codes, uint16_t width, void* out_row) {
    unsigned bits;
    switch (output) {
    case EPI_OUT_PACKED4:
        bits = 4;
        break;
    case EPI_OUT_PACKED2:
        bits = 2;
        break;
    case EPI_OUT_PACKED1:
        bits = 1;
        break;
    default:
        return EPI_ERR_INVALID_ARG;
    }
    if (!valid_panel(panel) || codes == NULL || width < 1 || out_row == NULL) {
        return EPI_ERR_INVALID_ARG;
    }
    const unsigned limit = 1u << bits;
    for (size_t i = 0; i < panel->ink_count; i++) {
        if (panel->inks[i].code >= limit) {
            return EPI_ERR_INVALID_ARG;
        }
    }
    for (size_t x = 0; x < width; x++) {
        if (codes[x] >= limit) {
            return EPI_ERR_INVALID_ARG;
        }
    }
    const uint8_t pad = lightest_code(panel);
    const unsigned per = 8u / bits;
    const size_t n = epi_output_row_size(output, width);
    uint8_t* out = (uint8_t*)out_row;
    for (size_t b = 0; b < n; b++) {
        unsigned v = 0;
        for (unsigned k = 0; k < per; k++) {
            const size_t x = b * per + k;
            v = (v << bits) | (x < width ? codes[x] : pad);
        }
        out[b] = (uint8_t)v;
    }
    return EPI_OK;
}
