#include "Display.h"

namespace hardware::display {
    esp_err_t init(){ return CO5300::init(); }
    esp_err_t set_brightness(uint8_t brightness){ return CO5300::set_brightness(brightness); }
    esp_err_t fill_rect(uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint16_t color){ return CO5300::fill_rect(x, y, width, height, color); }
    esp_err_t draw_rgb565(uint16_t x, uint16_t y, uint16_t width, uint16_t height, const uint16_t *pixels){ return CO5300::draw_rgb565(x, y, width, height, pixels); }
    esp_err_t draw_test_pattern(){ return CO5300::draw_test_pattern(); }
    esp_err_t shutdown(){ return CO5300::shutdown(); }
}
