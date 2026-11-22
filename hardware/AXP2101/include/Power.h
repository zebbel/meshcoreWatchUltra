#pragma once

#include "AXP2101.h"

namespace hardware::power {
    using battery_info_t = AXP2101::battery_info_t;
    esp_err_t init();
    esp_err_t configure();
    esp_err_t read_battery(battery_info_t &out);
}
