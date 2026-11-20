
#include "AXP2101.h"

#include "board.h"
#include "board_pins.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "sdkconfig.h"

namespace AXP2101 {

    static const char *TAG = "AXP2101";

    static i2c_master_dev_handle_t pmu_handle = nullptr;

    // AXP2101 register addresses
    static constexpr uint8_t REG_STATUS1    = 0x00;
    static constexpr uint8_t REG_STATUS2    = 0x01;
    static constexpr uint8_t REG_CHIP_ID    = 0x03;
    static constexpr uint8_t REG_ADC_CTRL   = 0x30;
    static constexpr uint8_t REG_BATTERY_V  = 0x34;
    static constexpr uint8_t REG_VBUS_V     = 0x38;
    static constexpr uint8_t REG_LDO_ENABLE = 0x90;
    static constexpr uint8_t REG_BATTERY_PC = 0xA4;

    static constexpr uint8_t CHIP_ID = 0x4A;

    // Read one or more consecutive AXP2101 registers
    static esp_err_t read_register(uint8_t reg, uint8_t *data, size_t len){
        if(pmu_handle == nullptr){
            return ESP_ERR_INVALID_STATE;
        }

        return i2c_master_transmit_receive(pmu_handle, &reg, 1, data, len, 1000);
    }

    esp_err_t init(){
        if(pmu_handle != nullptr){
            return ESP_OK;
        }

        if(board::i2c_bus() == nullptr){
            ESP_LOGE(TAG, "Board I2C bus is not initialized");
            return ESP_ERR_INVALID_STATE;
        }

        ESP_LOGI(TAG, "Initializing AXP2101");

        i2c_device_config_t config = {};
        config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        config.device_address = board::addresses::AXP2101;
        config.scl_speed_hz = 100000;

        esp_err_t err = i2c_master_bus_add_device(board::i2c_bus(), &config, &pmu_handle);

        if(err != ESP_OK){
            ESP_LOGE(TAG, "Unable to register AXP2101: %s", esp_err_to_name(err));
            return err;
        }

        // Verify that this is an AXP2101
        uint8_t chip_id = 0;
        err = read_register(REG_CHIP_ID, &chip_id, 1);

        if(err != ESP_OK || chip_id != CHIP_ID){
            ESP_LOGE(TAG, "AXP2101 identification failed");

            i2c_master_bus_rm_device(pmu_handle);
            pmu_handle = nullptr;

            return err != ESP_OK ? err : ESP_ERR_INVALID_RESPONSE;
        }

        ESP_LOGI(TAG, "AXP2101 detected, chip ID: 0x%02X", chip_id);
        return ESP_OK;
    }

    // Write one AXP2101 register
    static esp_err_t write_axp_register(uint8_t reg, uint8_t value){
        if(pmu_handle == nullptr){
            return ESP_ERR_INVALID_STATE;
        }

        uint8_t data[2] = {reg, value};
        return i2c_master_transmit(pmu_handle, data, sizeof(data), 1000);
    }

    // Update selected register bits and verify the result
    static esp_err_t update_axp_register(uint8_t reg, uint8_t mask, uint8_t value){
        uint8_t current = 0;

        esp_err_t err = read_register(reg, &current, 1);
        if(err != ESP_OK) return err;

        uint8_t updated = (current & ~mask) | (value & mask);

        if(updated == current){
            return ESP_OK;
        }

        err = write_axp_register(reg, updated);
        if(err != ESP_OK) return err;

        uint8_t verify = 0;
        err = read_register(reg, &verify, 1);
        if(err != ESP_OK) return err;

        if((verify & mask) != (updated & mask)){
            ESP_LOGE(TAG, "AXP2101 register 0x%02X verification failed", reg);
            return ESP_ERR_INVALID_RESPONSE;
        }

        return ESP_OK;
    }

    // Configure an LDO voltage without changing unrelated register bits
    static esp_err_t set_voltage_reg(uint8_t reg, uint16_t millivolts){
        if(millivolts < 500 || millivolts > 3500 || (millivolts - 500) % 100 != 0){
            return ESP_ERR_INVALID_ARG;
        }

        uint8_t code = (millivolts - 500) / 100;
        return update_axp_register(reg, 0x1F, code);
    }

    // Switch one peripheral supply rail
    esp_err_t set_enabled(rail_t rail, bool enabled){
        uint8_t bit = static_cast<uint8_t>(rail);

        if(bit > 7 || bit == 6){
            return ESP_ERR_INVALID_ARG;
        }

        uint8_t mask = 1 << bit;

        esp_err_t err = update_axp_register(0x90, mask, enabled ? mask : 0);

        if(err != ESP_OK){
            ESP_LOGE(TAG, "Unable to change rail %u: %s", bit, esp_err_to_name(err));
            return err;
        }

        ESP_LOGI(TAG, "Power rail %u: %s", bit, enabled ? "On" : "Off");
        return ESP_OK;
    }

