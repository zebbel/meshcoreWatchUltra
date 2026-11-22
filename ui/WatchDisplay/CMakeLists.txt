idf_component_register(
    SRCS "WatchDisplay.cpp" "DisplayTest.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES CO5300 DisplayPower esp_lcd freertos
                  espressif__esp_lvgl_adapter lvgl__lvgl
)
