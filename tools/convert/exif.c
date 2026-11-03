#include "exif.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static uint16_t rd16(const uint8_t* p, bool le) {
    return le ? (uint16_t)(p[0] | p[1] << 8) : (uint16_t)(p[0] << 8 | p[1]);
}

static uint32_t rd32(const uint8_t* p, bool le) {
    return le ? (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24
              : (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

/* Orientation from a TIFF block (the APP1 payload after "Exif\0\0"). */
static int tiff_orientation(const uint8_t* t, size_t n) {
    if (n < 8) {
        return 1;
    }
    bool le;
    if (t[0] == 'I' && t[1] == 'I') {
        le = true;
    } else if (t[0] == 'M' && t[1] == 'M') {
        le = false;
    } else {
        return 1;
    }
    if (rd16(t + 2, le) != 42) {
        return 1;
    }
    const uint32_t ifd = rd32(t + 4, le); /* IFD0 */
    if (ifd > n - 2) {
        return 1;
    }
    const uint16_t count = rd16(t + ifd, le);
    for (uint32_t i = 0; i < count; i++) {
        const size_t e = (size_t)ifd + 2 + (size_t)i * 12;
        if (e + 12 > n) {
            return 1;
        }
        if (rd16(t + e, le) == 0x0112) { /* Orientation: SHORT, count 1, value inline */
            if (rd16(t + e + 2, le) != 3 || rd32(t + e + 4, le) != 1) {
                return 1;
            }
            const uint16_t v = rd16(t + e + 8, le);
            return v >= 1 && v <= 8 ? v : 1;
        }
    }
    return 1;
}

int exif_orientation(const uint8_t* d, size_t n) {
    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) {
        return 1; /* not a JPEG (SOI) */
    }
    size_t p = 2;
    while (p + 4 <= n) {
        if (d[p] != 0xFF) {
            return 1;
        }
        const uint8_t marker = d[p + 1];
        if (marker == 0xFF) { /* fill byte */
            p++;
            continue;
        }
        if (marker == 0xDA || marker == 0xD9) {
            return 1; /* scan data or EOI: no EXIF before it */
        }
        const size_t len = rd16(d + p + 2, false); /* includes the two length bytes */
        if (len < 2 || p + 2 + len > n) {
            return 1;
        }
        const uint8_t* seg = d + p + 4;
        const size_t seg_n = len - 2;
        if (marker == 0xE1 && seg_n >= 6 && memcmp(seg, "Exif\0\0", 6) == 0) {
            return tiff_orientation(seg + 6, seg_n - 6);
        }
        p += 2 + len;
    }
    return 1;
}

uint8_t* exif_apply(const uint8_t* src, int w, int h, int channels, int orientation, int* ow, int* oh) {
    const bool swap = orientation >= 5 && orientation <= 8;
    *ow = swap ? h : w;
    *oh = swap ? w : h;
    const size_t px = (size_t)channels;
    uint8_t* out = malloc((size_t)w * (size_t)h * px);
    if (!out) {
        return NULL;
    }
    for (int y = 0; y < *oh; y++) {
        for (int x = 0; x < *ow; x++) {
            int sx, sy; /* the stored pixel shown at (x, y) */
            switch (orientation) {
            case 2: /* mirrored */
                sx = w - 1 - x;
                sy = y;
                break;
            case 3: /* rotated 180 */
                sx = w - 1 - x;
                sy = h - 1 - y;
                break;
            case 4: /* flipped */
                sx = x;
                sy = h - 1 - y;
                break;
            case 5: /* transposed */
                sx = y;
                sy = x;
                break;
            case 6: /* turn 90 clockwise to view */
                sx = y;
                sy = h - 1 - x;
                break;
            case 7: /* transversed */
                sx = w - 1 - y;
                sy = h - 1 - x;
                break;
            case 8: /* turn 90 anticlockwise to view */
                sx = w - 1 - y;
                sy = x;
                break;
            default:
                sx = x;
                sy = y;
                break;
            }
            memcpy(
                out + ((size_t)y * (size_t)*ow + (size_t)x) * px, src + ((size_t)sy * (size_t)w + (size_t)sx) * px, px
            );
        }
    }
    return out;
}
