
#pragma once

#include <stdint.h>
#include "esp_err.h"

namespace watch {
    esp_err_t pairing_display_init();
    void pairing_display_show(uint32_t six_digit_pin);
    void pairing_display_clear();
}
