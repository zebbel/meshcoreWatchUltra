#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdint.h>

// Sole owner of the CST9217 I2C connection, GPIO12 IRQ and 0xD000AB ACK.
// This component never calls LVGL and never decides when the display sleeps.
namespace hardware::cst9217 {
    struct TouchPoint {
        uint16_t x = 0;
        uint16_t y = 0;
        bool pressed = false;
        bool report_valid = false; // false for an IRQ without a valid coordinate frame
    };

    esp_err_t init();
    // Display policy task notification bits (not a counting semaphore).
    constexpr uint32_t kTouchIrqNotification = 1U << 0;
    void set_notification_task(TaskHandle_t task);
    // Main task only: after RTC pad restoration, recheck a latched IRQ level.
    void recheck_pending_irq();
    esp_err_t read_interrupt(TouchPoint &point);
    bool ready();
}
