
#include "AXP2101.h"
#include "Storage.h"

#include "board.h"
#include "board_pins.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "sdkconfig.h"

namespace hardware::axp2101 {

    static const char *TAG = "AXP2101";
    static i2c_master_dev_handle_t pmu_handle = nullptr;

    static constexpr uint8_t REG_STATUS1    = 0x00;
    static constexpr uint8_t REG_STATUS2    = 0x01;
    static constexpr uint8_t REG_CHIP_ID    = 0x03;
    static constexpr uint8_t REG_ADC_CTRL   = 0x30;
    static constexpr uint8_t REG_BATTERY_V  = 0x34;
    static constexpr uint8_t REG_VBUS_V     = 0x38;
    static constexpr uint8_t REG_LDO_ENABLE = 0x90;
    static constexpr uint8_t REG_BATTERY_PC = 0xA4;

    static constexpr uint8_t CHIP_ID = 0x4A;

    // The original 128-byte battery gauge model supplied by LilyGO for
    // T-Watch Ultra. Source: LilyGoLib/src/LilyGoWatchUltra.cpp (MIT license).
    static constexpr uint8_t LILYGO_WATCH_ULTRA_BATTERY_MODEL[128] = {
        0x01, 0xf5, 0x40, 0x00, 0x1b, 0x1e, 0x28, 0x0f, 0x0c, 0x1e, 0x32, 0x02, 0x14, 0x05, 0x0a, 0x04,
        0x74, 0xfc, 0xf4, 0x0d, 0x43, 0x10, 0x52, 0xfb, 0xa6, 0x01, 0xea, 0x04, 0x64, 0x06, 0x52, 0x06,
        0x18, 0x0a, 0xe7, 0x0f, 0x9f, 0x0f, 0x51, 0x09, 0xf7, 0x0e, 0x89, 0x0e, 0x71, 0x04, 0x58, 0x04,
        0x43, 0x09, 0x32, 0x0e, 0x1c, 0x0e, 0x14, 0x09, 0x04, 0x0d, 0xe9, 0x0d, 0xde, 0x03, 0xc8, 0x03,
        0xb3, 0x08, 0x9d, 0x0d, 0x79, 0x0d, 0x3a, 0x07, 0xf5, 0x9e, 0x56, 0x47, 0x36, 0x20, 0x24, 0x17,
        0xc5, 0x98, 0x7e, 0x66, 0x4e, 0x44, 0x38, 0x1a, 0x12, 0x0a, 0xf6, 0x00, 0x00, 0xf6, 0x00, 0xf6,
        0x00, 0xfb, 0x00, 0x00, 0xfb, 0x00, 0x00, 0xfb, 0x00, 0x00, 0xf6, 0x00, 0x00, 0xf6, 0x00, 0xf6,
        0x00, 0xfb, 0x00, 0x00, 0xfb, 0x00, 0x00, 0xfb, 0x00, 0x00, 0xf6, 0x00, 0x00, 0xf6, 0x00, 0xf6,
    };
    static constexpr uint8_t LILYGO_MODEL_VERSION = 1;
    static constexpr uint8_t REG_GAUGE_RESET = 0x17;
    static constexpr uint8_t REG_BATTERY_PARAMETERS = 0xA1;
    static constexpr uint8_t REG_GAUGE_CONTROL = 0xA2;

    static esp_err_t read_register(uint8_t reg, uint8_t *data, size_t len){
        if(pmu_handle == nullptr)
            return ESP_ERR_INVALID_STATE;

        return i2c_master_transmit_receive(pmu_handle, &reg, 1, data, len, 1000);
    }

    esp_err_t read_charging(bool &charging){
        // PMU status2: bits 6:5 indicate battery current direction;
        // bits 2:0 identify the charger phase (0-3 active, 4 done, 5 idle).
        uint8_t status = 0;
        charging = false;
        const esp_err_t err = read_register(REG_STATUS2, &status, 1);
        if(err != ESP_OK) return err;
        const uint8_t direction = (status >> 5) & 0x03U;
        const uint8_t phase = status & 0x07U;
        charging = direction == 1U && phase <= 3U;
        return ESP_OK;
    }

