idf_component_register(
    SRCS "DRV2605.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_common
    PRIV_REQUIRES
        board
        XL9555
        esp_driver_i2c
        esp_timer
        esp_rom
        log
)
