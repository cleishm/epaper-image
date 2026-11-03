/*
 * EXIF orientation for convert: stb_image decodes pixels as stored, so a
 * photo the camera tagged "rotate to display" would come out sideways.
 */
#ifndef CONVERT_EXIF_H
#define CONVERT_EXIF_H

#include <stddef.h>
#include <stdint.h>

/*
 * The EXIF Orientation tag (1..8) of a JPEG held in data[0..size), or 1
 * (as stored) when the data is not a JPEG, has no EXIF block, or the tag
 * is missing or malformed.
 */
int exif_orientation(const uint8_t* data, size_t size);

/*
 * Reorient a w x h image of `channels` bytes per pixel so it displays
 * upright for EXIF orientation `orientation`. Returns a new malloc'd buffer
 * (the caller frees it) and sets *ow, *oh (swapped for orientations 5..8),
 * or NULL when out of memory. Orientation 1 (or anything outside 2..8)
 * copies the image unchanged.
 */
uint8_t* exif_apply(const uint8_t* src, int w, int h, int channels, int orientation, int* ow, int* oh);

#endif /* CONVERT_EXIF_H */
