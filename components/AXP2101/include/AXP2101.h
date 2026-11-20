
#pragma once

#include <stdint.h>
#include "esp_err.h"

namespace AXP2101{
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

    esp_err_t init();                                                                                   // Initialize the AXP2101 device on the shared I2C bus
    esp_err_t configure();                                                                              // Apply configured voltage, charging and startup settings
    esp_err_t set_enabled(rail_t rail, bool enabled);                                                   // Control peripheral supply rails
    esp_err_t enable(rail_t rail);                                                                      // Enable peripheral supply rails
    esp_err_t disable(rail_t rail);                                                                     // Disable peripheral supply rails
}
