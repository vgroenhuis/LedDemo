cmake_minimum_required(VERSION 3.16)
# The firmware version is the first line of VERSION; the device reports it and the server compares it
# with the release it wants the device to run.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/VERSION")   # a new VERSION re-runs CMake
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/VERSION" PROJECT_VER LIMIT_COUNT 1)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(turtle)
