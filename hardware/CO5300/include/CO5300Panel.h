#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_lcd_types.h"

// LVGL-compatible CO5300 hardware owner. No LVGL dependency here.
// Do not initialize the legacy CO5300::init() drawing API concurrently.
namespace hardware::co5300 {
    constexpr uint16_t width = 410;
    constexpr uint16_t height = 502;
    constexpr uint16_t x_gap = 22;

    esp_err_t init(size_t max_transfer_bytes);
    esp_err_t set_screen_enabled(bool enabled);
    esp_err_t set_brightness(uint8_t percentage);
    bool ready();
    esp_lcd_panel_handle_t panel();
    esp_lcd_panel_io_handle_t panel_io();
    void shutdown_after_failed_ui_start();
}
