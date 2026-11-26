#pragma once

#include "esp_err.h"

#include <stdint.h>

// Watch UI entry point: LVGL lifecycle and integration only; no pin or I2C/SPI knowledge.
namespace watch::display {
    esp_err_t init();
    esp_err_t set_screen_awake(bool awake);
    bool screen_awake();
    bool ready();
    // Call on every main-loop iteration; navigation is handled by EEZ.
    void update_bluetooth_pairing();
}
