idf_component_register(
    SRCS "Board.cpp" "board.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_spi
)
