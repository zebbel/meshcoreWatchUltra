idf_component_register(SRCS "PowerManagement.cpp" INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_pm esp_hw_support esp_timer esp_driver_gpio board log)
