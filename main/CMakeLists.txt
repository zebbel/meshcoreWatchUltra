
idf_component_register(
    SRCS
        "main.cpp"

    INCLUDE_DIRS
        "."

    REQUIRES
        board
        AXP2101
        MIA_M10Q
        XL9555
        CO5300
        SX1262
        MeshCorePort
        nvs_flash
        bootloader_support
        esp_timer
        freertos
)

target_compile_options(
    ${COMPONENT_LIB}
    PRIVATE
    -Wno-error=reorder
)
