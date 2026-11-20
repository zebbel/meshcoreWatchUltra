
#pragma once

#include <stdint.h>
#include "driver/gpio.h"

namespace board {

    namespace pins {

        // Shared I2C bus
        // AXP2101, XL9555, CST9217, BHI260AP, PCF85063A, DRV2605
        constexpr gpio_num_t I2C_SDA = GPIO_NUM_3;
        constexpr gpio_num_t I2C_SCL = GPIO_NUM_2;

        // Shared SPI bus
        // LoRa, SD card, NFC
        constexpr gpio_num_t SPI_MOSI = GPIO_NUM_34;
        constexpr gpio_num_t SPI_MISO = GPIO_NUM_33;
        constexpr gpio_num_t SPI_SCK  = GPIO_NUM_35;

        // SX1262 LoRa radio
        constexpr gpio_num_t LORA_CS   = GPIO_NUM_36;
        constexpr gpio_num_t LORA_RST  = GPIO_NUM_47;
        constexpr gpio_num_t LORA_BUSY = GPIO_NUM_48;
        constexpr gpio_num_t LORA_DIO1 = GPIO_NUM_14;

        // MicroSD card
        // Uses shared SPI bus
        constexpr gpio_num_t SD_CS = GPIO_NUM_21;

        // CO5300 AMOLED display
        // Dedicated QSPI interface
        constexpr gpio_num_t DISPLAY_CS    = GPIO_NUM_41;
        constexpr gpio_num_t DISPLAY_SCK   = GPIO_NUM_40;
        constexpr gpio_num_t DISPLAY_D0    = GPIO_NUM_38;
        constexpr gpio_num_t DISPLAY_D1    = GPIO_NUM_39;
        constexpr gpio_num_t DISPLAY_D2    = GPIO_NUM_42;
        constexpr gpio_num_t DISPLAY_D3    = GPIO_NUM_45;
        constexpr gpio_num_t DISPLAY_RST   = GPIO_NUM_37;
        constexpr gpio_num_t DISPLAY_TE    = GPIO_NUM_6;

        // CST9217 capacitive touchscreen
        // Uses shared I2C bus
        constexpr gpio_num_t TOUCH_IRQ = GPIO_NUM_12;

        // AXP2101 power management
        // Uses shared I2C bus
        constexpr gpio_num_t PMU_IRQ = GPIO_NUM_7;

        // PCF85063A real-time clock
        // Uses shared I2C bus
        constexpr gpio_num_t RTC_IRQ = GPIO_NUM_1;

        // BHI260AP motion sensor
        // Uses shared I2C bus
        constexpr gpio_num_t SENSOR_IRQ = GPIO_NUM_8;

        // ST25R3916 NFC controller
        // Uses shared SPI bus
        constexpr gpio_num_t NFC_CS  = GPIO_NUM_4;
        constexpr gpio_num_t NFC_IRQ = GPIO_NUM_5;

        // MIA-M10Q GNSS receiver
        // UART pin directions from ESP32-S3
        constexpr gpio_num_t GPS_TX  = GPIO_NUM_43;
        constexpr gpio_num_t GPS_RX  = GPIO_NUM_44;
        constexpr gpio_num_t GPS_PPS = GPIO_NUM_13;

        // T3902 PDM microphone
        constexpr gpio_num_t MIC_CLK  = GPIO_NUM_17;
        constexpr gpio_num_t MIC_DATA = GPIO_NUM_18;

        // MAX98357A I2S speaker amplifier
        constexpr gpio_num_t I2S_BCLK = GPIO_NUM_9;
        constexpr gpio_num_t I2S_WCLK = GPIO_NUM_10;
        constexpr gpio_num_t I2S_DOUT = GPIO_NUM_11;

        // User button
        // GPIO0 is also the ESP32-S3 boot-mode pin
        constexpr gpio_num_t BOOT_BUTTON = GPIO_NUM_0;

        // ESP32-S3 native USB
        // Reserved for USB functionality
        constexpr gpio_num_t USB_DM = GPIO_NUM_19;
        constexpr gpio_num_t USB_DP = GPIO_NUM_20;

    }

    namespace addresses {

        // Shared I2C bus device addresses (7-bit)
        constexpr uint8_t TOUCH   = 0x1A; // CST9217
        constexpr uint8_t XL9555  = 0x20; // GPIO expander
        constexpr uint8_t SENSOR  = 0x28; // BHI260AP
        constexpr uint8_t AXP2101 = 0x34; // Power management
        constexpr uint8_t RTC     = 0x51; // PCF85063A
        constexpr uint8_t HAPTIC  = 0x5A; // DRV2605

    }

    namespace expander {

        // XL9555 GPIO expander (I2C address 0x20)
        // Values are linear 0-15 pin indices, NOT ESP32 GPIOs

        // Port 0
        constexpr uint8_t HAPTIC_EN  = 6;  // P0.6
        constexpr uint8_t DISPLAY_EN = 7;  // P0.7

        // Port 1
        constexpr uint8_t TOUCH_RST  = 8;  // P1.0
        constexpr uint8_t SD_DETECT  = 10; // P1.2
        constexpr uint8_t LORA_RF_SW = 11; // P1.3

    }

    namespace hardware {

        // External memory
        constexpr uint32_t FLASH_SIZE = 16 * 1024 * 1024;
        constexpr uint32_t PSRAM_SIZE = 8 * 1024 * 1024;

        // CO5300 AMOLED display
        constexpr uint16_t DISPLAY_WIDTH  = 502;
        constexpr uint16_t DISPLAY_HEIGHT = 410;

        // GNSS UART default baud rate
        constexpr uint32_t GPS_BAUD = 38400;

        // AXP2101 power rails
        //
        // DCDC1 : ESP32-S3
        // ALDO1 : MicroSD card
        // ALDO2 : AMOLED display
        // ALDO3 : LoRa radio
        // ALDO4 : Motion sensor
        // BLDO1 : GNSS
        // BLDO2 : Speaker amplifier
        // DLDO1 : NFC controller
        // VRTC  : GNSS backup
        //
        // Power button is managed by AXP2101.
        // The hardware reset button is not programmable.
        // Flash and PSRAM use the dedicated memory interface.

    }

}
