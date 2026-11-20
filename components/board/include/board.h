
#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"

namespace board {
    esp_err_t init();                                                       // Initialize shared hardware buses
    i2c_master_bus_handle_t i2c_bus();                                      // Get initialized I2C bus
    spi_host_device_t spi_host();                                           // Get SPI host identifier
}
