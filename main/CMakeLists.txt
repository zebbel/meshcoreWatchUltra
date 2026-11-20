
idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES board AXP2101 XL9555 CO5300
    PRIV_REQUIRES esp_psram
)
