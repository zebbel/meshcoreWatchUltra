
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h" // TaskHandle_t for IRQ wakeups

namespace sx1262 {

    struct config_t {
        uint32_t frequency_hz = 0;
        uint32_t bandwidth_hz = 250000;
        uint8_t spreading_factor = 11;
        uint8_t coding_rate = 5;
        uint8_t sync_word = 0x12;
        uint16_t preamble_symbols = 8;
        int8_t tx_power_dbm = 10;
        bool crc_enabled = true;
    };

    struct status_t {
        uint8_t raw = 0;
        uint8_t mode = 0;
        uint8_t command_status = 0;
        uint16_t device_errors = 0;
        bool busy = false;
        bool dio1 = false;
    };

    struct packet_t {
        uint8_t data[255] = {};
        uint8_t length = 0;
        float rssi_dbm = 0;
        float snr_db = 0;
    };

    // Hardware initialization and radio configuration
    esp_err_t init();
    esp_err_t configure(const config_t &config);
    esp_err_t read_status(status_t &status);
    esp_err_t shutdown();

    // Nonblocking operations for MeshCore integration
    esp_err_t start_receive();
    esp_err_t poll_receive(packet_t &packet, bool &available);
    // optional main-task wakeup via LoRa DIO1 ISR.
    void set_wakeup_task(TaskHandle_t task);

    // Check LoRa channel activity using SX1262 CAD.
    // busy is true when a signal is detected or the check cannot complete.
    esp_err_t channel_busy(bool &busy);

    esp_err_t start_transmit(const uint8_t *data, size_t length, uint32_t timeout_ms = 10000);
    esp_err_t poll_transmit(bool &completed);
    esp_err_t finish_transmit();
    esp_err_t abort_transmit();
    bool is_in_receive_mode();
    esp_err_t poll_transmit(bool &completed);
    esp_err_t finish_transmit();
    esp_err_t abort_transmit();
    bool is_in_receive_mode();

    // Blocking operations retained for hardware tests
    esp_err_t receive(packet_t &packet, uint32_t timeout_ms);
    esp_err_t transmit(const uint8_t *data, size_t length, uint32_t timeout_ms = 10000);

}