    esp_err_t enable(rail_t rail){
        return set_enabled(rail, true);
    }

    esp_err_t disable(rail_t rail){
        return set_enabled(rail, false);
    }

    // Apply verified hardware voltage assignments
    static esp_err_t configure_voltages(){
        struct voltage_t {
            uint8_t reg;
            uint16_t millivolts;
        };

        static constexpr voltage_t voltages[] = {
            {0x92, 3300}, // ALDO1 SD
            {0x93, 3300}, // ALDO2 Display
            {0x94, 3300}, // ALDO3 Radio
            {0x95, 1800}, // ALDO4 Sensor
            {0x96, 3300}, // BLDO1 GPS
            {0x97, 3300}, // BLDO2 Speaker
            {0x99, 3300}  // DLDO1 NFC
        };

        for(const auto &entry : voltages){
            esp_err_t err = set_voltage_reg(entry.reg, entry.millivolts);

            if(err != ESP_OK){
                ESP_LOGE(TAG, "Voltage configuration failed at 0x%02X", entry.reg);
                return err;
            }
        }

        ESP_LOGI(TAG, "Peripheral regulator voltages configured");
        return ESP_OK;
    }

    // Apply selected charger settings
    static esp_err_t configure_charger(){
#if defined(CONFIG_MCW_POWER_APPLY_CHARGER)

        // Validate the user-entered current and voltage steps
        if(CONFIG_MCW_POWER_PRECHARGE_MA % 25 != 0 || CONFIG_MCW_POWER_TERMINATION_MA % 25 != 0){
            ESP_LOGE(TAG, "Charging currents must use 25 mA steps");
            return ESP_ERR_INVALID_ARG;
        }

        if((CONFIG_MCW_POWER_USB_VOLTAGE_MV - 3880) % 80 != 0){
            ESP_LOGE(TAG, "USB voltage limit must use 80 mV steps");
            return ESP_ERR_INVALID_ARG;
        }

        uint8_t fast_code = 9;

#if defined(CONFIG_MCW_POWER_FAST_200)
        fast_code = 8;
#elif defined(CONFIG_MCW_POWER_FAST_400)
        fast_code = 10;
#endif

        uint8_t target_code = 3;

#if defined(CONFIG_MCW_POWER_TARGET_4100)
        target_code = 2;
#endif

        uint8_t usb_code = 4;

#if defined(CONFIG_MCW_POWER_USB_500)
        usb_code = 1;
#elif defined(CONFIG_MCW_POWER_USB_900)
        usb_code = 2;
#endif

        // Configure charger limits, preserving unrelated bits
        esp_err_t err = update_axp_register(0x61, 0x0F, CONFIG_MCW_POWER_PRECHARGE_MA / 25);
        if(err != ESP_OK) return err;

        err = update_axp_register(0x62, 0x1F, fast_code);
        if(err != ESP_OK) return err;

        err = update_axp_register(0x63, 0x1F, 0x10 | (CONFIG_MCW_POWER_TERMINATION_MA / 25));
        if(err != ESP_OK) return err;

        err = update_axp_register(0x64, 0x07, target_code);
        if(err != ESP_OK) return err;

        err = update_axp_register(0x16, 0x07, usb_code);
        if(err != ESP_OK) return err;

        err = update_axp_register(0x15, 0x0F, (CONFIG_MCW_POWER_USB_VOLTAGE_MV - 3880) / 80);
        if(err != ESP_OK) return err;

        // Configure charger enable after its limits are established
#if defined(CONFIG_MCW_POWER_CHARGING_ENABLED)
        err = update_axp_register(0x18, 0x02, 0x02);
#else
        err = update_axp_register(0x18, 0x02, 0x00);
#endif

        if(err != ESP_OK) return err;

        ESP_LOGI(TAG, "Battery charging configuration applied");

#else
        ESP_LOGI(TAG, "Existing battery charging configuration preserved");
#endif

        return ESP_OK;
    }

    // Initialize verified board power configuration
    esp_err_t configure(){
        if(pmu_handle == nullptr){
            return ESP_ERR_INVALID_STATE;
        }

        ESP_LOGI(TAG, "Configuring AXP2101");

        esp_err_t err = configure_voltages();
        if(err != ESP_OK) return err;

        err = configure_charger();
        if(err != ESP_OK) return err;

        // Enable battery and VBUS voltage ADC measurements
        err = update_axp_register(0x30, 0x05, 0x05);
        if(err != ESP_OK) return err;

        // Disable all peripheral power rails
        static constexpr rail_t rails[] = {
            rail_t::sd,
            rail_t::display,
            rail_t::radio,
            rail_t::sensor,
            rail_t::gps,
            rail_t::speaker,
            rail_t::nfc
        };

        for(const auto &rail : rails){
            err = disable(rail);
            if(err != ESP_OK) return err;
        }

        ESP_LOGI(TAG, "AXP2101 configuration complete");
        return ESP_OK;
    }
}
