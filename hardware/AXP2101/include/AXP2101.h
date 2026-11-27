#pragma once

#include "esp_err.h"

#include <stdint.h>

namespace hardware::axp2101 {

    struct battery_info_t {
        bool voltage_valid = false;
        uint16_t millivolts = 0;
        bool percent_valid = false;
        uint8_t percentage = 0;
    };

    enum class rail_t : uint8_t {
        sd = 0,
        display = 1,
        radio = 2,
        sensor = 3,
        gps = 4,
        speaker = 5,
        nfc = 7
    };

    esp_err_t init();
    esp_err_t configure();

    esp_err_t read_battery(battery_info_t &out);
    // True only while battery charging is active, not when charge is complete.
    esp_err_t read_charging(bool &charging);
    // Short power-key event only; do not change hardware shutdown settings.
    esp_err_t configure_power_key_irq();
    esp_err_t consume_power_key(bool &pressed);

    esp_err_t set_enabled(rail_t rail, bool enabled);
    esp_err_t enable(rail_t rail);
    esp_err_t disable(rail_t rail);
}
