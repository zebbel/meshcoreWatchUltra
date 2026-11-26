#include "DisplayPower.h"
#include "TouchInputBuffer.h"

#include "CST9217.h"
#include "Power.h"
#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_pm.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <atomic>

namespace services::display_power {
namespace {
    const char *const TAG = "DisplayPower";
    // sleep until next real display timeout or touch IRQ.
    // recover touch edges that happen during light sleep.
    constexpr uint32_t kScreenOffRecheckMs = 250;

    constexpr uint32_t kChargerPollMs = 1000;
    ScreenControl s_screen_control = nullptr;
    ChargingDisplayControl s_charging_display = nullptr;
    std::atomic<bool> s_charging{false};
    std::atomic<bool> s_keep_awake{false};
    bool s_charging_display_applied = false; // protected by transition mutex
    TaskHandle_t s_task = nullptr;
    uint32_t s_timeout_ms = 10000;
    std::atomic<bool> s_screen_awake{true};
    std::atomic<uint32_t> s_last_activity_ms{0};
    bool s_started = false;
    detail::TouchInputBuffer s_touch_buffer;
    portMUX_TYPE s_touch_mux = portMUX_INITIALIZER_UNLOCKED;

    void touch_set_awake(bool awake) {
        portENTER_CRITICAL(&s_touch_mux);
        s_touch_buffer.set_awake(awake);
        portEXIT_CRITICAL(&s_touch_mux);
    }
    bool touch_tracking() {
        portENTER_CRITICAL(&s_touch_mux);
        const bool tracking = s_touch_buffer.tracking();
        portEXIT_CRITICAL(&s_touch_mux);
        return tracking;
    }
    void touch_report(const hardware::cst9217::TouchPoint &point, esp_err_t err,
                      bool wake_contact, uint32_t now) {
        portENTER_CRITICAL(&s_touch_mux);
        if (wake_contact) {
            if (!point.report_valid || point.pressed || err != ESP_OK)
                s_touch_buffer.suppress_wake(now);
        } else if (err != ESP_OK) {
            s_touch_buffer.cancel(now);
        } else if (point.report_valid) {
            s_touch_buffer.report(point.x, point.y, point.pressed, now);
        }
        portEXIT_CRITICAL(&s_touch_mux);
    }
    void touch_expire(uint32_t now) {
        portENTER_CRITICAL(&s_touch_mux);
        s_touch_buffer.expire(now);
        portEXIT_CRITICAL(&s_touch_mux);
    }

    // own the PM lock in the display service,
    // not in the main loop. A touch can wake the panel before app_main
    // observes the screen-awake flag and reacquires its legacy PM guard.
    // Serialize screen transitions, including external wake calls.
    SemaphoreHandle_t s_transition_mutex = nullptr;
#if CONFIG_PM_ENABLE && CONFIG_FREERTOS_USE_TICKLESS_IDLE && CONFIG_MCW_AUTO_LIGHT_SLEEP && CONFIG_PM_LIGHT_SLEEP_CALLBACKS
    esp_pm_lock_handle_t s_display_sleep_lock = nullptr;
    bool s_display_sleep_lock_held = false; // protected by transition mutex

    esp_err_t display_lock_acquire() {
        if (s_display_sleep_lock_held) return ESP_OK;
        const esp_err_t err = esp_pm_lock_acquire(s_display_sleep_lock);
        if (err == ESP_OK) {
            s_display_sleep_lock_held = true;
        } else {
            ESP_LOGE(TAG, "Display PM lock acquire failed: %s", esp_err_to_name(err));
        }
        return err;
    }

    void display_lock_release() {
        if (!s_display_sleep_lock_held) return;
        const esp_err_t err = esp_pm_lock_release(s_display_sleep_lock);
        if (err == ESP_OK) {
            s_display_sleep_lock_held = false;
        } else {
            // Keep the held flag: retry on another successful screen-off.
            ESP_LOGE(TAG, "Display PM lock release failed: %s", esp_err_to_name(err));
        }
    }
#else
    esp_err_t display_lock_acquire() { return ESP_OK; }
    void display_lock_release() {}
#endif

