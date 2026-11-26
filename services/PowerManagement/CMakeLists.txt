idf_component_register(SRCS "PowerManagement.cpp" INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_pm esp_hw_support esp_driver_gpio board log freertos SX1262 Bluetooth)
