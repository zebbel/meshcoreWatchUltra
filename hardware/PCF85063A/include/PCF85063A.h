#pragma once

#include "esp_err.h"

#include <stdint.h>

namespace hardware::rtc {
    // The PCF85063A keeps UTC while its supply remains powered.
    // Read rejects an unset/oscillator-stopped calendar.
    esp_err_t init();
    esp_err_t read_utc(uint32_t &unix_seconds);
    esp_err_t write_utc(uint32_t unix_seconds);
}
