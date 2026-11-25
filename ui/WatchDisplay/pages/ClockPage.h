#pragma once
#include "esp_err.h"
#include <stdint.h>

namespace ui::watch_display {
// Called under the LVGL adapter lock, after the display is registered.
// Owns only UI objects/timers; hardware access uses existing service APIs.
esp_err_t render_clock_page();
// Refresh battery and charging state before revealing the panel; LVGL lock held.
void refresh_clock_battery();
void publish_ui_brightness(uint8_t percent);
// Select the charging-only page presentation, with the LVGL lock held.
void set_clock_charging_mode(bool charging);
}
