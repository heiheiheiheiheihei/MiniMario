# MiniMario - NES emulator for Waveshare ESP32-C6-LCD-1.47
cmake_minimum_required(VERSION 3.16)

# Third-party IDF components (Bluepad32 + BTstack, vendored, no network at build)
set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/bluepad32/src/components)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(MiniMario)
