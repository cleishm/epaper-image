/* ctest convert_exif: the EXIF Orientation parser and the reorientation. */
#include "exif.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            fprintf(stderr, "%s:%d: FAIL %s\n", __FILE__, __LINE__, #cond);                                            \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

/* SOI, an APP0, an APP1 EXIF block whose IFD0 holds a Make entry and
 * then `tag` (Orientation when 0x0112) with value v, then SOS. */
static size_t make_jpeg(uint8_t* b, int le, uint16_t tag, uint16_t v) {
    size_t n = 0;
#define PUT(x) (b[n++] = (uint8_t)(x))
#define PUT16(x) (le ? (PUT((x) & 0xFF), PUT((x) >> 8)) : (PUT((x) >> 8), PUT((x) & 0xFF)))
#define PUT32(x) (le ? (PUT16((x) & 0xFFFF), PUT16((x) >> 16)) : (PUT16((x) >> 16), PUT16((x) & 0xFFFF)))
    PUT(0xFF), PUT(0xD8);
    PUT(0xFF), PUT(0xE0), PUT(0), PUT(4), PUT('x'), PUT('y'); /* APP0, 2 payload bytes */
    const size_t app1 = n;
    PUT(0xFF), PUT(0xE1), PUT(0), PUT(0); /* length patched below */
    memcpy(b + n, "Exif\0\0", 6);
    n += 6;
    PUT(le ? 'I' : 'M'), PUT(le ? 'I' : 'M');
    PUT16(42);
    PUT32(8);                                    /* IFD0 right after the header */
    PUT16(2);                                    /* two entries */
    PUT16(0x010F), PUT16(2), PUT32(4), PUT32(0); /* Make (ASCII), ignored */
    PUT16(tag), PUT16(3), PUT32(1), PUT16(v), PUT16(0);
    PUT32(0); /* no next IFD */
    const size_t len = n - app1 - 2;
    b[app1 + 2] = (uint8_t)(len >> 8);
    b[app1 + 3] = (uint8_t)len;
    PUT(0xFF), PUT(0xDA), PUT(0), PUT(2);
    return n;
#undef PUT
#undef PUT16
#undef PUT32
}

int main(void) {
    uint8_t b[256];
    for (int le = 0; le <= 1; le++) {
        for (uint16_t v = 1; v <= 8; v++) {
            CHECK(exif_orientation(b, make_jpeg(b, le, 0x0112, v)) == v);
        }
        CHECK(exif_orientation(b, make_jpeg(b, le, 0x0112, 9)) == 1); /* out of range */
        CHECK(exif_orientation(b, make_jpeg(b, le, 0x0110, 6)) == 1); /* no Orientation tag */
        const size_t n = make_jpeg(b, le, 0x0112, 6);
        CHECK(exif_orientation(b, n - 20) == 1); /* truncated inside the block */
    }
    static const uint8_t png[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    CHECK(exif_orientation(png, sizeof png) == 1);

    /* A 3x2 image, one byte per pixel:  1 2 3 / 4 5 6. Each orientation's
     * upright result, row by row. */
    static const uint8_t img[6] = {1, 2, 3, 4, 5, 6};
    static const struct {
        int w, h;
        uint8_t px[6];
    } want[9] = {
        {0, 0, {0}},
        {3, 2, {1, 2, 3, 4, 5, 6}}, /* 1 as stored */
        {3, 2, {3, 2, 1, 6, 5, 4}}, /* 2 mirrored */
        {3, 2, {6, 5, 4, 3, 2, 1}}, /* 3 rotated 180 */
        {3, 2, {4, 5, 6, 1, 2, 3}}, /* 4 flipped */
        {2, 3, {1, 4, 2, 5, 3, 6}}, /* 5 transposed */
        {2, 3, {4, 1, 5, 2, 6, 3}}, /* 6 turned clockwise */
        {2, 3, {6, 3, 5, 2, 4, 1}}, /* 7 transversed */
        {2, 3, {3, 6, 2, 5, 1, 4}}, /* 8 turned anticlockwise */
    };
    for (int o = 1; o <= 8; o++) {
        int ow, oh;
        uint8_t* out = exif_apply(img, 3, 2, 1, o, &ow, &oh);
        CHECK(out != NULL);
        if (!out) {
            continue;
        }
        CHECK(ow == want[o].w && oh == want[o].h);
        if (memcmp(out, want[o].px, 6) != 0) {
            fprintf(
                stderr, "orientation %d: got %u %u %u %u %u %u\n", o, out[0], out[1], out[2], out[3], out[4], out[5]
            );
            failures++;
        }
        free(out);
    }
    if (failures == 0) {
        puts("convert_exif: OK");
    }
    return failures != 0;
}
