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
    // MCW_AMOLED_RENDER_V4: Normal callers keep immediate-on behavior. Wake-time
    // redraw uses defer_display_on=true to keep the CO5300 output blank.
    esp_err_t set_screen_enabled(bool enabled, bool defer_display_on = false);
    // Call only after the full LVGL redraw has been submitted, with LVGL locked.
    // LCD SPI panel commands drain queued color DMA before being transmitted.
    esp_err_t reveal_after_full_refresh();
    esp_err_t set_brightness(uint8_t percentage);
    bool ready();
    esp_lcd_panel_handle_t panel();
    esp_lcd_panel_io_handle_t panel_io();
    void shutdown_after_failed_ui_start();
}
