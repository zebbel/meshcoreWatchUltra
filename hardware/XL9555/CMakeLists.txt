idf_component_register(
    SRCS
        "GPIO.cpp"
        "XL9555.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_common
    PRIV_REQUIRES
        board
        esp_driver_i2c
        freertos
)
