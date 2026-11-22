#pragma once

#include <stdint.h>
#include "esp_err.h"

namespace hardware::watch_display {
    // Called once after board, AXP2101 and XL9555 initialization.
    // Starts LVGL and shows a static panel bring-up screen.
    esp_err_t init();
    // Brightness in percent (0-100), independent of future watch-face settings.
    esp_err_t set_brightness(uint8_t percentage);
    bool ready();
}
