
set(MESHCORE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../third_party/MeshCore")

if(NOT EXISTS "${MESHCORE_ROOT}/src/Dispatcher.cpp")
    message(FATAL_ERROR "MeshCore submodule missing. Run git submodule update --init --recursive")
endif()

idf_component_register(
    SRCS
        "${MESHCORE_ROOT}/src/Packet.cpp"
        "${MESHCORE_ROOT}/src/Dispatcher.cpp"
        "${MESHCORE_ROOT}/src/helpers/StaticPoolPacketManager.cpp"
        "MeshCoreRadio.cpp"
        "MeshCorePlatform.cpp"
        "WatchRouting.cpp"
    INCLUDE_DIRS
        "include"
        "compat"
        "${MESHCORE_ROOT}/src"
    REQUIRES
        SX1262
        mbedtls
        esp_timer
        esp_hw_support
        esp_system
)

# Current upstream initializes Dispatcher members in a different order
# from their declarations. Do not modify the submodule for this warning.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=reorder)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    MESH_DEBUG=0
    MESH_PACKET_LOGGING=0
    BRIDGE_DEBUG=0
)
