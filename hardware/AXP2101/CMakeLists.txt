idf_component_register(
    SRCS
        "Power.cpp"
        "AXP2101.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_common
    PRIV_REQUIRES
        board
        esp_driver_i2c
        Storage
)
