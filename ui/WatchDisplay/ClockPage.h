#pragma once

namespace ui::watch_display {
// Called under the LVGL adapter lock, after the display is registered.
// Owns only UI objects/timers; hardware access uses existing service APIs.
void render_clock_page();
}
