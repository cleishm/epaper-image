# The tests are C (Unity); only the C++ header check needs the CXX language.
enable_language(CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

# DOWNLOAD_EXTRACT_TIMESTAMP is a CMake 3.24 keyword (silences policy CMP0135).
set(epi_fetch_timestamp "")
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.24)
    set(epi_fetch_timestamp DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
endif()

FetchContent_Declare(
    unity
    URL https://github.com/ThrowTheSwitch/Unity/archive/refs/tags/v2.6.0.tar.gz
    URL_HASH SHA256=aa4c9fb1ae5fc5242f914c65f3557e817e40cb37f04a31e5ff76d1ab89dbf674
    ${epi_fetch_timestamp}
)
FetchContent_MakeAvailable(unity)
# 64-bit asserts everywhere: Unity enables them only on 64-bit hosts by
# default, so 32-bit targets (wasm32) would fail the int64 tests.
target_compile_definitions(unity PUBLIC UNITY_SUPPORT_64)

include(CTest)

# Test functions live in test_*.c (no main) so other runners can reuse them.
add_executable(epaper_image_tests
    test_runner.c
    test_list.c
    test_strerror.c
    test_math.c
    test_dither.c
    test_intent.c
    test_converter.c
    test_grey.c
    test_resampler.c
)
target_link_libraries(epaper_image_tests PRIVATE
    epaper_image::epaper_image
    unity
)
# Tests may include the library's internal headers (src/).
target_include_directories(epaper_image_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
# libm for the test oracles (cbrt, atan2, sin, cos, sqrt); not needed by the library.
if(NOT MSVC)
    target_link_libraries(epaper_image_tests PRIVATE m)
endif()
set_target_properties(epaper_image_tests PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
)
add_test(NAME epaper_image_tests COMMAND epaper_image_tests)

# The public header must be usable from C++ (extern "C" guards).
add_executable(epaper_image_cxx_header_test cxx_header_test.cpp)
target_link_libraries(epaper_image_cxx_header_test PRIVATE epaper_image::epaper_image)
add_test(NAME cxx_header COMMAND epaper_image_cxx_header_test)

set(epi_test_targets epaper_image_tests epaper_image_cxx_header_test)

# The randomized API harness (see tests/fuzz.c); run it with sanitizers.
if(EPAPER_IMAGE_BUILD_FUZZ)
    add_executable(epaper_image_fuzz fuzz.c)
    target_link_libraries(epaper_image_fuzz PRIVATE epaper_image::epaper_image)
    target_include_directories(epaper_image_fuzz PRIVATE ${PROJECT_SOURCE_DIR}/src)
    set_target_properties(epaper_image_fuzz PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
    )
    add_test(NAME epaper_image_fuzz COMMAND epaper_image_fuzz 8000)
    list(APPEND epi_test_targets epaper_image_fuzz)
endif()

# Same warning flags as the library, on our own test targets only (not Unity).
foreach(tgt ${epi_test_targets})
    if(MSVC)
        target_compile_options(${tgt} PRIVATE /W4 /fp:precise)
        if(EPAPER_IMAGE_WARNINGS_AS_ERRORS)
            target_compile_options(${tgt} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${tgt} PRIVATE -Wall -Wextra -Wpedantic -ffp-contract=off)
        if(EPAPER_IMAGE_WARNINGS_AS_ERRORS)
            target_compile_options(${tgt} PRIVATE -Werror)
        endif()
    endif()
endforeach()
