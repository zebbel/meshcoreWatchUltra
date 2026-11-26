idf_component_register(
    SRCS "HardwareSystem.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_common
    PRIV_REQUIRES
        esp_timer
        esp_system
        esp_hw_support
        esp_partition
        bootloader_support
)
