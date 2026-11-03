# ESP-IDF test app: runs the host Unity suite (tests/test_*.c, list in
# tests/test_list.c) on ESP32 targets, e.g. under QEMU. See README.md.
cmake_minimum_required(VERSION 3.16)

# The repository root is the epaper-image component (its CMakeLists.txt has
# the ESP_PLATFORM branch); it is one component, so tests/ and tools/ beneath
# it are not scanned.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../..")
# Only what the test app needs (faster builds, smaller image).
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(epaper_image_tests)
