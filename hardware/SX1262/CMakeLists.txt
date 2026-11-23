
idf_component_register(
    SRCS "LoRa.cpp" "SX1262.cpp"
    INCLUDE_DIRS "include"
    # MCW_CPU_LORA_IDLE_V1: exported LoRa headers expose TaskHandle_t.
    REQUIRES board AXP2101 XL9555 freertos
    PRIV_REQUIRES esp_driver_spi esp_driver_gpio
)
