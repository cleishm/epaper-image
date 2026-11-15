/*
 * Panel-independent sRGB constants (docs/design.md, "Colour model"). Defined in the
 * generated src/epi_srgb.c (tools/panelgen).
 */
#ifndef EPI_SRGB_H
#define EPI_SRGB_H

#include "epaper_image.h"

#include <stdint.h>

/* sRGB EOTF: linear light of the 8-bit code u, Q16 (u = 255 -> 65536). */
extern const uint32_t epi_srgb_to_lin_q16[256];

/* Linear sRGB -> CIE XYZ (D65, white Y = 1), the 4-decimal IEC matrix, Q16. */
extern const int32_t epi_srgb_to_xyz_q16[3][3];

/* CIE Y of an sRGB8 colour, Q32 (exact: Q16 matrix row × Q16 lin). */
static inline int64_t epi_srgb_y_q32(epi_rgb_t m) {
    return (int64_t)epi_srgb_to_xyz_q16[1][0] * epi_srgb_to_lin_q16[m.r] +
        (int64_t)epi_srgb_to_xyz_q16[1][1] * epi_srgb_to_lin_q16[m.g] +
        (int64_t)epi_srgb_to_xyz_q16[1][2] * epi_srgb_to_lin_q16[m.b];
}

/* The sRGB8 code whose linear value is nearest to lin (Q16, at most 65536), by
 * binary search in epi_srgb_to_lin_q16. An exact midpoint between two codes
 * goes to the lower one. */
static inline uint8_t epi_srgb8_from_lin_q16(uint32_t lin) {
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

#endif /* EPI_SRGB_H */
