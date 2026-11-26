#pragma once
#include "esp_err.h"

namespace watch::display {
// All runtime calls are serialized by the LVGL adapter lock.
esp_err_t init_flow();
// Refresh native values and settle pending actions before a forced redraw.
void refresh_flow(bool charging);
}
