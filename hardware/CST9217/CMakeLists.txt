idf_component_register(
    SRCS "CST9217.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_common freertos
    PRIV_REQUIRES board XL9555 esp_driver_gpio esp_driver_i2c esp_lcd
                  waveshare__esp_lcd_touch_cst9217
)
