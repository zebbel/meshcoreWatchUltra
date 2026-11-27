#pragma once

#include "esp_err.h"

#include <stdint.h>

// UI supplies a callback that safely serializes panel operations with its
// renderer. The service owns only inactivity policy and state, not LVGL.
namespace services::display_power {
    using ScreenControl = esp_err_t (*)(bool awake);
    using ChargingDisplayControl = esp_err_t (*)(bool charging);

    esp_err_t start(ScreenControl apply_screen_awake, uint32_t timeout_ms = 5000, ChargingDisplayControl charging_display = nullptr);
    // Serialize panel transitions and enforce charging/keep-awake policy.
    esp_err_t set_screen_awake(bool awake);
    // Main task only: queue a nonblocking wake without resetting an awake display's timeout.
    void request_wrist_wake();
    // Main task fallback after light sleep: notify if a button IRQ is latched.
    void recheck_buttons();
    // Read-only event sequence numbers for EEZ; zero means no event since boot.
    int32_t boot_button_count();
    int32_t power_button_count();
    bool is_screen_awake();
    bool is_charging();
    void notify_activity();
    void set_keep_awake(bool enabled);

    // Nonblocking, RAM-only pointer samples for the UI. No hardware I/O.
    struct TouchInput {
        uint16_t x = 0, y = 0;
        bool pressed = false;
        bool more = false;
        bool reset = false; // cancel stale LVGL input before processing more samples
    };
    TouchInput read_touch_input();
}
