#include "CST9217.h"

#include "XL9555.h"
#include "board.h"
#include "board_pins.h"

#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_cst9217.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace hardware::cst9217 {
namespace {
    const char *const TAG = "CST9217Hardware";
    esp_lcd_panel_io_handle_t s_io = nullptr;
    esp_lcd_touch_handle_t s_touch = nullptr;
    TaskHandle_t s_notification_task = nullptr;
    bool s_ready = false;

    void cleanup() {
        if (s_touch != nullptr) {
            esp_lcd_touch_del(s_touch);
            s_touch = nullptr;
        }
        if (s_io != nullptr) {
            esp_lcd_panel_io_del(s_io);
            s_io = nullptr;
        }
    }

    void IRAM_ATTR touch_irq(void *) {
        TaskHandle_t task = s_notification_task;
        if (task == nullptr) return;
        BaseType_t higher_priority_woken = pdFALSE;
        vTaskNotifyGiveFromISR(task, &higher_priority_woken);
        if (higher_priority_woken == pdTRUE) portYIELD_FROM_ISR();
    }
} // namespace

esp_err_t init() {
    if (s_ready) return ESP_OK;
    if (hardware::board::i2c_bus() == nullptr) return ESP_ERR_INVALID_STATE;

    // The touch reset signal is on XL9555 P1.0, not a native ESP32 GPIO.
    esp_err_t err = XL9555::set_touch_reset(false);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(15));
    err = XL9555::set_touch_reset(true);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(100));

    esp_lcd_panel_io_i2c_config_t io_cfg = {};
    io_cfg.dev_addr = hardware::board::addresses::TOUCH; // 0x1A on T-Watch Ultra.
    io_cfg.scl_speed_hz = 100000;
    io_cfg.control_phase_bytes = 1;
    io_cfg.dc_bit_offset = 0;
    io_cfg.lcd_cmd_bits = 8;
    io_cfg.lcd_param_bits = 8;
    io_cfg.flags.disable_control_phase = 1;
    io_cfg.transaction_timeout_ms = 200;

    err = esp_lcd_new_panel_io_i2c(hardware::board::i2c_bus(), &io_cfg, &s_io);
    if (err != ESP_OK) return err;

    esp_lcd_touch_config_t touch_cfg = {};
    touch_cfg.x_max = 410;
    touch_cfg.y_max = 502;
    touch_cfg.rst_gpio_num = GPIO_NUM_NC;
    touch_cfg.int_gpio_num = GPIO_NUM_NC; // IRQ owned by this component.
    touch_cfg.levels.reset = 0;
    touch_cfg.levels.interrupt = 0;

    err = esp_lcd_touch_new_i2c_cst9217(s_io, &touch_cfg, &s_touch);
    if (err != ESP_OK) { cleanup(); return err; }

    gpio_config_t gpio_cfg = {};
    gpio_cfg.pin_bit_mask = (1ULL << hardware::board::pins::TOUCH_IRQ);
    gpio_cfg.mode = GPIO_MODE_INPUT;
    gpio_cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_cfg.intr_type = GPIO_INTR_NEGEDGE;
    err = gpio_config(&gpio_cfg);
    if (err != ESP_OK) { cleanup(); return err; }
    
    // Register touch IRQ using the shared GPIO ISR service.
    err = gpio_isr_handler_add(
        hardware::board::pins::TOUCH_IRQ, touch_irq, nullptr);

    // Install the service only if it does not exist yet.
    if (err == ESP_ERR_INVALID_STATE) {
        esp_err_t install_err = gpio_install_isr_service(0);

        if (install_err != ESP_OK &&
            install_err != ESP_ERR_INVALID_STATE) {
            cleanup();
            return install_err;
        }

        err = gpio_isr_handler_add(
            hardware::board::pins::TOUCH_IRQ, touch_irq, nullptr);
    }

    if (err != ESP_OK) {
        cleanup();
        return err;
    }

    s_ready = true;
    ESP_LOGI(TAG, "CST9217 ready at 0x%02X; GPIO%d touch IRQ",
             hardware::board::addresses::TOUCH, static_cast<int>(hardware::board::pins::TOUCH_IRQ));
    return ESP_OK;
}

void set_notification_task(TaskHandle_t task) { s_notification_task = task; }

esp_err_t read_interrupt(TouchPoint &point) {
    point = {};
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    // Only read following GPIO12 IRQ: idle reads can lack the CST92xx 0xAB marker.
    const esp_err_t read_err = esp_lcd_touch_read_data(s_touch);
    esp_err_t point_err = ESP_OK;
    if (read_err == ESP_OK) {
        esp_lcd_touch_point_data_t data[1] = {};
        uint8_t count = 0;

        point_err = esp_lcd_touch_get_data(s_touch, data, &count, 1);
        if (point_err == ESP_OK) {
            point.report_valid = true;
            if (count > 0) {
                point.x = data[0].x;
                point.y = data[0].y;
                point.pressed = true;
            }
        }
    }

    // ACK the report after the read. With the 8-bit command and no control
    // phase, the transmitted sequence is D0 00 AB, as in the working build.
    const uint8_t ack[] = {0x00, 0xAB};
    const esp_err_t ack_err = esp_lcd_panel_io_tx_param(s_io, 0xD0, ack, sizeof(ack));
    if (ack_err != ESP_OK) return ack_err;
    // An IRQ without an 0xAB-marked coordinate report is still valid activity.
    if (read_err == ESP_ERR_INVALID_RESPONSE) return ESP_OK;
    return read_err == ESP_OK ? point_err : read_err;
}

bool ready() { return s_ready; }
} // namespace hardware::cst9217
