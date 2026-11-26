#include "WatchStatus.h"

#include "freertos/FreeRTOS.h"

#include "HardwareSystem.h"
#include "GPS.h"
#include "SDStorage.h"
#include "MeshCorePort.h"
#include "BluetoothTransport.h"

namespace watch::status {
    namespace {
        constexpr uint32_t kStatusUpdateMs = 1000U;
        WatchStatus s_status{};
        portMUX_TYPE s_status_mux = portMUX_INITIALIZER_UNLOCKED;
    }

    // ------------------------------------------------------------------------
    // Collect and publish status
    // ------------------------------------------------------------------------

    // Read hardware outside the lock and publish a complete snapshot.
    // Collect on app_main only. Never call drivers or services inside the lock.
    void update(bool meshcore_enabled){
        static uint32_t last_update_ms = 0;
        const uint32_t now = hardware::system::millis();
        if(last_update_ms != 0 && now - last_update_ms < kStatusUpdateMs) return;
        last_update_ms = now;

        WatchStatus snapshot{};
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
        portENTER_CRITICAL(&s_status_mux);
        s_status = snapshot;
        portEXIT_CRITICAL(&s_status_mux);
    }

    // ------------------------------------------------------------------------
    // Thread-safe snapshot access
    // ------------------------------------------------------------------------

    WatchStatus get(){
        portENTER_CRITICAL(&s_status_mux);
        const WatchStatus snapshot = s_status;
        portEXIT_CRITICAL(&s_status_mux);
        return snapshot;
    }
} // namespace watch::status
