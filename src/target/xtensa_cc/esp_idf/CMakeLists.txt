# CMakeLists.txt — M5Stack CardComputer target (ESP-IDF build)
#
# This is the top-level CMakeLists.txt for the ESP-IDF build system.
# ESP-IDF's idf.py uses this as the project root.
#
# Build:
#   export IDF_TOOLS_PATH=$PWD/tools/xtensa-toolchain
#   source third_party/esp-idf/export.sh
#   cd src/target/xtensa_cc
#   idf.py build
#
# Flash:
#   idf.py -p /dev/ttyACM0 flash monitor

cmake_minimum_required(VERSION 3.16)

set(PPAP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)

# Tell ESP-IDF where to find the PPAP kernel component
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/components")

# ESP-IDF project setup
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ppap_xtensa_cc)
