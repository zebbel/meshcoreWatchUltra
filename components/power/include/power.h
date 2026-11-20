
#pragma once

#include <stdint.h>
#include "esp_err.h"

namespace power {

    struct status_t {
        bool battery_present = false;
        bool vbus_good = false;
        bool charging = false;
        bool discharging = false;

        // -1 means measurement unavailable
        int battery_mv = -1;
        int vbus_mv = -1;
        int battery_percent = -1;

        // AXP2101 regulator enable states
        bool sd_enabled = false;
        bool display_enabled = false;
        bool radio_enabled = false;
        bool sensor_enabled = false;
        bool gps_enabled = false;
        bool speaker_enabled = false;
        bool nfc_enabled = false;
    };

    // Peripheral power rails controlled by AXP2101
    enum class rail_t : uint8_t {
        sd = 0,
        display = 1,
        radio = 2,
        sensor = 3,
        gps = 4,
        speaker = 5,
        nfc = 7
    };

    // Apply configured voltage, charging and startup settings
    esp_err_t configure();

    // Control peripheral supply rails
    esp_err_t set_enabled(rail_t rail, bool enabled);
    esp_err_t enable(rail_t rail);
    esp_err_t disable(rail_t rail);

    // Initialize the AXP2101 device on the shared I2C bus
    esp_err_t init();
}
