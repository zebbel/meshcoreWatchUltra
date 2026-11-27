idf_component_register(
    SRCS "services.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        PowerManagement
        PositionSync
        MIA_M10Q
        Bluetooth
        DisplayPower
        BHI260AP
        Notifications
)
