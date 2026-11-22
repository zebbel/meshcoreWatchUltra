idf_component_register(
    SRCS "BluetoothTransport.cpp"
    INCLUDE_DIRS "include"
    REQUIRES bt freertos esp_timer esp_hw_support esp_system
)
