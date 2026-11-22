idf_component_register(
    SRCS
        "main.cpp"
        "DeviceUI.cpp"

    INCLUDE_DIRS
        "."
        "include"

    REQUIRES
        Bluetooth
        Storage
        System
        CO5300
        board
        AXP2101
        MIA_M10Q
        XL9555
        SX1262
        MeshCorePort
        nvs_flash
        esp_timer
        freertos
)
