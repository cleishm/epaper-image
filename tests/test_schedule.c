/*
 * The output of a stage does not depend on when its rows are pushed and
 * popped: the resampler, the converter (with either pop) and the preview
 * reducer give the same bytes for every schedule they accept. The packer
 * works row by row with no state, so it needs no schedule test.
 *
 * Schedules, each run on a stage with the same input:
 * - eager: pop until not OK after every push (the reference);
 * - lazy: pop until not OK before every push (so before the first, which
 *   releases the resampler's background rows) and after the last; every
 *   push finds the stage drained, so it is valid in every build;
 * - greedy: push until EPI_ERR_NOT_DRAINED, drain, push the refused row again;
 * - random: seeded bursts of pushes and pops, refused pushes retried after
 *   draining.
 * A refused push changes nothing, so the last two run only where asserts are
 * off (a refused push asserts in debug builds).
 */
#include "epaper_image.h"
#include "test_util.h"
#include "unity.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef epi_err_t (*push_fn)(void* stage, const void* row);
typedef epi_err_t (*pop_fn)(void* stage, void* row);

static epi_err_t resampler_push(void* s, const void* r) {
    return epi_resampler_push(s, r);
}
static epi_err_t resampler_pop(void* s, void* r) {
    return epi_resampler_pop(s, r);
}
static epi_err_t converter_push(void* s, const void* r) {
    return epi_converter_push(s, r);
}
static epi_err_t converter_pop(void* s, void* r) {
    return epi_converter_pop(s, r);
}
static epi_err_t converter_pop_undithered(void* s, void* r) {
    return epi_converter_pop_undithered(s, r);
}
static epi_err_t preview_push(void* s, const void* r) {
    return epi_preview_push(s, r);
}
static epi_err_t preview_pop(void* s, void* r) {
    return epi_preview_pop(s, r);
}

typedef enum { EAGER, LAZY, GREEDY, RANDOM } schedule_t;

typedef struct {
    void* state;
    push_fn push;
    pop_fn pop;
    const uint8_t* in; /* in_n rows of in_row bytes */
    size_t in_row;
    uint32_t in_n;
    uint8_t* out; /* out_n rows of out_row bytes */
    size_t out_row;
    uint32_t out_n;
} stage_t;

static uint32_t pop_some(const stage_t* s, uint32_t popped, uint32_t most) {
    uint32_t n = 0;
    while (n < most && popped + n < s->out_n &&
           s->pop(s->state, s->out + (size_t)(popped + n) * s->out_row) == EPI_OK) {
        n++;
    }
    return n;
}

/* Runs the schedule to the end; every row must come out. The stage's state
 * is already initialised. */
static void run_schedule(const stage_t* s, schedule_t how, uint32_t* seed) {
    uint32_t pushed = 0, popped = 0;
    while (pushed < s->in_n) {
        if (how == LAZY) {
            popped += pop_some(s, popped, UINT32_MAX);
        }
        uint32_t k = how == EAGER || how == LAZY ? 1 : how == GREEDY ? s->in_n : xorshift32(seed) % 6;
        bool refused = false;
        for (; k > 0 && pushed < s->in_n; k--) {
            epi_err_t e = s->push(s->state, s->in + (size_t)pushed * s->in_row);
            if (e == EPI_ERR_NOT_DRAINED) {
                TEST_ASSERT_TRUE_MESSAGE(how == GREEDY || how == RANDOM, "refused");
                refused = true;
                break;
            }
            TEST_ASSERT_EQUAL_INT(EPI_OK, e);
            pushed++;
        }
        /* A random schedule sometimes stops popping early, unless refused. */
        uint32_t most = how == RANDOM && !refused ? xorshift32(seed) % 4 : UINT32_MAX;
        uint32_t got = pop_some(s, popped, most);
        popped += got;
        /* A refused row must be accepted after draining, so a drain that
         * frees nothing means the stage is stuck. */
        if (refused) {
            TEST_ASSERT_TRUE_MESSAGE(got > 0, "stuck");
        }
    }
    popped += pop_some(s, popped, UINT32_MAX);
    TEST_ASSERT_EQUAL_UINT32(s->out_n, popped);
    /* No extra row: the stage is finished. */
    TEST_ASSERT_EQUAL_INT(EPI_ERR_FINISHED, s->pop(s->state, s->out));
}

