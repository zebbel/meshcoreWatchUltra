idf_component_register(SRCS "WatchApplication.cpp" INCLUDE_DIRS "include"
    PRIV_REQUIRES Bluetooth SD Storage System WatchDisplay board AXP2101
                  MIA_M10Q XL9555 SX1262 MeshCorePort PowerManagement PositionSync TimeService WatchStatus freertos log)
