#pragma once

#include "esp_err.h"

#include <stdint.h>
#include <stddef.h>

namespace meshcore {
    // Load persistent storage, initialize LoRa and start MeshCore.
    esp_err_t init();
    void loop();
    // Main application task only.
    bool is_initialized();

    // Validated incoming plain text, independent of BLE; borrowed data for callback only.
    // Application task only. Channel text includes the sender prefix.
    using MessageCallback = void (*)(bool channel, const char *local_name, const uint8_t *text, size_t length);
    bool set_message_callback(MessageCallback callback);

    // RX count since startup, sampled only from the app_main MeshCore task.
    uint32_t received_packet_count();
    bool get_position(int32_t &latitude_micro, int32_t &longitude_micro);
    esp_err_t set_position(int32_t latitude_micro, int32_t longitude_micro);

}
