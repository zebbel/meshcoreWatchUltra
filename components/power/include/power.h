
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

    // Initialize the AXP2101 device on the shared I2C bus
    esp_err_t init();

    // Read battery, charging and regulator information
    esp_err_t read_status(status_t &status);

    // Print power diagnostics to the ESP-IDF console
    esp_err_t log_status();

}
