/*
 * The panel ID's canonical serialisation (src/epi_panel_id.h; docs/design.md,
 * "Panel ID"), hashed with SHA-256. Every integer is written by shifts, so the
 * bytes do not depend on the target's byte order or struct layout.
 */
#include "epi_panel_id.h"

#include "epi_panel_tables.h"
#include "epi_sha256.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

_Static_assert(sizeof(((epi_panel_t*)0)->id) == EPI_PANEL_ID_LEN, "epi_panel_t.id holds EPI_PANEL_ID_LEN bytes");

#define ID_TAG "epi-panel-id-v1"
#define ID_TAG_LEN (sizeof ID_TAG - 1)
/* Tag, kind, size, ink count and inks, sigma, knot count and knots. */
#define ID_MAX_LEN (ID_TAG_LEN + 1 + 4 + 1 + 4 * EPI_MAX_INKS + 4 + 1 + 12 * EPI_MAX_NEUTRAL_KNOTS)

static uint8_t* put_u16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    return p + 2;
}

static uint8_t* put_u32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
    return p + 4;
}

static uint32_t float_bits(float f) {
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    return u;
}

/* -0 as +0: the two compare equal, so they are the same knot. */
static uint8_t* put_knot_value(uint8_t* p, float f) {
    const uint32_t u = float_bits(f);
    return put_u32(p, u == 0x80000000u ? 0u : u);
}

void epi_panel_id(
    uint8_t id[EPI_PANEL_ID_LEN],
    uint8_t kind,
    uint16_t width,
    uint16_t height,
    const epi_ink_t* inks,
    size_t ink_count,
    float detail_sigma,
    const epi_neutral_knot_t* knots,
    size_t knot_count
) {
    uint8_t buf[ID_MAX_LEN];
    uint8_t* p = buf;
    memcpy(p, ID_TAG, ID_TAG_LEN);
    p += ID_TAG_LEN;
    *p++ = kind == EPI_PANEL_GREY ? 1 : 2;
    p = put_u16(p, width);
    p = put_u16(p, height);
    *p++ = (uint8_t)ink_count;
    for (size_t i = 0; i < ink_count; i++) {
        *p++ = inks[i].code;
        *p++ = inks[i].measured.r;
        *p++ = inks[i].measured.g;
        *p++ = inks[i].measured.b;
    }
    p = put_u32(p, float_bits(detail_sigma));
    *p++ = (uint8_t)knot_count;
    for (size_t i = 0; i < knot_count; i++) {
        p = put_knot_value(p, knots[i].lstar);
        p = put_knot_value(p, knots[i].a);
        p = put_knot_value(p, knots[i].b);
    }
    uint8_t digest[EPI_SHA256_LEN];
    epi_sha256(buf, (size_t)(p - buf), digest);
    memcpy(id, digest, EPI_PANEL_ID_LEN);
}
