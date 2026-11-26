#include "Power.h"

namespace hardware::power {
    esp_err_t init(){ return hardware::axp2101::init(); }
    esp_err_t read_battery(battery_info_t &out){ return hardware::axp2101::read_battery(out); }
    esp_err_t read_charging(bool &charging){ return hardware::axp2101::read_charging(charging); }
}
