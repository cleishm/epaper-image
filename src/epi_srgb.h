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

#endif /* EPI_SRGB_H */
