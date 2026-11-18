/*
 * SHA-256 and the panel ID (src/epi_sha256.c, src/epi_panel_id.c; docs/design.md,
 * "Panel ID"). The shipped panels' IDs are checked against init beside their
 * tables (test_colour_panel_el133uf1, test_grey_panel_builtin).
 */
#include "epaper_image.h"
#include "epi_panel_id.h"
#include "epi_panel_tables.h"
#include "epi_sha256.h"
#include "test_util.h"
#include "unity.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void assert_digest(const char* want_hex, const uint8_t* data, size_t len) {
    uint8_t d[EPI_SHA256_LEN];
    epi_sha256(data, len, d);
    char got[2 * EPI_SHA256_LEN + 1];
    for (size_t i = 0; i < EPI_SHA256_LEN; i++) {
        snprintf(got + 2 * i, 3, "%02x", d[i]);
    }
    TEST_ASSERT_EQUAL_STRING(want_hex, got);
}

/* FIPS 180-4 examples, and lengths either side of the padding boundary. */
void test_sha256_vectors(void) {
    uint8_t* a = xmalloc(1000);
    memset(a, 'a', 1000);
    assert_digest("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", NULL, 0);
    assert_digest("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", (const uint8_t*)"abc", 3);
    static const char m448[] = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    assert_digest("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", (const uint8_t*)m448, 56);
    static const char m896[] = "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
                               "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
    assert_digest("cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1", (const uint8_t*)m896, 112);
    assert_digest("9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318", a, 55);
    assert_digest("ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb", a, 64);
    assert_digest("41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3", a, 1000);
    free(a);
}

#define LV(code, v)                                                                                                    \
    {                                                                                                                  \
        (code), {(v), (v), (v)}, {                                                                                     \
            (v), (v), (v)                                                                                              \
        }                                                                                                              \
    }

static const epi_ink_t levels4[4] = {LV(0, 20), LV(1, 90), LV(2, 150), LV(3, 200)};

static const epi_neutral_knot_t knots2[2] = {{40.0f, 1.0f, -2.0f}, {80.0f, 0.5f, -1.0f}};

/* The inputs of one ID, varied one at a time below. */
typedef struct {
    uint8_t kind;
    uint16_t width, height;
    epi_ink_t inks[4];
    size_t ink_count;
    float sigma;
    epi_neutral_knot_t knots[2];
    size_t knot_count;
} id_inputs_t;

static id_inputs_t base_inputs(void) {
    id_inputs_t in = {
        .kind = EPI_PANEL_COLOUR, .width = 640, .height = 480, .ink_count = 4, .sigma = 5.0f, .knot_count = 2
    };
    memcpy(in.inks, levels4, sizeof levels4);
    memcpy(in.knots, knots2, sizeof knots2);
    return in;
}

static void id_of(const id_inputs_t* in, uint8_t id[EPI_PANEL_ID_LEN]) {
    epi_panel_id(id, in->kind, in->width, in->height, in->inks, in->ink_count, in->sigma, in->knots, in->knot_count);
}

static void assert_id(bool same, const id_inputs_t* in, const char* what) {
    uint8_t want[EPI_PANEL_ID_LEN], got[EPI_PANEL_ID_LEN];
    const id_inputs_t base = base_inputs();
    id_of(&base, want);
    id_of(in, got);
    TEST_ASSERT_TRUE_MESSAGE((memcmp(want, got, sizeof got) == 0) == same, what);
}

