
idf_component_register(
    SRCS "Display.cpp" "CO5300.cpp"
    INCLUDE_DIRS "include"
    REQUIRES board AXP2101 XL9555
    PRIV_REQUIRES esp_driver_spi esp_driver_gpio freertos
)
