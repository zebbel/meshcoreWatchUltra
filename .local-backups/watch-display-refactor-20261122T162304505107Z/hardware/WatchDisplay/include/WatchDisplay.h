#pragma once

#include <stdint.h>
#include "esp_err.h"

namespace hardware::watch_display {
    // Called once after board, AXP2101 and XL9555 initialization.
    // Starts LVGL and shows a static panel bring-up screen.
    esp_err_t init();
    // Brightness in percent (0-100), independent of future watch-face settings.
    esp_err_t set_brightness(uint8_t percentage);
    // Only CO5300 display output is disabled; I2C touch remains powered.
    esp_err_t set_screen_awake(bool awake);
    bool screen_awake();
    bool ready();
}
