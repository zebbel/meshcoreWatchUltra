idf_component_register(
    SRCS "Notifications.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES MeshCorePort DRV2605 Storage esp_timer log
)
