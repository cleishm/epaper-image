/* The test list shared by every runner (tests/test_runner.c on the host,
 * tests/esp_idf/main/app_main.c on ESP-IDF). Test functions live in test_*.c. */
#include "test_list.h"

#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

#define EPI_TEST(name) void name(void);
#include "test_list.inc"
#undef EPI_TEST

int epi_run_all_tests(void) {
    UNITY_BEGIN();
#define EPI_TEST(name) RUN_TEST(name);
#include "test_list.inc"
#undef EPI_TEST
    return UNITY_END();
}
