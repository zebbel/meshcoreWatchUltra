idf_component_register(
    SRCS
        "main.cpp"

    INCLUDE_DIRS
        "."
        "include"

    REQUIRES
        Bluetooth
        SD
        Storage
        System
        CO5300
        WatchDisplay
        board
        AXP2101
        MIA_M10Q
        XL9555
        SX1262
        MeshCorePort
        nvs_flash
        esp_timer
        freertos
        esp_pm # MCW_ENERGY_STAGE2_V1
        esp_hw_support # MCW_AUTO_LIGHT_SLEEP_V1
        esp_driver_gpio
)
