#pragma once

#include "MIA_M10Q.h"

// GPS interface used by application services.
namespace hardware::gps {
    using fix_t = hardware::mia_m10q::fix_t;
    using status_t = hardware::mia_m10q::status_t;
    using UtcCallback = hardware::mia_m10q::UtcCallback;
    void set_utc_callback(UtcCallback callback);
    esp_err_t init();
    void loop();
    bool get_fresh_fix(fix_t &fix);
    void request_fix();
    void get_status(status_t &status);
    bool is_enabled();
    esp_err_t set_enabled(bool enabled);
}
