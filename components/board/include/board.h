
#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"

namespace board {
    // Initialize shared hardware buses
    esp_err_t init();

    // Get initialized I2C bus
    i2c_master_bus_handle_t i2c_bus();

    // Get SPI host identifier
    spi_host_device_t spi_host();
}
