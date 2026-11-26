#include "Telemetry.h"

#include "esp_log.h"

#include <math.h>

#include "Power.h"
#include "GPS.h"

namespace meshcore {

    static const char *TAG = "Telemetry";

    static constexpr uint8_t CHANNEL_SELF = 1;
    static constexpr uint8_t LPP_VOLTAGE = 0x74;
    static constexpr uint8_t LPP_PERCENTAGE = 0x78;
    static constexpr uint8_t LPP_GPS = 0x88;

    // ------------------------------------------------------------------------
    // Signed 24-bit wire encoding
    // ------------------------------------------------------------------------

    static void add_i24(uint8_t *out, int32_t value){
        uint32_t bits = static_cast<uint32_t>(value) & 0xFFFFFF;

        out[0] = static_cast<uint8_t>(bits >> 16);
        out[1] = static_cast<uint8_t>(bits >> 8);
        out[2] = static_cast<uint8_t>(bits);
    }

    // ------------------------------------------------------------------------
    // Battery voltage
    // ------------------------------------------------------------------------

    uint16_t Telemetry::battery_millivolts() const{
        hardware::power::battery_info_t battery;

        if(hardware::power::read_battery(battery) != ESP_OK || !battery.voltage_valid){
            return 0;
        }

        return battery.millivolts;
    }

    // ------------------------------------------------------------------------
    // Permission-filtered telemetry encoding
    // ------------------------------------------------------------------------

    size_t Telemetry::encode(uint8_t permissions, uint8_t *out, size_t capacity){

        if(out == nullptr || !(permissions & TELEM_BASE)){
            return 0;
        }

        size_t n = 0;

        // Battery measurements come from the AXP2101 component.
        hardware::power::battery_info_t battery;
        hardware::power::read_battery(battery);

        if(battery.voltage_valid && capacity - n >= 4){
            uint16_t scaled = static_cast<uint16_t>((battery.millivolts + 5U) / 10U);

            out[n++] = CHANNEL_SELF;
            out[n++] = LPP_VOLTAGE;
            out[n++] = static_cast<uint8_t>(scaled >> 8);
            out[n++] = static_cast<uint8_t>(scaled);
        }

        if(battery.percent_valid && capacity - n >= 3){
            out[n++] = CHANNEL_SELF;
            out[n++] = LPP_PERCENTAGE;
            out[n++] = battery.percentage;
        }

        // Obtain GPS readings exclusively through MIA_M10Q.
        if(permissions & TELEM_LOCATION){
            hardware::gps::fix_t fix;

            if(hardware::gps::get_fresh_fix(fix) && capacity - n >= 11){

                int32_t lat = static_cast<int32_t>(lround(fix.latitude * 10000.0));

                int32_t lon = static_cast<int32_t>(lround(fix.longitude * 10000.0));

                int32_t alt = static_cast<int32_t>(lround(fix.altitude * 100.0));

                out[n++] = CHANNEL_SELF;
                out[n++] = LPP_GPS;

                add_i24(&out[n], lat);
                n += 3;

                add_i24(&out[n], lon);
                n += 3;

                add_i24(&out[n], alt);
                n += 3;

            }else{
                // Never transmit a stale location.
                hardware::gps::request_fix();
                ESP_LOGD(TAG, "No fresh GPS fix available");
            }
        }

        return n;
    }

}
