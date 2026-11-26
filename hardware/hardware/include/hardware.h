#pragma once

#include "esp_err.h"

// Central hardware lifecycle and application-task polling.
namespace hardware {
    esp_err_t init();
    void loop();
}
