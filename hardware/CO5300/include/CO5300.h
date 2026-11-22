
#pragma once

#include <stdint.h>
#include "esp_err.h"

namespace CO5300 {
    esp_err_t init();                                                                                   // Initialize display power, QSPI interface and controller
    esp_err_t set_brightness(uint8_t brightness);                                                       // Set AMOLED brightness (0-255)
    esp_err_t fill_rect(uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint16_t color);       // Draw a solid RGB565 rectangle in portrait coordinates
    esp_err_t draw_rgb565(uint16_t x, uint16_t y, uint16_t width, uint16_t height, const uint16_t *pixels);
    esp_err_t draw_test_pattern();                                                                      // Draw a red, green and blue hardware test pattern
    esp_err_t shutdown();                                                                               // Put the panel to sleep and turn off its power supply

}
