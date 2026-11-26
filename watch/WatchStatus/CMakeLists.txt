idf_component_register(
    SRCS "WatchStatus.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        System
        MIA_M10Q
        SD
        MeshCorePort
        Bluetooth
        freertos
)
