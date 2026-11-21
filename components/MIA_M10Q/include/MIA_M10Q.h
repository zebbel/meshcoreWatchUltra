
#pragma once

#include <stdint.h>
#include "esp_err.h"

namespace MIA_M10Q {

    struct fix_t {
        double latitude = 0;
        double longitude = 0;
        double altitude = 0;
        uint32_t updated_ms = 0;
        bool valid = false;
    };

    esp_err_t init();
    void loop();
    bool get_fresh_fix(fix_t &fix);
    void request_fix();

}
