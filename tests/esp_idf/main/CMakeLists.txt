# The test sources are the host suite's own files, compiled in place.
set(T "${CMAKE_CURRENT_LIST_DIR}/../..")
idf_component_register(
    SRCS "app_main.c"
         "${T}/test_list.c"
         "${T}/test_strerror.c"
         "${T}/test_math.c"
         "${T}/test_dither.c"
         "${T}/test_intent.c"
         "${T}/test_converter.c"
         "${T}/test_grey.c"
         "${T}/test_resampler.c"
         "${T}/test_preview.c"
    PRIV_INCLUDE_DIRS "${T}" "${T}/../src"
    PRIV_REQUIRES epaper-image unity esp_timer
)
# Same FP flags as the host test build. The 2^31-iteration exhaustive cbrt
# test is skipped (it would take hours emulated).
target_compile_options(${COMPONENT_LIB} PRIVATE -ffp-contract=off)
target_compile_definitions(${COMPONENT_LIB} PRIVATE EPI_TEST_EXHAUSTIVE=0)
