idf_component_register(SRCS "TimeService.cpp" INCLUDE_DIRS "include"
    REQUIRES esp_common PRIV_REQUIRES PCF85063A esp_timer log)
