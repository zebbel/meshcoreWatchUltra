
#pragma once

#include <stdint.h>
#include "esp_err.h"

namespace app::ui {
    esp_err_t init();
    void show_pairing_code(uint32_t six_digit_pin);
    void clear_pairing_code();
}
