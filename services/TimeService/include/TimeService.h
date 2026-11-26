#pragma once
#include "esp_err.h"

#include <stdint.h>

namespace services::time_service {
    // Epoch seconds in UTC; returns zero until a trusted time is available.
    esp_err_t initialize_utc_clock();
    uint32_t utc_time();
    esp_err_t set_utc_time(uint32_t epoch_seconds);  // Companion / manual time
    esp_err_t sync_gps_utc(uint32_t epoch_seconds); // Valid RMC only; throttled
}
