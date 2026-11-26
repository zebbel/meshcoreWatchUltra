#pragma once

#include "esp_err.h"
#include "esp_log.h"
#include "BluetoothTransport.h"

namespace hardware{
    esp_err_t init();
    bool meshcore_enabled = false;
    auto &bluetooth_transport = hardware::bluetooth::transport();
}