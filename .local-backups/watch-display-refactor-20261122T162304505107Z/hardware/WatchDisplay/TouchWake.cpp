#include "TouchWake.h"
#include "WatchDisplay.h"

#include "XL9555.h"
#include "board.h"
#include "board_pins.h"

#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_cst9217.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace hardware::watch_display::touch_wake {
namespace {
    const char *const TAG = "TouchWake";
    constexpr uint32_t kScreenTimeoutMs = 5000;
    constexpr uint32_t kWaitPeriodMs = 50;

    esp_lcd_panel_io_handle_t s_touch_io = nullptr;
    esp_lcd_touch_handle_t s_touch = nullptr;
    TaskHandle_t s_task = nullptr;
    bool s_started = false;

    uint64_t uptime_ms() {
        return static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL;
    }

    void cleanup() {
        if (s_touch != nullptr) {
            esp_lcd_touch_del(s_touch);
            s_touch = nullptr;
        }
        if (s_touch_io != nullptr) {
            esp_lcd_panel_io_del(s_touch_io);
            s_touch_io = nullptr;
        }
    }

    // CST9217 reports input using its active-low TP_INT line (GPIO12).
    // A notification is enough for wake; no I2C operations in the ISR.
    void IRAM_ATTR touch_irq(void *) {
        if (s_task == nullptr) return;
        BaseType_t higher_priority_woken = pdFALSE;
        vTaskNotifyGiveFromISR(s_task, &higher_priority_woken);
        if (higher_priority_woken == pdTRUE) portYIELD_FROM_ISR();
    }

    void touch_task(void *) {
        uint64_t last_activity_ms = uptime_ms();
        bool screen_off = false;
        bool touch_communication_ok = true;

        while (true) {
            const uint32_t events = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kWaitPeriodMs));
            const uint64_t now = uptime_ms();

            if (events > 0) {
                // Only read touch data following an IRQ. The CST9217 may not
                // provide a valid 0xAB-marked report when idle.
                const esp_err_t read_err = esp_lcd_touch_read_data(s_touch);

                // The CST92xx protocol requires writing 0xD000AB to acknowledge
                // the report and allow subsequent touch interrupts.
                // io_cfg.lcd_cmd_bits=8 and disable_control_phase=1 mean the
                // transaction bytes are exactly D0 00 AB.
                const uint8_t ack[] = {0x00, 0xAB};
                const esp_err_t ack_err = esp_lcd_panel_io_tx_param(
                    s_touch_io, 0xD0, ack, sizeof(ack));

                if (read_err != ESP_OK && read_err != ESP_ERR_INVALID_RESPONSE) {
                    ESP_LOGW(TAG, "Touch report read failed: %s",
                             esp_err_to_name(read_err));
                }
                if (ack_err != ESP_OK) {
                    if (touch_communication_ok) {
                        ESP_LOGW(TAG, "Touch report ACK failed: %s; display will stay awake",
                                 esp_err_to_name(ack_err));
                    }
                    touch_communication_ok = false;
                } else if (!touch_communication_ok) {
                    touch_communication_ok = true;
                    ESP_LOGI(TAG, "Touch report acknowledgments recovered");
                }

                // A physical touch interrupt is an activity signal regardless
                // of whether this report contained a valid coordinate sample.
                last_activity_ms = now;
                if (screen_off) {
                    if (hardware::watch_display::set_screen_awake(true) == ESP_OK) {
                        screen_off = false;
                        ESP_LOGI(TAG, "AMOLED woke on touch");
                    }
                }
            }

            // Screen-off is deliberately disabled after a failed I2C ACK so
            // that we don't leave an unreliable touch detector with a black UI.
            if (!screen_off && touch_communication_ok &&
                now - last_activity_ms >= kScreenTimeoutMs) {
                if (hardware::watch_display::set_screen_awake(false) == ESP_OK) {
                    screen_off = true;
                    ESP_LOGI(TAG, "AMOLED off after %lu ms idle",
                             static_cast<unsigned long>(kScreenTimeoutMs));
                }
            }
        }
    }
} // namespace

esp_err_t start() {
    if (s_started) return ESP_OK;
    if (board::i2c_bus() == nullptr) return ESP_ERR_INVALID_STATE;

    // Touch reset goes through XL9555 P1.0, not an ESP32 GPIO.
    esp_err_t err = XL9555::set_touch_reset(false);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(15));
    err = XL9555::set_touch_reset(true);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(100));

    esp_lcd_panel_io_i2c_config_t io_cfg = {};
    io_cfg.dev_addr = board::addresses::TOUCH; // 0x1A on T-Watch Ultra
    io_cfg.scl_speed_hz = 100000;
    io_cfg.control_phase_bytes = 1;
    io_cfg.dc_bit_offset = 0;
    io_cfg.lcd_cmd_bits = 8;
    io_cfg.lcd_param_bits = 8;
    io_cfg.flags.disable_control_phase = 1;
    io_cfg.transaction_timeout_ms = 200;

    err = esp_lcd_new_panel_io_i2c(board::i2c_bus(), &io_cfg, &s_touch_io);
    if (err != ESP_OK) return err;

    esp_lcd_touch_config_t touch_cfg = {};
    touch_cfg.x_max = 410;
    touch_cfg.y_max = 502;
    touch_cfg.rst_gpio_num = GPIO_NUM_NC; // Handled by XL9555.
    touch_cfg.int_gpio_num = GPIO_NUM_NC; // Our task owns GPIO12 IRQ.
    touch_cfg.levels.reset = 0;
    touch_cfg.levels.interrupt = 0;

    err = esp_lcd_touch_new_i2c_cst9217(s_touch_io, &touch_cfg, &s_touch);
    if (err != ESP_OK) {
        cleanup();
        return err;
    }

    gpio_config_t gpio_cfg = {};
    gpio_cfg.pin_bit_mask = (1ULL << board::pins::TOUCH_IRQ);
    gpio_cfg.mode = GPIO_MODE_INPUT;
    gpio_cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_cfg.intr_type = GPIO_INTR_NEGEDGE;
    err = gpio_config(&gpio_cfg);
    if (err != ESP_OK) {
        cleanup();
        return err;
    }

    err = gpio_isr_handler_add(board::pins::TOUCH_IRQ, touch_irq, nullptr);
    if (err != ESP_OK) {
        cleanup();
        return err;
    }

    const BaseType_t created = xTaskCreate(touch_task, "watch_touch_wake", 4096,
                                           nullptr, 4, &s_task);
    if (created != pdPASS) {
        gpio_isr_handler_remove(board::pins::TOUCH_IRQ);
        s_task = nullptr;
        cleanup();
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    ESP_LOGI(TAG, "CST9217 ready at 0x%02X; GPIO12 IRQ wake, 5-second timeout",
             board::addresses::TOUCH);
    return ESP_OK;
}
} // namespace hardware::watch_display::touch_wake
