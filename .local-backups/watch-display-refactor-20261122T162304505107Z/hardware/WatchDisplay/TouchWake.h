#pragma once

#include "esp_err.h"

namespace hardware::watch_display::touch_wake {
    // Begins CST9217 polling and manages the five-second panel-off timeout.
    // Runs separately from app_main so MeshCore/GPS/BLE are not delayed.
    esp_err_t start();
}
