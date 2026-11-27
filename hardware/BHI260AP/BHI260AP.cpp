#include "BHI260AP.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstring>

#include "AXP2101.h"
#include "board.h"
#include "board_pins.h"

#include "bhy2.h"
#include "firmware/bhi260ap/BHI260AP.fw.h"

namespace hardware::bhi260ap {

    namespace {
        static constexpr const char *TAG = "BHI260AP";
        // Temporary physical sensor verification; disable after testing.
        static constexpr bool kLogSensorEvents = true;
        static constexpr uint32_t kTransferBytes = 256;
        static constexpr uint32_t kRecoveryPollMs = 1000;
        static constexpr uint32_t kErrorRetryMs = 250;
        static constexpr int kI2cTimeoutMs = 100;
        static i2c_master_dev_handle_t s_device = nullptr;
        static bhy2_dev s_hub{};
        static uint8_t s_fifo[2048]{};
        static status_t s_status{};
        static bool s_powered = false;
        static bool s_irq_registered = false;
        static bool s_wrist_event = false;
        static bool s_motion_event = false;
        static uint8_t s_step_id = 0;
        static uint32_t s_last_poll = 0;
        static bool s_fifo_failed = false;
        // Shared only between the GPIO ISR and the application task.
        static portMUX_TYPE s_pending_mux = portMUX_INITIALIZER_UNLOCKED;
        static bool s_pending = false;

        // Bosch transport callbacks: the shared ESP-IDF I2C bus owns serialization.
        BHY2_INTF_RET_TYPE read_regs(uint8_t reg, uint8_t *data, uint32_t length, void *){
            if(!s_device || !data || length == 0 || length > kTransferBytes) return -1;
            return i2c_master_transmit_receive(s_device, &reg, 1, data, length, kI2cTimeoutMs) == ESP_OK ? BHY2_INTF_RET_SUCCESS : -1;
        }

        BHY2_INTF_RET_TYPE write_regs(uint8_t reg, const uint8_t *data, uint32_t length, void *){
            if(!s_device || !data || length == 0 || length > kTransferBytes) return -1;
            uint8_t buffer[kTransferBytes + 1];
            buffer[0] = reg;
            std::memcpy(buffer + 1, data, length);
            return i2c_master_transmit(s_device, buffer, length + 1, kI2cTimeoutMs) == ESP_OK ? BHY2_INTF_RET_SUCCESS : -1;
        }

        void delay_us(uint32_t us, void *){
            // Yield for long firmware waits; retain a precise short register delay.
            if(us >= 1000){
                const TickType_t ticks = pdMS_TO_TICKS((us + 999U) / 1000U);
                vTaskDelay(ticks + 1);
            }else if(us != 0){
                esp_rom_delay_us(us);
            }
        }

        uint32_t millis_now(){ return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL); }

        esp_err_t api_result(int8_t rc, const char *operation){
            if(rc == BHY2_OK) return ESP_OK;
            ESP_LOGW(TAG, "%s failed: Bosch status %d", operation, static_cast<int>(rc));
            return rc == BHY2_E_TIMEOUT ? ESP_ERR_TIMEOUT : ESP_FAIL;
        }

        // Signal FIFO data without I2C access or logging in the ISR.
        void IRAM_ATTR sensor_irq(void *){
            portENTER_CRITICAL_ISR(&s_pending_mux);
            s_pending = true;
            portEXIT_CRITICAL_ISR(&s_pending_mux);
        }

        void set_pending(bool pending){
            portENTER_CRITICAL(&s_pending_mux);
            s_pending = pending;
            portEXIT_CRITICAL(&s_pending_mux);
        }

        bool take_pending(){
            portENTER_CRITICAL(&s_pending_mux);
            const bool pending = s_pending;
            s_pending = false;
            portEXIT_CRITICAL(&s_pending_mux);
            return pending;
        }

        // The Bosch parser excludes the sensor ID from data_ptr, but includes it in data_size.
        void sensor_event(const bhy2_fifo_parse_data_info *info, void *){
            if(!info || !info->data_ptr) return;
            if(info->sensor_id == s_step_id && info->data_size == 5){
                const uint8_t *p = info->data_ptr;
                const uint32_t steps = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
                if(kLogSensorEvents && (!s_status.step_count_valid || steps != s_status.steps)) ESP_LOGI(TAG, "Step count: %lu", static_cast<unsigned long>(steps));
                s_status.steps = steps;
                s_status.step_count_valid = true;
            }else if(info->sensor_id == BHY2_SENSOR_ID_WRIST_TILT_GESTURE){
                s_wrist_event = true;
                if(kLogSensorEvents) ESP_LOGI(TAG, "Wrist tilt detected");
            }else if(info->sensor_id == BHY2_SENSOR_ID_MOTION_DET){
                s_motion_event = true;
                if(kLogSensorEvents) ESP_LOGI(TAG, "Motion detected");
            }
        }

