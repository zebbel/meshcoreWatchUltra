#pragma once

#include "esp_err.h"

namespace hardware::gpio {
    esp_err_t init();
    esp_err_t configure();
}
