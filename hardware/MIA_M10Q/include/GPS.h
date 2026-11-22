#pragma once

#include "MIA_M10Q.h"

namespace hardware::gps {
    using fix_t = MIA_M10Q::fix_t;
    using status_t = MIA_M10Q::status_t;
    esp_err_t init();
    void loop();
    bool get_fresh_fix(fix_t &fix);
    void request_fix();
    void get_status(status_t &status);
    bool is_enabled();
    esp_err_t set_enabled(bool enabled);
}