#ifdef NDEBUG
#define SCHEDULES 4
#else
#define SCHEDULES 2
#endif

typedef void (*init_fn)(const void* ctx, stage_t* s);

/* Runs every schedule of a stage (init starts it afresh) and checks each
 * output against the eager one. ref and got hold out_n rows. */
static void check_stage(init_fn init, const void* ctx, stage_t* s, uint8_t* ref, uint8_t* got) {
    uint32_t seed = 12345u;
    const size_t bytes = (size_t)s->out_n * s->out_row;
    for (int how = EAGER; how < SCHEDULES; how++) {
        for (int i = 0; i < (how == RANDOM ? 4 : 1); i++) { /* random: several seeds */
            init(ctx, s);
            s->out = how == EAGER ? ref : got;
            memset(s->out, 0xee, bytes);
            run_schedule(s, (schedule_t)how, &seed);
            if (how != EAGER) {
                TEST_ASSERT_EQUAL_MEMORY(ref, got, bytes);
            }
        }
    }
}

/* ---- resampler ------------------------------------------------------------ */

typedef struct {
    const epi_resampler_config_t* cfg;
    void* ws;
    size_t size;
} resampler_ctx_t;

static void resampler_init(const void* ctx, stage_t* s) {
    const resampler_ctx_t* c = ctx;
    epi_resampler_t* r = NULL;
    TEST_ASSERT_EQUAL_INT(EPI_OK, epi_resampler_init(c->cfg, c->ws, c->size, &r));
    s->state = r;
}

static void resampler_case(uint8_t which, uint32_t sw, uint32_t sh, uint16_t dw, uint16_t dh, epi_filter_t filter) {
    epi_resampler_config_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.src_width = sw;
    cfg.src_height = sh;
    cfg.dst_width = dw;
    cfg.dst_height = dh;
    cfg.filter = filter;
    cfg.background = (epi_rgb_t){10, 200, 30};
    TEST_ASSERT_EQUAL_INT(EPI_OK, layout(which, &cfg, NULL));
    resampler_ctx_t ctx = {&cfg, NULL, epi_resampler_workspace_size(&cfg)};
    TEST_ASSERT_NOT_EQUAL(0, ctx.size);
    ctx.ws = xmalloc(ctx.size);
    epi_rgb_t* in = xmalloc((size_t)sw * sh * sizeof *in);
    fill_noise(in, (size_t)sw * sh, sw * 31u + sh);
    const size_t out_row = (size_t)dw * sizeof(epi_rgb_t);
    uint8_t* ref = xmalloc(out_row * dh);
    uint8_t* got = xmalloc(out_row * dh);
    stage_t s = {
        .push = resampler_push,
        .pop = resampler_pop,
        .in = (const uint8_t*)in,
        .in_row = (size_t)sw * sizeof *in,
        .in_n = sh,
        .out_row = out_row,
        .out_n = dh
    };
    check_stage(resampler_init, &ctx, &s, ref, got);
    free(got);
    free(ref);
    free(in);
    free(ctx.ws);
}

void test_schedule_resampler(void) {
    resampler_case(1, 120, 80, 40, 27, EPI_FILTER_LANCZOS3); /* shrink */
    resampler_case(1, 120, 80, 40, 27, EPI_FILTER_BOX);
    resampler_case(1, 12, 8, 36, 28, EPI_FILTER_BICUBIC);    /* upscale */
    resampler_case(2, 100, 30, 40, 40, EPI_FILTER_BILINEAR); /* bands above and below */
    resampler_case(2, 3, 400, 6, 12, EPI_FILTER_LANCZOS3);   /* tall: height > 100 × width */
    resampler_case(1, 3, 400, 4, 1, EPI_FILTER_LANCZOS3);    /* one output row */
}

/* ---- converter, then the preview ------------------------------- */

typedef struct {
    const epi_config_t* cfg;
    void* ws;
    size_t size;
    const epi_preview_config_t* pcfg;
    void* pws;
    size_t psize;
} pipeline_ctx_t;

static void converter_init(const void* ctx, stage_t* s) {
    const pipeline_ctx_t* c = ctx;
    epi_converter_t* cv = NULL;
    TEST_ASSERT_EQUAL_INT(EPI_OK, epi_converter_init(c->cfg, c->ws, c->size, &cv));
    s->state = cv;
}

