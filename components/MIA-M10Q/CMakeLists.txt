
idf_component_register(
    SRCS
        "MIA-M10Q.cpp"

    INCLUDE_DIRS
        "include"

    PRIV_REQUIRES
        board
        AXP2101
        esp_driver_uart
        esp_timer
)
