set(BHI2XY_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../third_party/BHI2xy_SensorAPI")

if(NOT EXISTS "${BHI2XY_ROOT}/firmware/bhi260ap/BHI260AP.fw.h")
    message(FATAL_ERROR "BHI260AP: pinned Bosch SensorAPI missing; run apply_bhi260ap.py")
endif()

idf_component_register(
    SRCS
        "BHI260AP.cpp"
        "${BHI2XY_ROOT}/bhy2.c"
        "${BHI2XY_ROOT}/bhy2_hif.c"
        "${BHI2XY_ROOT}/bhy2_parse.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "${BHI2XY_ROOT}"
    REQUIRES
        esp_common
    PRIV_REQUIRES
        AXP2101
        board
        esp_driver_gpio
        esp_driver_i2c
        esp_rom
        esp_timer
        freertos
        log
)
