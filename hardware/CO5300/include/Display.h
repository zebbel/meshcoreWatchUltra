#pragma once

#include "CO5300.h"

namespace hardware::display {
    esp_err_t init();
    esp_err_t set_brightness(uint8_t brightness);
    esp_err_t fill_rect(uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint16_t color);
    esp_err_t draw_rgb565(uint16_t x, uint16_t y, uint16_t width, uint16_t height, const uint16_t *pixels);
    esp_err_t draw_test_pattern();
    esp_err_t shutdown();
}
