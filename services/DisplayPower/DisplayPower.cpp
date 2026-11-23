#include "DisplayPower.h"

#include "CST9217.h"
#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <atomic>

namespace services::display_power {
namespace {
    const char *const TAG = "DisplayPower";
    // MCW_CPU_LORA_IDLE_V1: sleep until next real display timeout or touch IRQ.
    // MCW_AUTO_LIGHT_SLEEP_V1: recover touch edges that happen during light sleep.
    constexpr uint32_t kScreenOffRecheckMs = 250;

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
            const uint32_t idle_ms = static_cast<uint32_t>(uptime_ms() - s_last_activity_ms.load());
            const uint32_t remaining_ms = idle_ms < s_timeout_ms ? s_timeout_ms - idle_ms : 1;
            // Screen awake: wake at its timeout. Screen off: touch IRQ wakes
            // immediately; the long fallback also sees external wake requests.
            const uint32_t wait_ms = s_screen_awake.load() ? remaining_ms : kScreenOffRecheckMs;
            const TickType_t wait_ticks = pdMS_TO_TICKS(wait_ms) > 0 ? pdMS_TO_TICKS(wait_ms) : 1;
            const uint32_t events = ulTaskNotifyTake(pdTRUE, wait_ticks);
            // MCW_AUTO_LIGHT_SLEEP_V1: EXT0 can wake before digital IRQ is
            // restored. A latched active-low GPIO12 still needs an I2C ACK.
            const bool pending_touch_level = !s_screen_awake.load() &&
                gpio_get_level(board::pins::TOUCH_IRQ) == 0;
            if (events > 0 || pending_touch_level) {
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
                if (!s_screen_awake.load()) {
                    if (set_screen_awake(true) == ESP_OK) {
                        ESP_LOGI(TAG, "AMOLED woke on touch");
                    }
                }
                // Start a fresh five-second period after the wake command
                // finishes. The previous code could compare an older
                // loop timestamp with a newer touch timestamp and
                // underflow the unsigned inactivity calculation.
                notify_activity();
            }

            // Take the clock snapshot only after all touch processing.
            const uint32_t now = uptime_ms();
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
    if (err == ESP_OK) {
        s_screen_awake.store(awake);
        // Any explicit wake request should restart the idle timeout,
        // including future button or application wake sources.
        if (awake) notify_activity();
    }
    return err;
}

bool is_screen_awake() { return s_screen_awake.load(); }
void notify_activity() { s_last_activity_ms.store(uptime_ms()); }
} // namespace services::display_power
