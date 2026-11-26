idf_component_register(
    SRCS "DisplayPower.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_common
    PRIV_REQUIRES
        AXP2101
        CST9217
        esp_timer
        esp_pm
        freertos
        board
        esp_driver_gpio
)
