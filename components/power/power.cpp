
#include "power.h"

#include "board.h"
#include "board_pins.h"

#include "driver/i2c_master.h"
#include "esp_log.h"

namespace power {

    static const char *TAG = "power";

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

    esp_err_t read_status(status_t &status){
        if(pmu_handle == nullptr){
            return ESP_ERR_INVALID_STATE;
        }

        status = status_t{};

        uint8_t status1 = 0;
        uint8_t status2 = 0;
        uint8_t adc_ctrl = 0;
        uint8_t ldo_enable = 0;

        // Read PMU status registers
        esp_err_t err = read_register(REG_STATUS1, &status1, 1);
        if(err != ESP_OK) return err;

        err = read_register(REG_STATUS2, &status2, 1);
        if(err != ESP_OK) return err;

        err = read_register(REG_ADC_CTRL, &adc_ctrl, 1);
        if(err != ESP_OK) return err;

        err = read_register(REG_LDO_ENABLE, &ldo_enable, 1);
        if(err != ESP_OK) return err;

        status.battery_present = (status1 & (1 << 3)) != 0;
        status.vbus_good = (status1 & (1 << 5)) != 0;

        // Charge direction: 0 = standby, 1 = charge, 2 = discharge
        uint8_t charge_mode = (status2 >> 5) & 0x03;

        status.charging = charge_mode == 1;
        status.discharging = charge_mode == 2;

        // Read battery voltage if the ADC channel is enabled
        if(status.battery_present && (adc_ctrl & (1 << 0))){
            uint8_t data[2] = {};

            err = read_register(REG_BATTERY_V, data, sizeof(data));
            if(err != ESP_OK) return err;

            status.battery_mv = ((data[0] & 0x1F) << 8) | data[1];
        }

        // Read battery percentage reported by the fuel gauge
        if(status.battery_present){
            uint8_t percent = 0;

            err = read_register(REG_BATTERY_PC, &percent, 1);
            if(err != ESP_OK) return err;

            if(percent <= 100){
                status.battery_percent = percent;
            }
        }

        // Read USB voltage if VBUS is good and its ADC is enabled
        if(status.vbus_good && (adc_ctrl & (1 << 2))){
            uint8_t data[2] = {};

            err = read_register(REG_VBUS_V, data, sizeof(data));
            if(err != ESP_OK) return err;

            status.vbus_mv = ((data[0] & 0x3F) << 8) | data[1];
        }

        // Read current regulator enable states
        status.sd_enabled      = (ldo_enable & (1 << 0)) != 0;
        status.display_enabled = (ldo_enable & (1 << 1)) != 0;
        status.radio_enabled   = (ldo_enable & (1 << 2)) != 0;
        status.sensor_enabled  = (ldo_enable & (1 << 3)) != 0;
        status.gps_enabled     = (ldo_enable & (1 << 4)) != 0;
        status.speaker_enabled = (ldo_enable & (1 << 5)) != 0;
        status.nfc_enabled     = (ldo_enable & (1 << 7)) != 0;

        return ESP_OK;
    }

    esp_err_t log_status(){
        status_t status;

        esp_err_t err = read_status(status);

        if(err != ESP_OK){
            ESP_LOGE(TAG, "Unable to read power status: %s", esp_err_to_name(err));
            return err;
        }

        ESP_LOGI(TAG, "Battery present: %s", status.battery_present ? "Yes" : "No");

        ESP_LOGI(TAG, "USB VBUS good: %s", status.vbus_good ? "Yes" : "No");

        ESP_LOGI(TAG, "Charge mode: %s", status.charging ? "Charging" : status.discharging ? "Discharging" : "Standby/other");

        if(status.battery_mv >= 0){
            ESP_LOGI(TAG, "Battery voltage: %d mV", status.battery_mv);
        }else{
            ESP_LOGW(TAG, "Battery voltage unavailable");
        }

        if(status.battery_percent >= 0){
            ESP_LOGI(TAG, "Battery percentage: %d%%", status.battery_percent);
        }

        if(status.vbus_mv >= 0){
            ESP_LOGI(TAG, "USB voltage: %d mV", status.vbus_mv);
        }else{
            ESP_LOGI(TAG, "USB voltage unavailable");
        }

        ESP_LOGI(TAG, "SD power: %s", status.sd_enabled ? "On" : "Off");

        ESP_LOGI(TAG, "Display power: %s", status.display_enabled ? "On" : "Off");

        ESP_LOGI(TAG, "LoRa power: %s", status.radio_enabled ? "On" : "Off");

        ESP_LOGI(TAG, "Sensor power: %s", status.sensor_enabled ? "On" : "Off");

        ESP_LOGI(TAG, "GPS power: %s", status.gps_enabled ? "On" : "Off");

        ESP_LOGI(TAG, "Speaker power: %s", status.speaker_enabled ? "On" : "Off");

        ESP_LOGI(TAG, "NFC power: %s", status.nfc_enabled ? "On" : "Off");

        return ESP_OK;
    }

}
