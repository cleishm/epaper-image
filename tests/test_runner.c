/* Host test runner. The test list is in test_list.c and the test functions
 * in test_*.c, so other runners (ESP-IDF test app, Emscripten/Node) reuse them. */
#include "test_list.h"

int main(void) {
    return epi_run_all_tests();
}
