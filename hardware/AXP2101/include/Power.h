#pragma once

#include "AXP2101.h"

namespace hardware::power {
    using battery_info_t = hardware::axp2101::battery_info_t;
    esp_err_t init();
    esp_err_t read_battery(battery_info_t &out);
    // True only while battery charging is active, not when charge is complete.
    esp_err_t read_charging(bool &charging);
}
