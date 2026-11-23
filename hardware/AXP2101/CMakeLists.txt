
idf_component_register(
    SRCS "Power.cpp" "AXP2101.cpp"
    INCLUDE_DIRS "include"
    REQUIRES board
    PRIV_REQUIRES esp_driver_i2c Storage
)
