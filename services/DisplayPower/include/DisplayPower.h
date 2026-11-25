#pragma once

#include <stdint.h>
#include "esp_err.h"

// UI supplies a callback that safely serializes panel operations with its
// renderer. The service owns only inactivity policy and state, not LVGL.
namespace services::display_power {
    using ScreenControl = esp_err_t (*)(bool awake);
    using ChargingDisplayControl = esp_err_t (*)(bool charging);

    esp_err_t start(ScreenControl apply_screen_awake, uint32_t timeout_ms = 5000,
                    ChargingDisplayControl charging_display = nullptr);
    esp_err_t set_screen_awake(bool awake);
    bool is_screen_awake();
    bool is_charging();
    void notify_activity();

    // Nonblocking, RAM-only pointer samples for the UI. No hardware I/O.
    struct TouchInput {
        uint16_t x = 0, y = 0;
        bool pressed = false;
        bool more = false;
        bool reset = false; // cancel stale LVGL input before processing more samples
    };
    TouchInput read_touch_input();
}
