#pragma once

#include <stdint.h>
#include "esp_err.h"

namespace meshcore {
    esp_err_t init();
    void loop();

    // Read-only compatibility access for persisted BLE PIN; lifecycle belongs to Main.
    uint32_t legacy_ble_pin();
    bool get_position(int32_t &latitude_micro, int32_t &longitude_micro);
    esp_err_t set_position(int32_t latitude_micro, int32_t longitude_micro);

}
