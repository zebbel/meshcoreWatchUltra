
idf_component_register(
    SRCS "Display.cpp" "CO5300.cpp" "CO5300Panel.cpp"
    INCLUDE_DIRS "include"
    REQUIRES board AXP2101 XL9555 esp_lcd
    PRIV_REQUIRES esp_driver_spi esp_driver_gpio freertos espressif__esp_lcd_co5300
)