/* What the ID covers, and what it leaves out. */
void test_panel_id_inputs(void) {
    id_inputs_t in;
    in = base_inputs();
    assert_id(true, &in, "same inputs");
    in.kind = EPI_PANEL_GREY;
    assert_id(false, &in, "kind");
    in = base_inputs();
    in.width = 641;
    assert_id(false, &in, "width");
    in = base_inputs();
    in.height = 481;
    assert_id(false, &in, "height");
    in = base_inputs();
    in.inks[2].code = 7;
    assert_id(false, &in, "code");
    for (int c = 0; c < 3; c++) {
        in = base_inputs();
        uint8_t* m = c == 0 ? &in.inks[1].measured.r : c == 1 ? &in.inks[1].measured.g : &in.inks[1].measured.b;
        *m ^= 1;
        assert_id(false, &in, "a measured channel");
    }
    in = base_inputs();
    in.ink_count = 3;
    assert_id(false, &in, "ink count");
    in = base_inputs();
    in.sigma = 5.0001f;
    assert_id(false, &in, "sigma");
    in = base_inputs();
    in.knots[1].lstar = 81.0f;
    assert_id(false, &in, "knot L*");
    in = base_inputs();
    in.knots[0].a = 1.5f;
    assert_id(false, &in, "knot a*");
    in = base_inputs();
    in.knots[1].b = -1.5f;
    assert_id(false, &in, "knot b*");
    in = base_inputs();
    in.knot_count = 1;
    assert_id(false, &in, "knot count");

    in = base_inputs();
    in.inks[0].nominal = (epi_rgb_t){1, 2, 3};
    in.inks[3].nominal = (epi_rgb_t){255, 255, 255};
    assert_id(true, &in, "nominal is not covered");

    /* -0 and +0 are the same knot value. */
    id_inputs_t pos = base_inputs(), neg = base_inputs();
    pos.knots[0] = (epi_neutral_knot_t){0.0f, 0.0f, 0.0f};
    neg.knots[0] = (epi_neutral_knot_t){-0.0f, -0.0f, -0.0f};
    uint8_t a[EPI_PANEL_ID_LEN], b[EPI_PANEL_ID_LEN];
    id_of(&pos, a);
    id_of(&neg, b);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(a, b, sizeof a);
}

typedef union {
    long long ll;
    long double ld;
    void* p;
    unsigned char bytes[3072];
} storage_t;

/* One storage for the test, on the heap (the ESP32 test app has no DRAM to
 * spare for .bss): two grey panels in its halves, then one colour panel in the
 * whole. */
static storage_t* st;
#define HALF (sizeof st->bytes / 2)

static const epi_panel_t* grey(size_t at, const epi_grey_panel_config_t* cfg) {
    const epi_panel_t* p = NULL;
    TEST_ASSERT_EQUAL_INT(EPI_OK, epi_grey_panel_init(cfg, st->bytes + at, HALF - 1, &p, NULL));
    return p;
}

/* Init's ID is the ID of the inputs it uses; the name is not part of it. */
void test_panel_id_init(void) {
    static const uint8_t zero[EPI_PANEL_ID_LEN] = {0};
    uint8_t want[EPI_PANEL_ID_LEN];
    st = xmalloc(sizeof *st);

    epi_grey_panel_config_t gc = {"A", 800, 480, levels4, 4, 6.0f};
    const epi_panel_t* p = grey(1, &gc);
    epi_panel_id(want, EPI_PANEL_GREY, 800, 480, levels4, 4, 6.0f, NULL, 0);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, p->id, sizeof want);
    TEST_ASSERT_FALSE(memcmp(zero, p->id, sizeof zero) == 0);

    gc.name = "B";
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, grey(HALF, &gc)->id, sizeof want);
    /* Re-init from the panel's own levels, in the same storage. */
    gc.levels = p->inks;
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, grey(1, &gc)->id, sizeof want);

    /* A colour panel from the EL133UF1's inks, without its size or knots. */
    const epi_panel_t* el = &epi_panel_el133uf1;
    epi_colour_panel_config_t cc = {"C", 640, 480, el->inks, el->ink_count, el->detail_sigma, NULL, NULL, knots2, 2};
    const epi_panel_t* q = NULL;
    TEST_ASSERT_EQUAL_INT(EPI_OK, epi_colour_panel_init(&cc, st->bytes, sizeof st->bytes, &q, NULL));
    epi_panel_id(want, EPI_PANEL_COLOUR, 640, 480, el->inks, el->ink_count, el->detail_sigma, knots2, 2);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, q->id, sizeof want);
    TEST_ASSERT_FALSE(memcmp(el->id, q->id, sizeof want) == 0);
    free(st);
    st = NULL;
}
