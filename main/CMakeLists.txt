idf_component_register(
    SRCS "main.cpp"
    PRIV_REQUIRES
        Bluetooth
        WatchDisplay
        SX1262
        MeshCorePort
        PowerManagement
        TimeService
        log
        hardware
        services
        watch
)
