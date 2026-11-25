#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_lcd_types.h"

// CO5300 hardware driver. Owns panel IO, power and synchronous pixel transfers.
namespace hardware::co5300 {
    constexpr uint16_t width = 410;
    constexpr uint16_t height = 502;
    constexpr uint16_t x_gap = 22;

    esp_err_t init();
    // Exclusive right/bottom bounds. Packed RGB565 in panel byte order.
    // Returns only after all submitted pixel DMA has completed on success.
    // Callers must serialize all panel access.
    esp_err_t draw_rgb565(int x_start, int y_start, int x_end, int y_end,
                          const void *color_map);
    // Normal callers keep immediate-on behavior. Wake-time
    // redraw uses defer_display_on=true to keep the CO5300 output blank.
    esp_err_t set_screen_enabled(bool enabled, bool defer_display_on = false);
    // Call after the complete redraw; caller must serialize panel access.
    esp_err_t reveal_after_full_refresh();
    esp_err_t set_brightness(uint8_t percentage);
    // Last successfully requested brightness; caller serializes panel access.
    uint8_t brightness();
    bool ready();
    esp_lcd_panel_handle_t panel();
    esp_lcd_panel_io_handle_t panel_io();
    void shutdown(); // Stop the renderer before calling.
}
