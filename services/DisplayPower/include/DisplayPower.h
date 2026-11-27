#pragma once

#include "esp_err.h"

#include <stdint.h>

// UI supplies a callback that safely serializes panel operations with its
// renderer. EEZ owns inactivity and page policy; this service executes hardware transitions.
namespace services::display_power {
    using ScreenControl = esp_err_t (*)(bool awake);

    esp_err_t start(ScreenControl apply_screen_awake);
    // Task-context panel transition; never call from the LVGL worker.
    esp_err_t set_screen_awake(bool awake);
    // Main task: queue a wrist event without touching the renderer.
    void request_wrist_wake();
    // Main task fallback after light sleep: notify if a button IRQ is latched.
    void recheck_buttons();
    // Read-only event sequence numbers for EEZ; zero means no event since boot.
    int32_t boot_button_count();
    int32_t power_button_count();
    bool is_screen_awake();
    bool is_charging();
    int32_t touch_count();
    int32_t wrist_count();
    int32_t display_wake_count();
    // Asynchronous command interface for EEZ; false cancels a queued request.
    bool sleep_request_pending();
    void request_sleep(bool requested);
    void request_external_wake();

    // Nonblocking, RAM-only pointer samples for the UI. No hardware I/O.
    struct TouchInput {
        uint16_t x = 0, y = 0;
        bool pressed = false;
        bool more = false;
        bool reset = false; // cancel stale LVGL input before processing more samples
    };
    TouchInput read_touch_input();
}