static void preview_init(const void* ctx, stage_t* s) {
    const pipeline_ctx_t* c = ctx;
    epi_preview_t* p = NULL;
    TEST_ASSERT_EQUAL_INT(EPI_OK, epi_preview_init(c->pcfg, c->pws, c->psize, &p));
    s->state = p;
}

static void pipeline_case(const epi_panel_t* panel, const epi_intent_t* intent, int w, int h) {
    epi_config_t cfg = config(panel, (uint16_t)w, (uint16_t)h);
    cfg.intent = intent;
    epi_preview_config_t pcfg;
    memset(&pcfg, 0, sizeof pcfg);
    pcfg.panel = panel;
    pcfg.src_width = (uint16_t)w;
    pcfg.src_height = (uint16_t)h;
    pcfg.dst_width = (uint16_t)(w / 3 + 1);
    pcfg.dst_height = (uint16_t)(h / 4 + 1);
    pipeline_ctx_t ctx = {
        &cfg, NULL, epi_converter_workspace_size(&cfg), &pcfg, NULL, epi_preview_workspace_size(&pcfg)
    };
    TEST_ASSERT_NOT_EQUAL(0, ctx.size);
    TEST_ASSERT_NOT_EQUAL(0, ctx.psize);
    ctx.ws = xmalloc(ctx.size);
    ctx.pws = xmalloc(ctx.psize);

    const size_t n = (size_t)w * h;
    epi_rgb_t* in = xmalloc(n * sizeof *in);
    epi_rgb_t* noise = xmalloc(n * sizeof *noise);
    fill_gradient(in, w, h);
    fill_noise(noise, n, 99);
    for (size_t i = 0; i < n; i += 3) { /* a gradient with noisy pixels */
        in[i] = noise[i];
    }
    free(noise);
    uint8_t* ref = xmalloc(n);
    uint8_t* got = xmalloc(n);
    stage_t s = {
        .push = converter_push,
        .pop = converter_pop,
        .in = (const uint8_t*)in,
        .in_row = (size_t)w * sizeof *in,
        .in_n = (uint32_t)h,
        .out_row = (size_t)w,
        .out_n = (uint32_t)h
    };
    check_stage(converter_init, &ctx, &s, ref, got);

    /* The undithered rows of the same input. */
    const size_t urow = (size_t)w * sizeof(epi_rgb_t);
    uint8_t* uref = xmalloc(urow * h);
    uint8_t* ugot = xmalloc(urow * h);
    stage_t u = s;
    u.pop = converter_pop_undithered;
    u.out_row = urow;
    check_stage(converter_init, &ctx, &u, uref, ugot);
    free(ugot);
    free(uref);

    /* The preview over the reference codes. */
    const size_t prow = (size_t)pcfg.dst_width * sizeof(epi_rgb_t);
    uint8_t* vref = xmalloc(prow * pcfg.dst_height);
    uint8_t* vgot = xmalloc(prow * pcfg.dst_height);
    stage_t v = {
        .push = preview_push,
        .pop = preview_pop,
        .in = ref,
        .in_row = (size_t)w,
        .in_n = (uint32_t)h,
        .out_row = prow,
        .out_n = pcfg.dst_height
    };
    check_stage(preview_init, &ctx, &v, vref, vgot);

    free(vgot);
    free(vref);
    free(got);
    free(ref);
    free(in);
    free(ctx.pws);
    free(ctx.ws);
}

void test_schedule_pipeline(void) {
    const epi_panel_t* const panels[] = {&epi_panel_el133uf1, &epi_panel_gdey075t7_grey4, &epi_panel_gdey075t7_mono};
    for (size_t k = 0; k < sizeof panels / sizeof panels[0]; k++) {
        const epi_panel_t* p = panels[k];
        TEST_ASSERT_TRUE(p->preset_count > 0);
        pipeline_case(p, epi_panel_preset(p, NULL), 40, 80);
        epi_intent_t extreme = *epi_panel_preset(p, NULL);
        extreme.adapt = 1.0f;
        extreme.vibrance = 2.0f;
        extreme.lift = 20.0f;
        extreme.veil = 3.0f;
        pipeline_case(p, &extreme, 40, 80);
        pipeline_case(p, epi_panel_preset(p, NULL), 5, 3); /* shorter than the blur's radius */
    }
}
