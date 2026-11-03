# Panel viewer firmware for the Seeed reTerminal E1004 (see ../README.md).
cmake_minimum_required(VERSION 3.16)

# The optional local idfxx checkout, and the shared protocol component.
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/idfxx.cmake)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(epaper_panel_viewer)