    uint32_t uptime_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL); }

    void policy_task(void *) {
        bool touch_communication_ok = true;
        uint32_t last_charger_poll = uptime_ms() - kChargerPollMs;
        unsigned charger_read_failures = 0;
        while (true) {
            if (s_keep_awake.load() && !s_screen_awake.load()) {
                (void)set_screen_awake(true);
            }

            const uint32_t poll_now = uptime_ms();
            if (s_charging_display && poll_now - last_charger_poll >= kChargerPollMs) {
                last_charger_poll = poll_now;
                bool charging = false;
                if (hardware::power::read_charging(charging) == ESP_OK) {
                    charger_read_failures = 0;
                    s_charging.store(charging);
                } else if (++charger_read_failures >= 3) {
                    // Do not keep the display on indefinitely with unknown charger state.
                    charger_read_failures = 3;
                    s_charging.store(false);
                }
                // Reconcile the mode under the existing transition mutex. Retry
                // transient wake/brightness failures on the next poll.
                if (s_charging.load() || s_screen_awake.load()) {
                    (void)set_screen_awake(true);
                }
            }
            const uint32_t idle_ms = static_cast<uint32_t>(uptime_ms() - s_last_activity_ms.load());
            const uint32_t remaining_ms = idle_ms < s_timeout_ms ? s_timeout_ms - idle_ms : 1;
            // Screen awake: wake at its timeout. Screen off: touch IRQ wakes
            // immediately; the long fallback also sees external wake requests.
            uint32_t wait_ms = s_screen_awake.load()
                ? ((s_charging.load() || s_keep_awake.load())
                    ? kChargerPollMs : remaining_ms)
                : kScreenOffRecheckMs;
            if (s_charging_display && wait_ms > kChargerPollMs) wait_ms = kChargerPollMs;
            // A bounded release watchdog only while tracking a contact. No
            // additional I2C reads: all coordinate reads remain IRQ-driven.
            if (s_screen_awake.load() && touch_tracking() && wait_ms > 25) wait_ms = 25;
            const TickType_t wait_ticks = pdMS_TO_TICKS(wait_ms) > 0 ? pdMS_TO_TICKS(wait_ms) : 1;
            const uint32_t events = ulTaskNotifyTake(pdTRUE, wait_ticks);
            // EXT0 can wake before digital IRQ is
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

                // The wake contact is consumed until its release (or a quiet
                // interval); it must not activate a widget on the new screen.
                const bool wake_contact = !s_screen_awake.load();
                if (wake_contact) {
                    if (set_screen_awake(true) == ESP_OK) {
                        touch_report(point, err, true, uptime_ms());
                    }
                } else {
                    touch_report(point, err, false, uptime_ms());
                }
                // Start a fresh five-second period after the wake command
                // finishes. The previous code could compare an older
                // loop timestamp with a newer touch timestamp and
                // underflow the unsigned inactivity calculation.
                notify_activity();
            }

            // Take the clock snapshot only after all touch processing.
            const uint32_t now = uptime_ms();
            touch_expire(now);
            // Fail safe: keep AMOLED on if touch ACK isn't reliable.
            if (s_screen_awake.load() &&
                !s_charging.load() &&
                !s_keep_awake.load() &&
                touch_communication_ok &&
                static_cast<uint32_t>(now - s_last_activity_ms.load()) >= s_timeout_ms) {
                (void)set_screen_awake(false);
            }
        }
    }
} // namespace

esp_err_t start(ScreenControl apply_screen_awake, uint32_t timeout_ms,
                ChargingDisplayControl charging_display) {
    if (s_started) return ESP_OK;
    if (apply_screen_awake == nullptr || timeout_ms == 0) return ESP_ERR_INVALID_ARG;

    esp_err_t err = hardware::cst9217::init();
    if (err != ESP_OK) return err;

    s_transition_mutex = xSemaphoreCreateMutex();
    if (s_transition_mutex == nullptr) return ESP_ERR_NO_MEM;
#if CONFIG_PM_ENABLE && CONFIG_FREERTOS_USE_TICKLESS_IDLE && CONFIG_MCW_AUTO_LIGHT_SLEEP && CONFIG_PM_LIGHT_SLEEP_CALLBACKS
    // Acquire before the main task enables automatic light sleep.
    err = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0,
                             "amoled-active", &s_display_sleep_lock);
    if (err != ESP_OK) {
        vSemaphoreDelete(s_transition_mutex);
        s_transition_mutex = nullptr;
        ESP_LOGE(TAG, "Display PM lock creation failed: %s", esp_err_to_name(err));
        return err;
    }
    err = display_lock_acquire();
    if (err != ESP_OK) {
        esp_pm_lock_delete(s_display_sleep_lock);
        s_display_sleep_lock = nullptr;
        vSemaphoreDelete(s_transition_mutex);
        s_transition_mutex = nullptr;
        return err;
    }
