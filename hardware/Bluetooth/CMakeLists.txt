idf_component_register(
    SRCS "BluetoothTransport.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_common
        freertos
    PRIV_REQUIRES
        bt
        esp_timer
        esp_hw_support
        esp_system
        Storage
)
