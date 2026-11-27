#pragma once

#include "esp_err.h"

#include <stddef.h>
#include <stdint.h>

namespace hardware::drv2605 {
    // All operations belong to the application task, not ISR or LVGL context.
    esp_err_t init();
    esp_err_t configure();
    esp_err_t shutdown();

    // ROM effects 1-117; up to eight effects per sequence. No blocking playback.
    esp_err_t play_effect(uint8_t effect);
    esp_err_t play_sequence(const uint8_t *effects, size_t count);
    esp_err_t stop();

    bool ready();
    bool busy();
    void loop();
}
