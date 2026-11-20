
idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES board AXP2101
    PRIV_REQUIRES esp_psram
)
