#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

namespace hardware::system {
    int64_t micros();
    uint32_t millis();
    void random_bytes(uint8_t *out, size_t length);
    void enable_boot_random();
    void disable_boot_random();
    void restart();
    // Epoch seconds in UTC; returns zero until a trusted time is available.
    esp_err_t initialize_utc_clock();
    uint32_t utc_time();
    esp_err_t set_utc_time(uint32_t epoch_seconds);  // Companion / manual time
    esp_err_t sync_gps_utc(uint32_t epoch_seconds); // Valid RMC only; throttled
}
