idf_component_register(
    SRCS "WatchDisplay.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES
        board
        AXP2101
        XL9555
        esp_driver_spi
        esp_driver_gpio
        esp_lcd
        freertos
        espressif__esp_lcd_co5300
        espressif__esp_lvgl_adapter
        lvgl__lvgl
)