        // Availability is discovered from the uploaded firmware, not assumed from the chip ID.
        bool enable_sensor(uint8_t id, const char *name, float rate, uint32_t latency){
            if(!bhy2_is_sensor_available(id, &s_hub)){
                ESP_LOGW(TAG, "%s unavailable in sensor firmware", name);
                return false;
            }
            int8_t rc = bhy2_register_fifo_parse_callback(id, sensor_event, nullptr, &s_hub);
            if(rc == BHY2_OK) rc = bhy2_set_virt_sensor_cfg(id, rate, latency, &s_hub);
            if(api_result(rc, name) != ESP_OK){
                (void)bhy2_deregister_fifo_parse_callback(id, &s_hub);
                return false;
            }
            return true;
        }

        esp_err_t boot_firmware(){
            int8_t rc = bhy2_init(BHY2_I2C_INTERFACE, read_regs, write_regs, delay_us, kTransferBytes, nullptr, &s_hub);
            if(rc == BHY2_OK) rc = bhy2_soft_reset(&s_hub);
            uint8_t product = 0;
            if(rc == BHY2_OK) rc = bhy2_get_product_id(&product, &s_hub);
            if(rc != BHY2_OK) return api_result(rc, "Sensor identification");
            if(product != BHY2_PRODUCT_ID){
                ESP_LOGE(TAG, "Unexpected product ID: 0x%02X", product);
                return ESP_ERR_INVALID_RESPONSE;
            }

            // Active-high, level-triggered, push-pull interrupt. Leave FIFO interrupts enabled.
            rc = bhy2_set_host_interrupt_ctrl(BHY2_ICTL_DISABLE_STATUS_FIFO | BHY2_ICTL_DISABLE_DEBUG, &s_hub);
            if(rc == BHY2_OK) rc = bhy2_set_host_intf_ctrl(0, &s_hub);
            uint8_t boot_status = 0;
            if(rc == BHY2_OK) rc = bhy2_get_boot_status(&boot_status, &s_hub);
            if(rc != BHY2_OK) return api_result(rc, "Boot status");
            if(!(boot_status & BHY2_BST_HOST_INTERFACE_READY)) return ESP_ERR_INVALID_STATE;

            // Standard Bosch BHI260AP image; RAM upload only, never sensor flash.
            rc = bhy2_upload_firmware_to_ram(bhy2_firmware_image, sizeof(bhy2_firmware_image), &s_hub);
            if(rc == BHY2_OK) rc = bhy2_boot_from_ram(&s_hub);
            if(rc == BHY2_OK) rc = bhy2_get_kernel_version(&s_status.kernel_version, &s_hub);
            if(rc != BHY2_OK) return api_result(rc, "RAM firmware boot");
            if(s_status.kernel_version == 0) return ESP_ERR_INVALID_RESPONSE;

            rc = bhy2_get_and_process_fifo(s_fifo, sizeof(s_fifo), &s_hub);
            if(rc == BHY2_OK) rc = bhy2_update_virtual_sensor_list(&s_hub);
            return api_result(rc, "Virtual sensor discovery");
        }

