idf_component_register(
    SRCS "CO5300Panel.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_common
        esp_lcd
    PRIV_REQUIRES
        board
        AXP2101
        XL9555
        esp_driver_spi
        esp_driver_gpio
        freertos
        heap
        espressif__esp_lcd_co5300
)
