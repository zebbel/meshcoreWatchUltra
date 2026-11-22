idf_component_register(
    SRCS "DisplayPower.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES CST9217 esp_timer freertos
)
