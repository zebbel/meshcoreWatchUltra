
cmake_minimum_required(VERSION 3.22)

list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/hardware" "${CMAKE_CURRENT_LIST_DIR}/meshcore" "${CMAKE_CURRENT_LIST_DIR}/services" "${CMAKE_CURRENT_LIST_DIR}/ui")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

idf_build_set_property(MINIMAL_BUILD ON)

project(meshcoreWatchUltra)