    esp_err_t read_battery(battery_info_t &out){
        out = battery_info_t{};

        uint8_t raw[2] = {};

        esp_err_t voltage_err = read_register(REG_BATTERY_V, raw, sizeof(raw));

        if(voltage_err == ESP_OK){
            uint16_t mv = (static_cast<uint16_t>(raw[0] & 0x1F) << 8) | static_cast<uint16_t>(raw[1]);

            if(mv >= 2000 && mv <= 5000){
                out.voltage_valid = true;
                out.millivolts = mv;
            }
        }

        uint8_t percent = 0;
        esp_err_t percent_err = read_register(REG_BATTERY_PC, &percent, 1);

        if(percent_err == ESP_OK && percent <= 100){
            out.percent_valid = true;
            out.percentage = percent;
        }

        if(out.voltage_valid || out.percent_valid)
            return ESP_OK;

        if(voltage_err != ESP_OK)
            return voltage_err;

        if(percent_err != ESP_OK)
            return percent_err;

        return ESP_ERR_INVALID_RESPONSE;
    }

    static esp_err_t write_axp_register(uint8_t reg, uint8_t value){
        if(pmu_handle == nullptr)
            return ESP_ERR_INVALID_STATE;

        uint8_t data[2] = {reg, value};

        return i2c_master_transmit(pmu_handle, data, sizeof(data), 1000);
    }

    static esp_err_t update_axp_register(uint8_t reg, uint8_t mask, uint8_t value){

        uint8_t current = 0;
        esp_err_t err = read_register(reg, &current, 1);

        if(err != ESP_OK)
            return err;

        uint8_t updated = (current & ~mask) | (value & mask);

        if(updated == current)
            return ESP_OK;

        err = write_axp_register(reg, updated);
        if(err != ESP_OK)
            return err;

        uint8_t verify = 0;
        err = read_register(reg, &verify, 1);

        if(err != ESP_OK)
            return err;

        if((verify & mask) != (updated & mask)){
            ESP_LOGE(TAG, "AXP2101 register 0x%02X verification failed", reg);
            return ESP_ERR_INVALID_RESPONSE;
        }

        return ESP_OK;
    }

    // Match the AXP2101 gauge-ROM programming protocol from XPowersLib.
    // The battery model is written ONCE (after NVS marker check), not at
    // every boot. The PMU's state is restored even if verification fails.
    static esp_err_t gauge_control_bit(uint8_t reg, uint8_t bit, bool enabled){
        uint8_t value = 0;
        esp_err_t err = read_register(reg, &value, 1);
        if(err != ESP_OK) return err;
        const uint8_t mask = static_cast<uint8_t>(1U << bit);
        value = enabled ? static_cast<uint8_t>(value | mask)
                        : static_cast<uint8_t>(value & ~mask);
        return write_axp_register(reg, value);
    }

    static esp_err_t reset_gauge(){
        esp_err_t err = gauge_control_bit(REG_GAUGE_RESET, 2, true);
        if(err != ESP_OK) return err;
        return gauge_control_bit(REG_GAUGE_RESET, 2, false);
    }

    static esp_err_t select_gauge_rom(){
        // The ROM/SRAM selection can survive an earlier firmware session.
        // Explicitly select ROM before starting the serial model interface.
        esp_err_t err = gauge_control_bit(REG_GAUGE_CONTROL, 4, false);
        if(err != ESP_OK) return err;
        err = gauge_control_bit(REG_GAUGE_CONTROL, 0, false);
        if(err != ESP_OK) return err;
        return gauge_control_bit(REG_GAUGE_CONTROL, 0, true);
    }

