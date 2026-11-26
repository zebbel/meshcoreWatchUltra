#pragma once

#include <stdint.h>
#include "esp_err.h"

namespace meshcore {
    // Load persistent storage, initialize LoRa and start MeshCore.
    esp_err_t init();
    void loop();
    // Main application task only.
    bool is_initialized();

    // RX count since startup, sampled only from the app_main MeshCore task.
    uint32_t received_packet_count();
    bool get_position(int32_t &latitude_micro, int32_t &longitude_micro);
    esp_err_t set_position(int32_t latitude_micro, int32_t longitude_micro);

}
