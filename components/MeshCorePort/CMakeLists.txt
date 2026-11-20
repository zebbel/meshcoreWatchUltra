
set(MESHCORE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../third_party/MeshCore")

if(NOT EXISTS "${MESHCORE_ROOT}/src/Packet.cpp")
    message(FATAL_ERROR "MeshCore submodule missing. Run git submodule update --init --recursive")
endif()

idf_component_register(
    SRCS
        "${MESHCORE_ROOT}/src/Packet.cpp"
        "MeshCoreRadio.cpp"
        "MeshCorePlatform.cpp"
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

# Upstream MeshCore Dispatcher.h has a constructor initializer
# order warning. Keep upstream sources unmodified.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=reorder
)
