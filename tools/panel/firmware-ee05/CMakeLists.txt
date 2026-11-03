# Panel viewer firmware for the Seeed Studio XIAO ePaper Display Board EE05
# (ESP32-S3 + 7.5" 800x480 UC8179 panel, GDEY075T7 glass); see ../README.md.
cmake_minimum_required(VERSION 3.16)

# The optional local idfxx checkout, and the shared protocol component.
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/idfxx.cmake)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(epaper_panel_viewer_ee05)
