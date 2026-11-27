#include "PositionSync.h"

#include "esp_log.h"

#include <math.h>

#include "GPS.h"
#include "MeshCorePort.h"
#include "HardwareSystem.h"

namespace services::position_sync {
    static const char *TAG = "PositionSync";
    static constexpr int64_t position_threshold_microdegrees = 100;
    static constexpr uint32_t save_interval_ms = 300000U;

    // ------------------------------------------------------------------------
    // Persist fresh GPS position
    // ------------------------------------------------------------------------

    // Avoid unnecessary SD writes by requiring a meaningful change and a minimum interval between successful saves.
    void synchronize(){
        static uint32_t last_save_ms = 0;
        hardware::gps::fix_t fix = {};
        if(!hardware::gps::get_fresh_fix(fix)) return;

        const int32_t gps_latitude = static_cast<int32_t>(lround(fix.latitude * 1000000.0));
        const int32_t gps_longitude = static_cast<int32_t>(lround(fix.longitude * 1000000.0));
        int32_t current_latitude = 0;
        int32_t current_longitude = 0;
        if(!meshcore::get_position(current_latitude, current_longitude)) return;

        int64_t latitude_difference = static_cast<int64_t>(gps_latitude) - current_latitude;
        int64_t longitude_difference = static_cast<int64_t>(gps_longitude) - current_longitude;
        if(latitude_difference < 0) latitude_difference = -latitude_difference;
        if(longitude_difference < 0) longitude_difference = -longitude_difference;

        const bool position_missing = current_latitude == 0 && current_longitude == 0;
        const bool position_changed = latitude_difference >= position_threshold_microdegrees || longitude_difference >= position_threshold_microdegrees;
        const uint32_t now = static_cast<uint32_t>(hardware::system::millis());

        if((position_missing || position_changed) && (last_save_ms == 0 || now - last_save_ms >= save_interval_ms)){
            esp_err_t err = meshcore::set_position(gps_latitude, gps_longitude);

            if(err == ESP_OK){
                last_save_ms = now;
            }else{
                ESP_LOGW(TAG, "GPS position could not be saved: %s", esp_err_to_name(err));
            }
        }
    }

}
