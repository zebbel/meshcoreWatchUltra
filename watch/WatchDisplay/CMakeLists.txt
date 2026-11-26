# EEZ Studio owns all UI and navigation. Export Flow + its matching runtime.
set(WATCH_EEZ_DIR "${CMAKE_CURRENT_LIST_DIR}/generated")
foreach(required screens.h screens.c ui.h ui.c vars.h eez-flow.h eez-flow.cpp)
    if(NOT EXISTS "${WATCH_EEZ_DIR}/${required}")
        message(FATAL_ERROR "WatchDisplay: missing Flow export generated/${required}")
    endif()
endforeach()
file(GLOB_RECURSE WATCH_EEZ_SOURCES
    "${WATCH_EEZ_DIR}/*.c" "${WATCH_EEZ_DIR}/*.cpp")
# Some EEZ runtime releases use hard-coded lvgl/lvgl.h and
# lvgl/src/lvgl_private.h even with the export include set to lvgl.h.
# Build-directory forwarding headers avoid modifying generated sources.
set(WATCH_EEZ_COMPAT "${CMAKE_CURRENT_BINARY_DIR}/eez-include")
file(MAKE_DIRECTORY "${WATCH_EEZ_COMPAT}/lvgl/src")
file(WRITE "${WATCH_EEZ_COMPAT}/lvgl/lvgl.h" "#pragma once\n#include <lvgl.h>\n")
file(WRITE "${WATCH_EEZ_COMPAT}/lvgl/src/lvgl_private.h" "#pragma once\n#include <src/lvgl_private.h>\n")
idf_component_register(
    SRCS "WatchDisplay.cpp" "FlowBridge.cpp" ${WATCH_EEZ_SOURCES}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "${WATCH_EEZ_DIR}" "${WATCH_EEZ_COMPAT}"
    REQUIRES esp_common WatchStatus
    PRIV_REQUIRES CO5300 DisplayPower AXP2101 esp_lcd freertos
                  espressif__esp_lvgl_adapter lvgl__lvgl
)
message(STATUS "WatchDisplay: EEZ Flow UI")
