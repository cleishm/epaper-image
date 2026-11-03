#include "unity.h"

#include <epaper_image.h>

void test_strerror(void) {
    static const struct {
        epi_err_t err;
        const char* msg;
    } cases[] = {
        {EPI_OK, "success"},
        {EPI_ERR_INVALID_ARG, "invalid argument"},
        {EPI_ERR_WORKSPACE_TOO_SMALL, "workspace too small"},
        {EPI_ERR_NOT_READY, "no output row ready"},
        {EPI_ERR_NOT_DRAINED, "earlier rows not drained"},
        {EPI_ERR_FINISHED, "all rows already processed"},
        {EPI_ERR_SOURCE_TOO_LARGE, "source too large"},
        {(epi_err_t)-1, "unknown error"},
        {(epi_err_t)1000, "unknown error"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const char* msg = epi_strerror(cases[i].err);
        TEST_ASSERT_NOT_NULL(msg);
        TEST_ASSERT_EQUAL_STRING(cases[i].msg, msg);
    }
}
