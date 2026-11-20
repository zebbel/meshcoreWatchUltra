
#pragma once

#include <stdint.h>
#include "esp_err.h"

namespace XL9555 {

    struct state_t {
        uint8_t input0 = 0;
        uint8_t input1 = 0;
        uint8_t output0 = 0;
        uint8_t output1 = 0;
        uint8_t polarity0 = 0;
        uint8_t polarity1 = 0;
        uint8_t direction0 = 0;
        uint8_t direction1 = 0;
    };

    esp_err_t init();                                                                                   // Register the expander on the shared I2C bus without changing its pins
    esp_err_t configure();                                                                              // Apply safe GPIO expander startup configuration
    esp_err_t set_input(uint8_t pin);                                                                   // Configure a pin as an input
    esp_err_t set_output(uint8_t pin, bool initial_level);                                              // Set the output latch before enabling output mode to avoid glitches
    esp_err_t read_pin(uint8_t pin, bool &level);                                                       // Read the current level of a pin (linear indices 0-15)
    esp_err_t write_pin(uint8_t pin, bool level);                                                       // Change an existing output pin; returns an error if it is an input
    esp_err_t read_state(state_t &state);                                                               // Read all port registers without changing their configuration
    esp_err_t set_haptic_enabled(bool enabled);                                                         // Enable or disable the haptic driver
    esp_err_t set_display_enabled(bool enabled);                                                        // Control the display enable signal
    esp_err_t set_touch_reset(bool released);                                                           // Release touchscreen reset when true, assert reset when false
    esp_err_t set_lora_antenna(bool external);                                                          // Select external USB RF path or built-in LoRa antenna
    esp_err_t is_sd_inserted(bool &inserted);                                                           // Read SD card detection, active-low

}
