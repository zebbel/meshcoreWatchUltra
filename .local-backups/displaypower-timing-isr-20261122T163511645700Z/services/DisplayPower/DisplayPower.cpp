#include "DisplayPower.h"

#include "CST9217.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <atomic>

namespace services::display_power {
namespace {
    const char *const TAG = "DisplayPower";
    constexpr uint32_t kWaitPeriodMs = 50;

    ScreenControl s_screen_control = nullptr;
    TaskHandle_t s_task = nullptr;
    uint32_t s_timeout_ms = 5000;
    std::atomic<bool> s_screen_awake{true};
    std::atomic<uint32_t> s_last_activity_ms{0};
    bool s_started = false;

    uint32_t uptime_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL); }

    void policy_task(void *) {
        bool touch_communication_ok = true;
        while (true) {
            const uint32_t events = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kWaitPeriodMs));
            const uint32_t now = uptime_ms();
            if (events > 0) {
                hardware::cst9217::TouchPoint point{};
                const esp_err_t err = hardware::cst9217::read_interrupt(point);
                if (err != ESP_OK) {
                    if (touch_communication_ok) {
                        ESP_LOGW(TAG, "Touch read/ACK failed: %s; keeping screen awake",
                                 esp_err_to_name(err));
                    }
                    touch_communication_ok = false;
                } else if (!touch_communication_ok) {
                    touch_communication_ok = true;
                    ESP_LOGI(TAG, "Touch communication recovered");
                }

                // The first tap on a sleeping display wakes it without being
                // sent to a UI control. Subsequent taps are reserved for a
                // future LVGL input adapter; no new UI actions in this refactor.
                notify_activity();
                if (!s_screen_awake.load()) {
                    if (set_screen_awake(true) == ESP_OK) {
                        ESP_LOGI(TAG, "AMOLED woke on touch");
                    }
                }
            }

            // Fail safe: keep AMOLED on if touch ACK isn't reliable.
            if (s_screen_awake.load() && touch_communication_ok &&
                static_cast<uint32_t>(now - s_last_activity_ms.load()) >= s_timeout_ms) {
                if (set_screen_awake(false) == ESP_OK) {
                    ESP_LOGI(TAG, "AMOLED off after %lu ms idle",
                             static_cast<unsigned long>(s_timeout_ms));
                }
            }
        }
    }
} // namespace

esp_err_t start(ScreenControl apply_screen_awake, uint32_t timeout_ms) {
    if (s_started) return ESP_OK;
    if (apply_screen_awake == nullptr || timeout_ms == 0) return ESP_ERR_INVALID_ARG;

    esp_err_t err = hardware::cst9217::init();
    if (err != ESP_OK) return err;

    s_screen_control = apply_screen_awake;
    s_timeout_ms = timeout_ms;
    s_last_activity_ms.store(uptime_ms());
    s_screen_awake.store(true);

    if (xTaskCreate(policy_task, "display_power", 4096, nullptr, 4, &s_task) != pdPASS) {
        s_screen_control = nullptr;
        return ESP_ERR_NO_MEM;
    }
    hardware::cst9217::set_notification_task(s_task);
    s_started = true;
    ESP_LOGI(TAG, "5-second display timeout and touch wake active (GPIO12 IRQ)");
    return ESP_OK;
}

esp_err_t set_screen_awake(bool awake) {
    if (s_screen_control == nullptr) return ESP_ERR_INVALID_STATE;
    if (s_screen_awake.load() == awake) return ESP_OK;
    const esp_err_t err = s_screen_control(awake);
    if (err == ESP_OK) s_screen_awake.store(awake);
    return err;
}

bool is_screen_awake() { return s_screen_awake.load(); }
void notify_activity() { s_last_activity_ms.store(uptime_ms()); }
} // namespace services::display_power
