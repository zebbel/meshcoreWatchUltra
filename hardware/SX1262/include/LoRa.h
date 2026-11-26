#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h" // TaskHandle_t for IRQ wakeups

#include "SX1262.h"

namespace hardware::lora {
    using config_t = hardware::sx1262::config_t;
    using packet_t = hardware::sx1262::packet_t;
    esp_err_t init();
    esp_err_t configure(const config_t &config);
    esp_err_t start_receive();
    esp_err_t poll_receive(packet_t &packet, bool &available);
    // optional main-task wakeup via LoRa DIO1 ISR.
    void set_wakeup_task(TaskHandle_t task);
    esp_err_t channel_busy(bool &busy);
    esp_err_t start_transmit(const uint8_t *data, size_t length, uint32_t timeout_ms = 10000);
    esp_err_t poll_transmit(bool &completed);
    esp_err_t finish_transmit();
    esp_err_t abort_transmit();
    bool is_in_receive_mode();
}
