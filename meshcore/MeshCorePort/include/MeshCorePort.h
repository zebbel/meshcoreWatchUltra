#pragma once

#include <stdint.h>
#include "esp_err.h"

namespace meshcore {
    // Preload ALL MeshCore files in a single SD session before radio init.
    esp_err_t prepare_storage();
    // Called after prepare_storage() and hardware::lora::init(). No SD I/O.
    esp_err_t init();
    void loop();

    // RX count since startup, sampled only from the app_main MeshCore task.
    uint32_t received_packet_count();
    bool get_position(int32_t &latitude_micro, int32_t &longitude_micro);
    esp_err_t set_position(int32_t latitude_micro, int32_t longitude_micro);

}