    static esp_err_t program_lilygo_battery_model(){
        esp_err_t err = reset_gauge();
        if(err != ESP_OK) return err;
        err = select_gauge_rom();
        if(err != ESP_OK) goto cleanup;

        for(uint8_t value : LILYGO_WATCH_ULTRA_BATTERY_MODEL){
            err = write_axp_register(REG_BATTERY_PARAMETERS, value);
            if(err != ESP_OK) goto cleanup;
        }

        // XPowers writeGaugeData(): latch the ROM table before readback.
        err = select_gauge_rom();
        if(err != ESP_OK) goto cleanup;
        // XPowers compareGaugeData(): restart ROM read pointer, then read 128.
        err = select_gauge_rom();
        if(err != ESP_OK) goto cleanup;
        for(uint8_t expected : LILYGO_WATCH_ULTRA_BATTERY_MODEL){
            uint8_t actual = 0;
            err = read_register(REG_BATTERY_PARAMETERS, &actual, 1);
            if(err != ESP_OK) goto cleanup;
            if(actual != expected){
                ESP_LOGE(TAG, "LilyGO AXP2101 battery model readback mismatch");
                err = ESP_ERR_INVALID_CRC;
                goto cleanup;
            }
        }

    cleanup:
        // Exit ROM access and select the normal gauge data interface.
        // Preserve the first failure, but still attempt every cleanup step.
        esp_err_t restore = gauge_control_bit(REG_GAUGE_CONTROL, 0, false);
        if(err == ESP_OK) err = restore;
        restore = gauge_control_bit(REG_GAUGE_CONTROL, 4, true);
        if(err == ESP_OK) err = restore;
        restore = reset_gauge();
        if(err == ESP_OK) err = restore;
        return err;
    }

    static esp_err_t ensure_lilygo_battery_model(){
        hardware::storage::Handle model_store;
        esp_err_t err = hardware::storage::open("axp2101", hardware::storage::Access::read_write, model_store);
        if(err != ESP_OK) return err;

        uint8_t stored_version = 0;
        size_t length = sizeof(stored_version);
        err = hardware::storage::read_blob(model_store, "lilygo_gauge", &stored_version, &length);
        if(err == ESP_OK && length == sizeof(stored_version) && stored_version == LILYGO_MODEL_VERSION){
            hardware::storage::close(model_store);
            ESP_LOGD(TAG, "LilyGO T-Watch Ultra battery model already installed");
            return ESP_OK;
        }
        if(err != ESP_OK && !hardware::storage::is_not_found(err)){
            hardware::storage::close(model_store);
            return err;
        }

        ESP_LOGI(TAG, "Installing LilyGO T-Watch Ultra AXP2101 fuel-gauge model");
        err = program_lilygo_battery_model();
        if(err == ESP_OK){
            // Only mark success AFTER the full 128-byte ROM readback passed.
            err = hardware::storage::write_blob(model_store, "lilygo_gauge",
                                                 &LILYGO_MODEL_VERSION, sizeof(LILYGO_MODEL_VERSION));
            if(err == ESP_OK) err = hardware::storage::commit(model_store);
        }
        hardware::storage::close(model_store);
        if(err == ESP_OK) ESP_LOGI(TAG, "LilyGO battery model verified and saved in NVS");
        return err;
    }

    static esp_err_t set_voltage_reg(uint8_t reg, uint16_t millivolts){

        if(millivolts < 500 || millivolts > 3500 || (millivolts - 500) % 100 != 0){
            return ESP_ERR_INVALID_ARG;
        }

        uint8_t code = (millivolts - 500) / 100;
        return update_axp_register(reg, 0x1F, code);
    }

    esp_err_t set_enabled(rail_t rail, bool enabled){
        uint8_t bit = static_cast<uint8_t>(rail);

        if(bit > 7 || bit == 6)
            return ESP_ERR_INVALID_ARG;

        uint8_t mask = 1 << bit;

        esp_err_t err = update_axp_register(0x90, mask, enabled ? mask : 0);

        if(err != ESP_OK){
            ESP_LOGE(TAG, "Unable to change rail %u: %s", bit, esp_err_to_name(err));
            return err;
        }

        ESP_LOGD(TAG, "Power rail %u: %s", bit, enabled ? "On" : "Off");

        return ESP_OK;
    }

    esp_err_t enable(rail_t rail){
        return set_enabled(rail, true);
    }

    esp_err_t disable(rail_t rail){
        return set_enabled(rail, false);
    }

