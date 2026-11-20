
idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES board AXP2101 XL9555 CO5300 SX1262 MeshCorePort
    PRIV_REQUIRES esp_psram freertos
)

# Allow the existing upstream MeshCore constructor order warning.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=reorder
)
