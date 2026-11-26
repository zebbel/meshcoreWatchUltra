#pragma once

#include "esp_err.h"

// Board-level GPIO initialization interface.
namespace hardware::gpio {
    esp_err_t init();
}
