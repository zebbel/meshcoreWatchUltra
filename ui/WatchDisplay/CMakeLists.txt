idf_component_register(
    SRCS "WatchDisplay.cpp" "DisplayTest.cpp" "ClockPage.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES CO5300 DisplayPower AXP2101 esp_lcd freertos heap
                  espressif__esp_lvgl_adapter lvgl__lvgl
)
