idf_component_register(
    SRCS "SDStorage.cpp"
    INCLUDE_DIRS "include"
    REQUIRES board AXP2101 XL9555 fatfs sdmmc freertos esp_driver_gpio
)
