#pragma once

#include <stdint.h>
#include "esp_err.h"

namespace meshcore {

    struct PairingEvent {
        uint32_t pin = 0;
        bool clear = false;
    };

    esp_err_t init();
    void loop();

    PairingEvent take_pairing_event();
    bool get_position(int32_t &latitude_micro, int32_t &longitude_micro);
    esp_err_t set_position(int32_t latitude_micro, int32_t longitude_micro);

}
