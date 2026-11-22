#pragma once

#include <stdint.h>
#include "esp_err.h"

// Watch UI entry point: LVGL lifecycle and integration only; no pin or I2C/SPI knowledge.
namespace ui::watch_display {
    esp_err_t init();
    esp_err_t set_brightness(uint8_t percentage);
    esp_err_t set_screen_awake(bool awake);
    bool screen_awake();
    bool ready();
}

// Temporary compatibility API for existing local main.cpp and callers.
// New code should use ui::watch_display or services::display_power directly.
namespace hardware::watch_display {
    inline esp_err_t init() { return ui::watch_display::init(); }
    inline esp_err_t set_brightness(uint8_t percent) { return ui::watch_display::set_brightness(percent); }
    inline esp_err_t set_screen_awake(bool awake) { return ui::watch_display::set_screen_awake(awake); }
    inline bool screen_awake() { return ui::watch_display::screen_awake(); }
    inline bool ready() { return ui::watch_display::ready(); }
}