#endif

    s_screen_control = apply_screen_awake;
    s_charging_display = charging_display;
    s_timeout_ms = timeout_ms;
    s_last_activity_ms.store(uptime_ms());
    s_screen_awake.store(true);
    touch_set_awake(true);

    if (xTaskCreate(policy_task, "display_power", 8192, nullptr, 4, &s_task) != pdPASS) {
        s_screen_control = nullptr;
        touch_set_awake(false);
        display_lock_release();
#if CONFIG_PM_ENABLE && CONFIG_FREERTOS_USE_TICKLESS_IDLE && CONFIG_MCW_AUTO_LIGHT_SLEEP && CONFIG_PM_LIGHT_SLEEP_CALLBACKS
        if (!s_display_sleep_lock_held) {
            esp_pm_lock_delete(s_display_sleep_lock);
            s_display_sleep_lock = nullptr;
        }
#endif
        vSemaphoreDelete(s_transition_mutex);
        s_transition_mutex = nullptr;
        return ESP_ERR_NO_MEM;
    }
    hardware::cst9217::set_notification_task(s_task);
    s_started = true;
    ESP_LOGI(TAG, "Display timeout %lu ms; touch wake enabled", static_cast<unsigned long>(s_timeout_ms));
    return ESP_OK;
}

esp_err_t set_screen_awake(bool awake) {
    if (s_screen_control == nullptr || s_transition_mutex == nullptr)
        return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_transition_mutex, pdMS_TO_TICKS(3000)) != pdTRUE)
        return ESP_ERR_TIMEOUT;

    esp_err_t err = ESP_OK;
    const bool charging = s_charging.load();
    // Refuse screen-off requests while charging or keep-awake is active.
    if (!awake && (charging || s_keep_awake.load())) {
        xSemaphoreGive(s_transition_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    if (awake && s_screen_awake.load() && s_charging_display &&
        s_charging_display_applied != charging) {
        err = s_charging_display(charging);
        if (err == ESP_OK) {
            s_charging_display_applied = charging;
            notify_activity(); // Restore a fresh normal timeout at charge completion.
        }
    } else if (s_screen_awake.load() != awake) {
        if (awake) {
            // Crucial: acquire BEFORE panel reset, SPI writes or LVGL resume.
            // Even if app_main is still asleep, auto light sleep is inhibited.
            err = display_lock_acquire();
        }
        if (err == ESP_OK && !awake) touch_set_awake(false);
        if (err == ESP_OK) err = s_screen_control(awake);
        if (err == ESP_OK) {
            s_screen_awake.store(awake);
            if (awake) {
                touch_set_awake(true);
                // The wake callback renders the requested mode before revealing.
                s_charging_display_applied = charging;
                notify_activity();
            } else {
                // Release only after LVGL has stopped and CO5300 is asleep.
                display_lock_release();
            }
        } else if (awake) {
            // Fail-safe: a partially reinitialized panel must not go straight
            // back to CPU light sleep; the next touch can retry the wake.
            ESP_LOGW(TAG, "AMOLED wake incomplete; keeping CPU light sleep blocked");
        } else {
            // Panel failed to sleep: restore input, without replaying old taps.
            touch_set_awake(true);
        }
    }
    xSemaphoreGive(s_transition_mutex);
    return err;
}

TouchInput read_touch_input() {
    portENTER_CRITICAL(&s_touch_mux);
    const TouchInput input = s_touch_buffer.read();
    portEXIT_CRITICAL(&s_touch_mux);
    return input;
}

bool is_screen_awake() { return s_screen_awake.load(); }
bool is_charging() { return s_charging.load(); }
void notify_activity() { s_last_activity_ms.store(uptime_ms()); }

void set_keep_awake(bool enabled) {
    if (s_keep_awake.load() == enabled) return;

    notify_activity();
    s_keep_awake.store(enabled);
}

} // namespace services::display_power
