cmake_minimum_required(VERSION 3.16)
# Add both the general components directory and the esp32-camera component directory:
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/components;${CMAKE_CURRENT_SOURCE_DIR}/components/esp32-camera")
set(PROJECT_NAME "esp32_sentry")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(${PROJECT_NAME})
