#pragma once

#include "esp_err.h"
#include <stdint.h>

namespace services::notifications {
    enum class ChannelMode : uint8_t { disabled = 0, enabled = 1, mentions_only = 2 };

    // Application task only. UI adapters must marshal writes to this task.
    ChannelMode channel_mode();
    esp_err_t set_channel_mode(ChannelMode mode);
    bool private_enabled();
    esp_err_t set_private_enabled(bool enabled);
    void loop();
}
