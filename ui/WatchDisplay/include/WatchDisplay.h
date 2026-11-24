#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "WatchStatus.h"

// Watch UI entry point: LVGL lifecycle and integration only; no pin or I2C/SPI knowledge.
namespace ui::watch_display {
    esp_err_t init();
    esp_err_t set_brightness(uint8_t percentage);
    esp_err_t set_screen_awake(bool awake);
    bool screen_awake();
    bool ready();
    void publish_watch_status(const watch::WatchStatus &status);
}