    static esp_err_t configure_voltages(){
        struct voltage_t {
            uint8_t reg;
            uint16_t millivolts;
        };

        static constexpr voltage_t voltages[] = { {0x92, 3300}, {0x93, 3300}, {0x94, 3300}, {0x95, 1800}, {0x96, 3300}, {0x97, 3300}, {0x99, 3300} };

        for(const auto &entry : voltages){
            esp_err_t err = set_voltage_reg(entry.reg, entry.millivolts);

            if(err != ESP_OK){
                ESP_LOGE(TAG, "Voltage configuration failed at 0x%02X", entry.reg);
                return err;
            }
        }

        ESP_LOGD(TAG, "Peripheral regulator voltages configured");
        return ESP_OK;
    }

    static esp_err_t configure_charger(){
#if defined(CONFIG_MCW_POWER_APPLY_CHARGER)

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

#if defined(CONFIG_MCW_POWER_CHARGING_ENABLED)
        err = update_axp_register(0x18, 0x02, 0x02);
#else
        err = update_axp_register(0x18, 0x02, 0x00);
#endif

        if(err != ESP_OK)
            return err;

        ESP_LOGI(TAG, "Battery charging configuration applied");
#else
        ESP_LOGI(TAG, "Existing battery charging configuration preserved");
#endif

        return ESP_OK;
    }

   esp_err_t init(){
        static bool configured = false;

        if(pmu_handle != nullptr){
            if(configured) return ESP_OK;

            esp_err_t err = configure();
            configured = (err == ESP_OK);
            return err;
        }

        if(hardware::board::i2c_bus() == nullptr){
            ESP_LOGE(TAG, "Board I2C bus is not initialized");
            return ESP_ERR_INVALID_STATE;
        }

        ESP_LOGD(TAG, "Initializing AXP2101");

        i2c_device_config_t config = {};
        config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        config.device_address = hardware::board::addresses::AXP2101;
        config.scl_speed_hz = 100000;

        esp_err_t err = i2c_master_bus_add_device(hardware::board::i2c_bus(), &config, &pmu_handle);

        if(err != ESP_OK){
            ESP_LOGE(TAG, "Unable to register AXP2101: %s", esp_err_to_name(err));
            return err;
        }

        uint8_t chip_id = 0;
        err = read_register(REG_CHIP_ID, &chip_id, 1);

        if(err != ESP_OK || chip_id != CHIP_ID){
            ESP_LOGE(TAG, "AXP2101 identification failed");

            i2c_master_bus_rm_device(pmu_handle);
            pmu_handle = nullptr;

            return err != ESP_OK ? err : ESP_ERR_INVALID_RESPONSE;
        }

        ESP_LOGI(TAG, "AXP2101 detected, chip ID: 0x%02X", chip_id);

        err = configure();
        configured = (err == ESP_OK);
        return err;
    }

    esp_err_t configure(){
        if(pmu_handle == nullptr)
            return ESP_ERR_INVALID_STATE;

        ESP_LOGD(TAG, "Configuring AXP2101");

        esp_err_t err = configure_voltages();
        if(err != ESP_OK) return err;

        err = configure_charger();
        if(err != ESP_OK) return err;

        // Battery detection and fuel-gauge enable.
        err = update_axp_register(0x18, 0x08, 0x08);
        if(err != ESP_OK) return err;

        err = update_axp_register(0x68, 0x01, 0x01);
        if(err != ESP_OK) return err;

        // Enable battery/VBUS ADC measurements.
        err = update_axp_register(0x30, 0x05, 0x05);
        if(err != ESP_OK) return err;

        // Like LilyGO's official firmware, program its battery model once.
        // An absent/failed calibration does not stop the watch booting; on
        // failure the next boot may retry, with warning in serial logs.
        err = ensure_lilygo_battery_model();
        if(err != ESP_OK){
            ESP_LOGW(TAG, "LilyGO fuel-gauge setup failed (%s); using existing gauge state",
                     esp_err_to_name(err));
        }

        static constexpr rail_t rails[] = { rail_t::sd, rail_t::display, rail_t::radio, rail_t::sensor, rail_t::gps, rail_t::speaker, rail_t::nfc };

        for(const auto &rail : rails){
            err = disable(rail);
            if(err != ESP_OK) return err;
        }

        ESP_LOGI(TAG, "AXP2101 configuration complete");
        return ESP_OK;
    }

}
