idf_component_register(
    SRCS "DisplayPower.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES CST9217 esp_timer esp_pm freertos board esp_driver_gpio # MCW_DISPLAY_WAKE_GUARD_V1
)
