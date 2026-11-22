idf_component_register(
    SRCS "PCF85063A.cpp"
    INCLUDE_DIRS "include"
    REQUIRES board
    PRIV_REQUIRES esp_driver_i2c
)
