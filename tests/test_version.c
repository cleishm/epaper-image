#include "unity.h"

#include <epaper_image.h>

void test_version(void) {
    const uint32_t want =
        (uint32_t)EPI_VERSION_MAJOR << 16 | (uint32_t)EPI_VERSION_MINOR << 8 | (uint32_t)EPI_VERSION_PATCH;
    TEST_ASSERT_EQUAL_HEX32(want, epi_version());
}
