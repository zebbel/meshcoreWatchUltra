
#pragma once

#include <stdint.h>
#include "driver/gpio.h"

namespace board {
    namespace pins {
        // Shared I2C bus
        constexpr gpio_num_t I2C_SDA = GPIO_NUM_3;
        constexpr gpio_num_t I2C_SCL = GPIO_NUM_2;

        // Shared SPI bus: LoRa, SD card, NFC
        constexpr gpio_num_t SPI_MOSI = GPIO_NUM_34;
        constexpr gpio_num_t SPI_MISO = GPIO_NUM_33;
        constexpr gpio_num_t SPI_SCK  = GPIO_NUM_35;

        // SX1262 LoRa
        constexpr gpio_num_t LORA_CS   = GPIO_NUM_36;
        constexpr gpio_num_t LORA_RST  = GPIO_NUM_47;
        constexpr gpio_num_t LORA_BUSY = GPIO_NUM_48;
        constexpr gpio_num_t LORA_DIO1 = GPIO_NUM_14;

        // Power management
        constexpr gpio_num_t PMU_IRQ = GPIO_NUM_7;

        // User button
        constexpr gpio_num_t BOOT_BUTTON = GPIO_NUM_0;
    }

    namespace addresses {
        // I2C device addresses
        constexpr uint8_t AXP2101 = 0x34;
        constexpr uint8_t XL9555  = 0x20;
        constexpr uint8_t RTC     = 0x51;
    }
}
