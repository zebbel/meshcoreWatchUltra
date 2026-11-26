#pragma once

#include <stdint.h>

// Watch status is collected by app_main and read as a thread-safe snapshot.
namespace watch::status {
    struct WatchStatus {
        bool gps_enabled = false;
        bool gps_has_fix = false; // Fresh, valid position (not an old GGA record)
        bool gps_acquiring = false; // GPS receiver is currently powered
        uint8_t gps_satellites = 0; // Satellites from last GGA, shown only for a fresh fix
        bool meshcore_active = false;   // MeshCore initialized and storage available
        uint32_t meshcore_rx_packets = 0; // MeshCore RX packets since boot
        bool ble_connected = false; // Authenticated BLE companion connection
    };

    // Collect hardware and protocol state on the main application task once per second.
    void update(bool meshcore_enabled);
    // RAM-only copy; safe to call from the LVGL task.
    WatchStatus get();
} // namespace watch::status