        esp_err_t configure_interrupt(){
            gpio_config_t cfg{};
            cfg.pin_bit_mask = 1ULL << hardware::board::pins::SENSOR_IRQ;
            cfg.mode = GPIO_MODE_INPUT;
            cfg.pull_up_en = GPIO_PULLUP_DISABLE;
            cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
            cfg.intr_type = GPIO_INTR_POSEDGE;
            esp_err_t err = gpio_config(&cfg);
            if(err != ESP_OK) return err;
            err = gpio_isr_handler_add(hardware::board::pins::SENSOR_IRQ, sensor_irq, nullptr);
            if(err == ESP_OK) s_irq_registered = true;
            return err;
        }
    }

    // Hardware lifecycle
    esp_err_t shutdown(){
        s_status = {};
        s_wrist_event = false;
        s_motion_event = false;
        if(s_irq_registered){
            gpio_intr_disable(hardware::board::pins::SENSOR_IRQ);
            const esp_err_t err = gpio_isr_handler_remove(hardware::board::pins::SENSOR_IRQ);
            if(err != ESP_OK) return err;
            s_irq_registered = false;
        }
        set_pending(false);
        if(s_device){
            const esp_err_t err = i2c_master_bus_rm_device(s_device);
            if(err != ESP_OK) return err;
            s_device = nullptr;
        }
        if(s_powered){
            const esp_err_t err = hardware::axp2101::disable(hardware::axp2101::rail_t::sensor);
            if(err != ESP_OK) return err;
            s_powered = false;
        }
        return ESP_OK;
    }

    esp_err_t init(){
        if(s_status.ready) return ESP_OK;
        if(s_device || s_powered || s_irq_registered){
            const esp_err_t err = shutdown();
            if(err != ESP_OK) return err;
        }
        if(!hardware::board::i2c_bus()) return ESP_ERR_INVALID_STATE;
        esp_err_t err = hardware::axp2101::enable(hardware::axp2101::rail_t::sensor);
        if(err != ESP_OK) return err;
        s_powered = true;
        vTaskDelay(pdMS_TO_TICKS(100) + 1);

        i2c_device_config_t cfg{};
        cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        cfg.device_address = hardware::board::addresses::SENSOR;
        cfg.scl_speed_hz = 400000;
        err = i2c_master_bus_add_device(hardware::board::i2c_bus(), &cfg, &s_device);
        s_hub = {};
        if(err == ESP_OK) err = boot_firmware();
        if(err == ESP_OK) err = configure_interrupt();
        if(err != ESP_OK){
            const esp_err_t cleanup = shutdown();
            if(cleanup != ESP_OK) ESP_LOGE(TAG, "Cleanup failed: %s", esp_err_to_name(cleanup));
            return err;
        }

        // Prefer the low-power step counter when offered; otherwise use the standard counter.
        s_step_id = bhy2_is_sensor_available(BHY2_SENSOR_ID_STC_LP, &s_hub) ? BHY2_SENSOR_ID_STC_LP : BHY2_SENSOR_ID_STC;
        s_status.steps_available = enable_sensor(s_step_id, "Step counter", 1.0f, 1000);
        s_status.wrist_tilt_available = enable_sensor(BHY2_SENSOR_ID_WRIST_TILT_GESTURE, "Wrist tilt", 1.0f, 0);
        s_status.motion_available = enable_sensor(BHY2_SENSOR_ID_MOTION_DET, "Motion detection", 1.0f, 0);
        if(!s_status.steps_available && !s_status.wrist_tilt_available && !s_status.motion_available){
            const esp_err_t cleanup = shutdown();
            if(cleanup != ESP_OK) ESP_LOGE(TAG, "Cleanup failed: %s", esp_err_to_name(cleanup));
            return ESP_ERR_NOT_SUPPORTED;
        }
        s_fifo_failed = false;
        s_last_poll = millis_now();
        set_pending(true);
        s_status.ready = true;
        ESP_LOGI(TAG, "Ready: kernel=%u steps=%d wrist=%d motion=%d", s_status.kernel_version, s_status.steps_available, s_status.wrist_tilt_available, s_status.motion_available);
        return ESP_OK;
    }

    // FIFO processing in the application task; no I2C or logging in the GPIO ISR.
    void loop(){
        if(!s_status.ready) return;
        const uint32_t now = millis_now();
        if(s_fifo_failed && now - s_last_poll < kErrorRetryMs) return;
        const bool pending = take_pending();
        if(!pending && !gpio_get_level(hardware::board::pins::SENSOR_IRQ) && now - s_last_poll < kRecoveryPollMs) return;
        s_last_poll = now;
        const int8_t rc = bhy2_get_and_process_fifo(s_fifo, sizeof(s_fifo), &s_hub);
        if(rc != BHY2_OK){
            if(!s_fifo_failed) (void)api_result(rc, "FIFO processing");
            s_fifo_failed = true;
            set_pending(true);
        }else{
            if(s_fifo_failed) ESP_LOGI(TAG, "FIFO communication recovered");
            s_fifo_failed = false;
        }
    }

    // Main-task-only snapshot and consumable events.
    bool ready(){ return s_status.ready; }
    status_t get_status(){ return s_status; }
    uint32_t step_count(){ return s_status.steps; }
    bool wrist_tilt_detected(){ const bool detected = s_wrist_event; s_wrist_event = false; return detected; }
    bool motion_detected(){ const bool detected = s_motion_event; s_motion_event = false; return detected; }
}
