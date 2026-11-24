#pragma once

#include <stdint.h>

// A tiny thread-safe view of application state for the watch face.
// app_main publishes measurements; LVGL consumes them without calling GPS,
// MeshCore, or BLE code from the rendering task.
namespace ui::watch_display {

struct WatchStatus {
    bool gps_enabled = false;
    bool gps_has_fix = false;       // Fresh, valid position (not an old GGA record)
    bool gps_acquiring = false;     // GPS receiver is currently powered
    uint8_t gps_satellites = 0;     // Satellites from last GGA, shown only for a fresh fix
    bool meshcore_active = false;   // MeshCore initialized and storage available
    uint32_t meshcore_rx_packets = 0; // MeshCore RX packets since boot
    bool ble_connected = false;     // Authenticated BLE companion connection
    // MCW_POWER_OPTIMIZATION_V2: on-device diagnostics while USB is absent.
    bool pm_enabled = false;
    uint8_t sleep_percent = 0;      // Actual cumulative sleep / enabled elapsed
    uint32_t sleep_entries = 0;     // Successful light-sleep exits since boot
};

void publish_watch_status(const WatchStatus &status);

} // namespace ui::watch_display
