/* The stb_image (decode) and stb_image_write (preview PNG) implementations,
 * kept apart from convert.c so the warning flags apply to convert.c only. */
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
