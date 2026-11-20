
#include "XL9555.h"

#include "board.h"
#include "board_pins.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace XL9555 {

    static const char *TAG = "XL9555";

    static i2c_master_dev_handle_t io_handle = nullptr;
    static SemaphoreHandle_t io_mutex = nullptr;

    // XL9555 port registers
    static constexpr uint8_t REG_INPUT0    = 0x00;
    static constexpr uint8_t REG_OUTPUT0   = 0x02;
    static constexpr uint8_t REG_POLARITY0 = 0x04;
    static constexpr uint8_t REG_CONFIG0   = 0x06;

    // Low-level register access; callers must hold io_mutex after init
    static esp_err_t read_register(uint8_t reg, uint8_t &value){
        if(io_handle == nullptr){
            return ESP_ERR_INVALID_STATE;
        }

        return i2c_master_transmit_receive(io_handle, &reg, 1, &value, 1, 1000);
    }

    static esp_err_t write_register(uint8_t reg, uint8_t value){
        if(io_handle == nullptr){
            return ESP_ERR_INVALID_STATE;
        }

        uint8_t data[2] = {reg, value};
        return i2c_master_transmit(io_handle, data, sizeof(data), 1000);
    }

    static esp_err_t update_register(uint8_t reg, uint8_t mask, bool high){
        uint8_t current = 0;
        esp_err_t err = read_register(reg, current);
        if(err != ESP_OK) return err;

        uint8_t updated = high ? (current | mask) : (current & ~mask);
        if(updated == current) return ESP_OK;

        err = write_register(reg, updated);
        if(err != ESP_OK) return err;

        uint8_t verify = 0;
        err = read_register(reg, verify);
        if(err != ESP_OK) return err;

        if((verify & mask) != (updated & mask)){
            ESP_LOGE(TAG, "Register 0x%02X verification failed", reg);
            return ESP_ERR_INVALID_RESPONSE;
        }

        return ESP_OK;
    }

    static esp_err_t lock_device(){
        if(io_handle == nullptr || io_mutex == nullptr){
            return ESP_ERR_INVALID_STATE;
        }

        return xSemaphoreTake(io_mutex, portMAX_DELAY) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
    }

    static uint8_t port_register(uint8_t base, uint8_t pin){
        return base + (pin / 8);
    }

    static uint8_t pin_mask(uint8_t pin){
        return 1U << (pin % 8);
    }

    esp_err_t init(){
        if(io_handle != nullptr){
            return ESP_OK;
        }

        if(board::i2c_bus() == nullptr){
            ESP_LOGE(TAG, "Board I2C bus is not initialized");
            return ESP_ERR_INVALID_STATE;
        }

        ESP_LOGI(TAG, "Initializing XL9555");

        i2c_device_config_t config = {};
        config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        config.device_address = board::addresses::XL9555;
        config.scl_speed_hz = 100000;

        esp_err_t err = i2c_master_bus_add_device(board::i2c_bus(), &config, &io_handle);

        if(err != ESP_OK){
            ESP_LOGE(TAG, "Unable to register XL9555: %s", esp_err_to_name(err));
            return err;
        }

        io_mutex = xSemaphoreCreateMutex();

        if(io_mutex == nullptr){
            i2c_master_bus_rm_device(io_handle);
            io_handle = nullptr;
            return ESP_ERR_NO_MEM;
        }

        // Read the direction registers to verify communication without changing IO states
        uint8_t direction0 = 0;
        uint8_t direction1 = 0;
        err = read_register(REG_CONFIG0, direction0);
        if(err == ESP_OK) err = read_register(REG_CONFIG0 + 1, direction1);

        if(err != ESP_OK){
            ESP_LOGE(TAG, "XL9555 register read failed: %s", esp_err_to_name(err));
            vSemaphoreDelete(io_mutex);
            io_mutex = nullptr;
            i2c_master_bus_rm_device(io_handle);
            io_handle = nullptr;
            return err;
        }

        ESP_LOGI(TAG, "XL9555 initialized, directions P0=0x%02X P1=0x%02X", direction0, direction1);
        return ESP_OK;
    }

    esp_err_t read_pin(uint8_t pin, bool &level){
        if(pin >= 16) return ESP_ERR_INVALID_ARG;

        esp_err_t err = lock_device();
        if(err != ESP_OK) return err;

        uint8_t value = 0;
        err = read_register(port_register(REG_INPUT0, pin), value);
        if(err == ESP_OK) level = (value & pin_mask(pin)) != 0;

        xSemaphoreGive(io_mutex);
        return err;
    }

    esp_err_t set_input(uint8_t pin){
        if(pin >= 16) return ESP_ERR_INVALID_ARG;

        esp_err_t err = lock_device();
        if(err != ESP_OK) return err;

        // 1 = input, 0 = output
        err = update_register(port_register(REG_CONFIG0, pin), pin_mask(pin), true);

        xSemaphoreGive(io_mutex);
        return err;
    }

    esp_err_t set_output(uint8_t pin, bool initial_level){
        if(pin >= 16) return ESP_ERR_INVALID_ARG;

        esp_err_t err = lock_device();
        if(err != ESP_OK) return err;

        // Set the output latch before changing the pin direction
        err = update_register(port_register(REG_OUTPUT0, pin), pin_mask(pin), initial_level);
        if(err == ESP_OK) err = update_register(port_register(REG_CONFIG0, pin), pin_mask(pin), false);

        xSemaphoreGive(io_mutex);
        return err;
    }

    esp_err_t write_pin(uint8_t pin, bool level){
        if(pin >= 16) return ESP_ERR_INVALID_ARG;

        esp_err_t err = lock_device();
        if(err != ESP_OK) return err;

        uint8_t direction = 0;
        err = read_register(port_register(REG_CONFIG0, pin), direction);

        if(err == ESP_OK){
            if((direction & pin_mask(pin)) != 0){
                err = ESP_ERR_INVALID_STATE;
            }else{
                err = update_register(port_register(REG_OUTPUT0, pin), pin_mask(pin), level);
            }
        }

        xSemaphoreGive(io_mutex);
        return err;
    }

    esp_err_t read_state(state_t &state){
        esp_err_t err = lock_device();
        if(err != ESP_OK) return err;

        uint8_t registers[8] = {};

        for(uint8_t reg = 0; reg < sizeof(registers); reg++){
            err = read_register(reg, registers[reg]);
            if(err != ESP_OK) break;
        }

        xSemaphoreGive(io_mutex);
        if(err != ESP_OK) return err;

        state.input0 = registers[0];
        state.input1 = registers[1];
        state.output0 = registers[2];
        state.output1 = registers[3];
        state.polarity0 = registers[4];
        state.polarity1 = registers[5];
        state.direction0 = registers[6];
        state.direction1 = registers[7];

        return ESP_OK;
    }

    esp_err_t log_state(){
        state_t state;
        esp_err_t err = read_state(state);

        if(err != ESP_OK){
            ESP_LOGE(TAG, "Unable to read XL9555 state: %s", esp_err_to_name(err));
            return err;
        }

        ESP_LOGI(TAG, "Port 0: IN=0x%02X OUT=0x%02X INV=0x%02X DIR=0x%02X", state.input0, state.output0, state.polarity0, state.direction0);
        ESP_LOGI(TAG, "Port 1: IN=0x%02X OUT=0x%02X INV=0x%02X DIR=0x%02X", state.input1, state.output1, state.polarity1, state.direction1);

        // SD detection is active-low; undo input inversion before interpreting the pin
        uint8_t sd_bit = pin_mask(board::expander::SD_DETECT);
        bool sd_high = ((state.input1 ^ state.polarity1) & sd_bit) != 0;
        bool sd_input = (state.direction1 & sd_bit) != 0;

        if(sd_input){
            ESP_LOGI(TAG, "SD card: %s", sd_high ? "Not inserted" : "Inserted");
        }else{
            ESP_LOGW(TAG, "SD detect pin is configured as an output");
        }

        ESP_LOGI(TAG, "XL9555 state read complete");
        return ESP_OK;
    }

    // Enable or disable the haptic driver
    esp_err_t set_haptic_enabled(bool enabled){
        esp_err_t err = set_output(board::expander::HAPTIC_EN, enabled);

        if(err != ESP_OK){
            ESP_LOGE(TAG, "Haptic enable configuration failed: %s", esp_err_to_name(err));
            return err;
        }

        ESP_LOGI(TAG, "Haptic driver: %s", enabled ? "Enabled" : "Disabled");
        return ESP_OK;
    }

    // Control the display enable signal
    esp_err_t set_display_enabled(bool enabled){
        esp_err_t err = set_output(board::expander::DISPLAY_EN, enabled);

        if(err != ESP_OK){
            ESP_LOGE(TAG, "Display enable configuration failed: %s", esp_err_to_name(err));
            return err;
        }

        ESP_LOGI(TAG, "Display enable: %s", enabled ? "On" : "Off");
        return ESP_OK;
    }

    // Assert or release the touchscreen reset signal
    esp_err_t set_touch_reset(bool released){
        esp_err_t err = set_output(board::expander::TOUCH_RST, released);

        if(err != ESP_OK){
            ESP_LOGE(TAG, "Touch reset configuration failed: %s", esp_err_to_name(err));
            return err;
        }

        ESP_LOGI(TAG, "Touch reset: %s", released ? "Released" : "Asserted");
        return ESP_OK;
    }

    // Select the external USB RF path or the built-in antenna
    esp_err_t set_lora_antenna(bool external){
        // LOW selects USB RF, HIGH selects the built-in antenna
        esp_err_t err = set_output(board::expander::LORA_RF_SW, !external);

        if(err != ESP_OK){
            ESP_LOGE(TAG, "LoRa antenna configuration failed: %s", esp_err_to_name(err));
            return err;
        }

        ESP_LOGI(TAG, "LoRa antenna: %s", external ? "USB RF" : "Built-in");
        return ESP_OK;
    }

    // Read the active-low SD card detection input
    esp_err_t is_sd_inserted(bool &inserted){
        state_t state;
        esp_err_t err = read_state(state);
        if(err != ESP_OK) return err;

        uint8_t mask = pin_mask(board::expander::SD_DETECT);

        if((state.direction1 & mask) == 0){
            ESP_LOGE(TAG, "SD detect pin is not configured as input");
            return ESP_ERR_INVALID_STATE;
        }

        // Account for the input polarity inversion register
        bool high = ((state.input1 ^ state.polarity1) & mask) != 0;
        inserted = !high;

        return ESP_OK;
    }

    // Configure the initial GPIO expander state
    esp_err_t configure(){
        if(io_handle == nullptr){
            return ESP_ERR_INVALID_STATE;
        }

        ESP_LOGI(TAG, "Configuring XL9555");

        // Disable haptic driver at startup
        esp_err_t err = set_haptic_enabled(false);
        if(err != ESP_OK) return err;

        // Configure SD card detection as an input
        err = set_input(board::expander::SD_DETECT);
        if(err != ESP_OK) return err;

        ESP_LOGI(TAG, "SD card detection configured as input");
        ESP_LOGI(TAG, "XL9555 configuration complete");
        return ESP_OK;
    }

}
