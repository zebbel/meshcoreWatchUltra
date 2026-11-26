#include "WatchStatus.h"
#include "HardwareSystem.h"
#include "GPS.h"
#include "SDStorage.h"
#include "MeshCorePort.h"
#include "BluetoothTransport.h"
#include "PowerManagement.h"
#include "freertos/FreeRTOS.h"

namespace watch {
    namespace {
        WatchStatus s_status{};
        portMUX_TYPE s_status_mux = portMUX_INITIALIZER_UNLOCKED;
    }

    // Collect on app_main only. Never call drivers or services inside the lock.
    void update_status(bool meshcore_enabled) {
        static uint32_t last_update_ms = 0;
        const uint32_t now = hardware::system::millis();
        if (last_update_ms != 0 && now - last_update_ms < 1000U) return;
        last_update_ms = now;

        watch::WatchStatus snapshot{};
        snapshot.gps_enabled = hardware::gps::is_enabled();
        hardware::gps::fix_t fix{};
        snapshot.gps_has_fix = hardware::gps::get_fresh_fix(fix);
        hardware::gps::status_t gps_info{};
        hardware::gps::get_status(gps_info);
        snapshot.gps_acquiring = snapshot.gps_enabled && gps_info.powered;
        if(snapshot.gps_has_fix) snapshot.gps_satellites = gps_info.satellites;

        snapshot.meshcore_active = meshcore_enabled && hardware::sd::ready();
        if(snapshot.meshcore_active) snapshot.meshcore_rx_packets = meshcore::received_packet_count();
        snapshot.ble_connected = hardware::bluetooth::transport().connected();
        const auto sleep = services::power_management::sleep_stats();
        snapshot.pm_enabled = sleep.pm_enabled;
        snapshot.sleep_percent = sleep.sleep_percent;
        snapshot.sleep_entries = sleep.sleep_entries;
        portENTER_CRITICAL(&s_status_mux);
        s_status = snapshot;
        portEXIT_CRITICAL(&s_status_mux);
    }

    WatchStatus get_status() {
        portENTER_CRITICAL(&s_status_mux);
        const WatchStatus snapshot = s_status;
        portEXIT_CRITICAL(&s_status_mux);
        return snapshot;
    }
} // namespace watch
