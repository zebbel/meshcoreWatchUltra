idf_component_register(
    SRCS "PCF85063A.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_common
    PRIV_REQUIRES
        board
        esp_driver_i2c
)
