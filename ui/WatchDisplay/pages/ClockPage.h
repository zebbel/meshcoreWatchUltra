#pragma once

namespace ui::watch_display {
// Called under the LVGL adapter lock, after the display is registered.
// Owns only UI objects/timers; hardware access uses existing service APIs.
void render_clock_page();
// Refresh battery and charging state before revealing the panel; LVGL lock held.
void refresh_clock_battery();
// Select the charging-only page presentation, with the LVGL lock held.
void set_clock_charging_mode(bool charging);
}
