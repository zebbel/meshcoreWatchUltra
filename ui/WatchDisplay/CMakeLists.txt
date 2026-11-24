idf_component_register(
    SRCS "WatchDisplay.cpp" "DisplayTest.cpp" "pages/ClockPage.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_common WatchStatus
    PRIV_REQUIRES CO5300 DisplayPower AXP2101 esp_lcd freertos
                  espressif__esp_lvgl_adapter lvgl__lvgl
)
