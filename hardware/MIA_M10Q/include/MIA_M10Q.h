
#pragma once

#include "esp_err.h"

#include <stdint.h>

namespace hardware::mia_m10q {

    struct fix_t {
        double latitude = 0;
        double longitude = 0;
        double altitude = 0;
        uint32_t updated_ms = 0;
        bool valid = false;
    };

    struct status_t {
        bool initialized = false;
        bool powered = false;
        bool has_gga = false;
        uint32_t gga_count = 0;
        uint32_t last_gga_ms = 0;
        uint8_t fix_quality = 0;
        uint8_t satellites = 0;
        float hdop = 0;
        char utc[7] = {};
    };

    // Invoked on the existing GPS polling task for valid RMC UTC.
    using UtcCallback = esp_err_t (*)(uint32_t);
    void set_utc_callback(UtcCallback callback);
    bool is_enabled();
    esp_err_t set_enabled(bool enabled);

    esp_err_t init();
    void loop();
    bool get_fresh_fix(fix_t &fix);
    void request_fix();
    void get_status(status_t &status);
}
