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
// Native EEZ setter runs during ui_init/ui_tick with the adapter lock held.
// EEZ owns brightness policy. No nested lock or recursive Flow tick.
esp_err_t set_ui_brightness_locked(uint8_t percentage);
// Select the charging-only page presentation, with the LVGL lock held.
void set_clock_charging_mode(bool charging);
}
