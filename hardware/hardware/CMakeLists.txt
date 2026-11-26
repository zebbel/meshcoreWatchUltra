
idf_component_register(
    SRCS "hardware.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        Storage
        board
        AXP2101
        XL9555
        WatchDisplay
        TimeService
        MIA_M10Q
        Bluetooth
        SD
        MeshCorePort
        SX1262
        log
)