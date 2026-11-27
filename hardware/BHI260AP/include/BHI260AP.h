#pragma once

#include "esp_err.h"

#include <stdint.h>

namespace hardware::bhi260ap {

    struct status_t {
        bool ready = false;
        bool steps_available = false;
        bool wrist_tilt_available = false;
        bool motion_available = false;
        bool step_count_valid = false;
        uint32_t steps = 0;
        uint16_t kernel_version = 0;
    };

    // All operations belong to the main application task. The GPIO ISR only signals pending data.
    esp_err_t init();
    esp_err_t shutdown();
    void loop();
    bool ready();
    status_t get_status();
    uint32_t step_count();

    // Consume a pending gesture once; no display or GPS action is performed here.
    bool wrist_tilt_detected();
    bool motion_detected();
}
