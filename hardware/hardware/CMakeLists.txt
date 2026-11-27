idf_component_register(
    SRCS "hardware.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        Storage
        board
        AXP2101
        XL9555
        MIA_M10Q
        BHI260AP
        SD
        Bluetooth
        log
)