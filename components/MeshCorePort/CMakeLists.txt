
set(MESHCORE_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/../../third_party/MeshCore")

set(MONOCYPHER_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/../../third_party/Monocypher")

if(NOT EXISTS "${MESHCORE_ROOT}/src/Mesh.cpp")
    message(FATAL_ERROR
        "MeshCore submodule missing: git submodule update --init --recursive")
endif()

if(NOT EXISTS
    "${MONOCYPHER_ROOT}/src/optional/monocypher-ed25519.c")
    message(FATAL_ERROR
        "Monocypher submodule missing: add third_party/Monocypher at tag 4.0.2")
endif()

idf_component_register(
    SRCS
        "${MESHCORE_ROOT}/src/Packet.cpp"
        "${MESHCORE_ROOT}/src/Dispatcher.cpp"
        "${MESHCORE_ROOT}/src/Mesh.cpp"
        "${MESHCORE_ROOT}/src/Utils.cpp"
        "${MESHCORE_ROOT}/src/Identity.cpp"
        "${MESHCORE_ROOT}/src/helpers/StaticPoolPacketManager.cpp"

        "${MESHCORE_ROOT}/lib/ed25519/fe.c"
        "${MESHCORE_ROOT}/lib/ed25519/ge.c"
        "${MESHCORE_ROOT}/lib/ed25519/sc.c"
        "${MESHCORE_ROOT}/lib/ed25519/sha512.c"
        "${MESHCORE_ROOT}/lib/ed25519/keypair.c"
        "${MESHCORE_ROOT}/lib/ed25519/key_exchange.c"
        "${MESHCORE_ROOT}/lib/ed25519/sign.c"

        "${MONOCYPHER_ROOT}/src/monocypher.c"
        "${MONOCYPHER_ROOT}/src/optional/monocypher-ed25519.c"

        "MeshCorePlatform.cpp"
        "WatchRouting.cpp"
        "WatchMesh.cpp"
        "WatchIdentityStore.cpp"
        "WatchBLE.cpp"
        "WatchDisplay.cpp"
        "WatchCompanion.cpp"
        "WatchContacts.cpp"
        "WatchSettings.cpp"
        "WatchTelemetry.cpp"
        "MeshCoreRadio.cpp"
        "Ed25519.cpp"

    INCLUDE_DIRS
        "include"
        "compat"
        "${MESHCORE_ROOT}/src"
        "${MESHCORE_ROOT}/lib/ed25519"
        "${MONOCYPHER_ROOT}/src"
        "${MONOCYPHER_ROOT}/src/optional"

    REQUIRES
        MIA_M10Q
        AXP2101
        SX1262
        mbedtls
        esp_timer
        esp_hw_support
        esp_system
        nvs_flash
        esp_partition
        bt
        CO5300
        bootloader_support
        freertos
)

target_compile_options(
    ${COMPONENT_LIB}
    PRIVATE
    -Wno-error=reorder
)

target_compile_definitions(
    ${COMPONENT_LIB}
    PRIVATE
    ED25519_NO_SEED=1
    MESH_DEBUG=0
    MESH_PACKET_LOGGING=0
    BRIDGE_DEBUG=0
)
