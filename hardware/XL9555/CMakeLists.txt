
idf_component_register(
    SRCS "GPIO.cpp" "XL9555.cpp"
    INCLUDE_DIRS "include"
    REQUIRES board
    PRIV_REQUIRES esp_driver_i2c freertos
)
