#include "Power.h"

namespace hardware::power {
    esp_err_t init(){ return AXP2101::init(); }
    esp_err_t configure(){ return AXP2101::configure(); }
    esp_err_t read_battery(battery_info_t &out){ return AXP2101::read_battery(out); }
    esp_err_t read_charging(bool &charging){ return AXP2101::read_charging(charging); }
}
