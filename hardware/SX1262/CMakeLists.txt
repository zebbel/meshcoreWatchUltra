idf_component_register(
    SRCS
        "LoRa.cpp"
        "SX1262.cpp"
    INCLUDE_DIRS "include"
    # exported LoRa headers expose TaskHandle_t.
    REQUIRES
        esp_common
        freertos
    PRIV_REQUIRES
        board
        AXP2101
        XL9555
        esp_driver_spi
        esp_driver_gpio
)
