cmake_minimum_required(VERSION 3.14)

# ESP-IDF component support (must be before project() for script mode compatibility)
if(ESP_PLATFORM)
    idf_component_register(
        SRCS "src/epaper_image.c" "src/epi_math.c" "src/epi_srgb.c"
             "src/epi_intent.c" "src/epi_dither.c" "src/epi_converter.c"
             "src/epi_grey_panel.c" "src/epi_colour_panel.c" "src/epi_resampler.c" "src/epi_preview.c"
             "src/panels/el133uf1.c" "src/panels/gdey075t7.c"
        INCLUDE_DIRS "include"
        PRIV_INCLUDE_DIRS "src"
    )
    # Determinism (design.md "Fixed-point arithmetic and determinism"):
    # no FMA contraction in init-time double code.
    target_compile_options(${COMPONENT_LIB} PRIVATE -ffp-contract=off)
    # IDF builds gnu17, so epi_fp.h cannot rely on __STRICT_ANSI__.
    target_compile_definitions(${COMPONENT_LIB} PRIVATE EPI_FP_CONTRACT_OFF)
    return()
endif()

project(epaper_image
    VERSION 0.3.0
    DESCRIPTION "Image conversion for e-paper panels"
    HOMEPAGE_URL "https://github.com/cleishm/epaper-image"
    LANGUAGES C
)

# PROJECT_IS_TOP_LEVEL needs CMake 3.21; we support 3.14.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(EPAPER_IMAGE_IS_TOP_LEVEL ON)
else()
    set(EPAPER_IMAGE_IS_TOP_LEVEL OFF)
endif()

option(EPAPER_IMAGE_BUILD_TESTS "Build tests" ${EPAPER_IMAGE_IS_TOP_LEVEL})
# The convert command-line tool (tools/convert); a host tool, so off for
# Emscripten.
if(EPAPER_IMAGE_IS_TOP_LEVEL AND NOT EMSCRIPTEN)
    set(EPAPER_IMAGE_BUILD_TOOLS_DEFAULT ON)
else()
    set(EPAPER_IMAGE_BUILD_TOOLS_DEFAULT OFF)
endif()
option(EPAPER_IMAGE_BUILD_TOOLS "Build the convert tool" ${EPAPER_IMAGE_BUILD_TOOLS_DEFAULT})
# The randomised API harness (tests/fuzz.c), for sanitizer runs; needs
# EPAPER_IMAGE_BUILD_TESTS. Not built by default or under ESP-IDF.
option(EPAPER_IMAGE_BUILD_FUZZ "Build the randomised API harness" OFF)
option(EPAPER_IMAGE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

# Main library target (static only: nothing is exported for a shared build)
add_library(epaper_image STATIC
    src/epaper_image.c
    src/epi_math.c
    src/epi_srgb.c
    src/epi_intent.c
    src/epi_dither.c
    src/epi_converter.c
    src/epi_grey_panel.c
    src/epi_colour_panel.c
    src/epi_resampler.c
    src/epi_preview.c
    src/panels/el133uf1.c
    src/panels/gdey075t7.c
)
add_library(epaper_image::epaper_image ALIAS epaper_image)

target_include_directories(epaper_image PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_include_directories(epaper_image PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

set_target_properties(epaper_image PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
)

if(MSVC)
    # Determinism (design.md "Fixed-point arithmetic and determinism"):
    # IEEE semantics, no contraction.
    target_compile_options(epaper_image PRIVATE /W4 /fp:precise)
    if(EPAPER_IMAGE_WARNINGS_AS_ERRORS)
        target_compile_options(epaper_image PRIVATE /WX)
    endif()
else()
    target_compile_options(epaper_image PRIVATE -Wall -Wextra -Wpedantic -ffp-contract=off)
    target_compile_definitions(epaper_image PRIVATE EPI_FP_CONTRACT_OFF)
    if(EPAPER_IMAGE_WARNINGS_AS_ERRORS)
        target_compile_options(epaper_image PRIVATE -Werror)
    endif()
endif()

# Tests
if(EPAPER_IMAGE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Tools (never installed)
if(EPAPER_IMAGE_BUILD_TOOLS)
    add_subdirectory(tools/convert)
endif()

# Documentation
option(EPAPER_IMAGE_BUILD_DOCS "Build documentation" OFF)

if(EPAPER_IMAGE_BUILD_DOCS)
    find_package(Doxygen)
    if(DOXYGEN_FOUND)
        set(DOXYGEN_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/docs)
        add_custom_target(docs
            COMMAND ${CMAKE_COMMAND} -E make_directory ${DOXYGEN_OUTPUT_DIRECTORY}
            COMMAND ${DOXYGEN_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/docs/Doxyfile
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/docs
            COMMENT "Generating API documentation with Doxygen"
            VERBATIM
        )
        message(STATUS "Doxygen found: documentation can be built with 'make docs'")
    else()
        message(WARNING "Doxygen not found: documentation cannot be built")
    endif()
endif()

# Installation
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS epaper_image
    EXPORT epaper_imageTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT epaper_imageTargets
    FILE epaper_imageTargets.cmake
    NAMESPACE epaper_image::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/epaper_image
)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/epaper_imageConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/epaper_imageConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/epaper_image
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/epaper_imageConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/epaper_imageConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/epaper_imageConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/epaper_image
)
