#include "DisplayPower.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_pm.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <atomic>

#include "TouchInputBuffer.h"
#include "CST9217.h"
#include "Power.h"
#include "board_pins.h"

namespace services::display_power {
    namespace {
        const char *const TAG = "DisplayPower";
        // Timing constants: IRQ-driven wake, with fallback for missed touch edges.
        constexpr uint32_t kScreenOffRecheckMs = 250;
        // Wrist requests use a separate notification bit from touch IRQs.
        constexpr uint32_t kWristWakeNotification = 1U << 1;
        static_assert((kWristWakeNotification & hardware::cst9217::kTouchIrqNotification) == 0, "Display task notification bits must not overlap");

        constexpr uint32_t kChargerPollMs = 1000;
        constexpr uint32_t kTouchReleaseCheckMs = 25;
        // Callbacks implemented by WatchDisplay: policy does not render pixels.
        ScreenControl s_screen_control = nullptr;
        ChargingDisplayControl s_charging_display = nullptr;
        // Shared policy state: atomic values cross FreeRTOS task boundaries.
        std::atomic<bool> s_charging{false};
        std::atomic<bool> s_keep_awake{false};
        bool s_charging_display_applied = false; // protected by transition mutex
        TaskHandle_t s_task = nullptr;
        uint32_t s_timeout_ms = 10000;
        std::atomic<bool> s_screen_awake{true};
        // Main-task request; notification wakes the policy task immediately.
        std::atomic<bool> s_wrist_wake_pending{false};
        std::atomic<uint32_t> s_last_activity_ms{0};
        bool s_started = false;
        // Touch reports pass from the policy task to LVGL through this buffer.
        detail::TouchInputBuffer s_touch_buffer;
        portMUX_TYPE s_touch_mux = portMUX_INITIALIZER_UNLOCKED;

        // ------------------------------------------------------------------------
        // Touch buffer synchronization
        // ------------------------------------------------------------------------

        // Only RAM operations are performed inside the critical section.
        void touch_set_awake(bool awake){
            portENTER_CRITICAL(&s_touch_mux);
            s_touch_buffer.set_awake(awake);
            portEXIT_CRITICAL(&s_touch_mux);
        }
        bool touch_tracking(){
            portENTER_CRITICAL(&s_touch_mux);
            const bool tracking = s_touch_buffer.tracking();
            portEXIT_CRITICAL(&s_touch_mux);
            return tracking;
        }
        void touch_report(const hardware::cst9217::TouchPoint &point, esp_err_t err, bool wake_contact, uint32_t now){
            portENTER_CRITICAL(&s_touch_mux);
            if(wake_contact){
                if(!point.report_valid || point.pressed || err != ESP_OK) s_touch_buffer.suppress_wake(now);
            }else if(err != ESP_OK){
                s_touch_buffer.cancel(now);
            }else if(point.report_valid){
                s_touch_buffer.report(point.x, point.y, point.pressed, now);
            }
            portEXIT_CRITICAL(&s_touch_mux);
        }
        void touch_expire(uint32_t now){
            portENTER_CRITICAL(&s_touch_mux);
            s_touch_buffer.expire(now);
            portEXIT_CRITICAL(&s_touch_mux);
        }

        // ------------------------------------------------------------------------
        // Transition synchronization and ESP-IDF power locks
        // ------------------------------------------------------------------------

        // Hold a dedicated display PM lock during panel transitions, before the main task can observe a wake.
        // Serialize screen transitions, including external wake calls.
        SemaphoreHandle_t s_transition_mutex = nullptr;
#if CONFIG_PM_ENABLE
        // Created lazily under the transition mutex; retained for service lifetime.
        esp_pm_lock_handle_t s_wake_cpu_lock = nullptr;
        bool s_wake_cpu_lock_held = false;
#endif
#if CONFIG_PM_ENABLE && CONFIG_FREERTOS_USE_TICKLESS_IDLE && CONFIG_MCW_AUTO_LIGHT_SLEEP && CONFIG_PM_LIGHT_SLEEP_CALLBACKS
        esp_pm_lock_handle_t s_display_sleep_lock = nullptr;
        bool s_display_sleep_lock_held = false; // protected by transition mutex

        esp_err_t display_lock_acquire(){
            if(s_display_sleep_lock_held) return ESP_OK;
            const esp_err_t err = esp_pm_lock_acquire(s_display_sleep_lock);
            if(err == ESP_OK){
                s_display_sleep_lock_held = true;
            }else{
                ESP_LOGE(TAG, "Display PM lock acquire failed: %s", esp_err_to_name(err));
            }
            return err;
        }

        void display_lock_release(){
            if(!s_display_sleep_lock_held) return;
            const esp_err_t err = esp_pm_lock_release(s_display_sleep_lock);
            if(err == ESP_OK){
                s_display_sleep_lock_held = false;
            }else{
                // Keep the held flag: retry on another successful screen-off.
                ESP_LOGE(TAG, "Display PM lock release failed: %s", esp_err_to_name(err));
            }
        }
#else
        esp_err_t display_lock_acquire(){ return ESP_OK; }
        void display_lock_release(){}
#endif

        // ------------------------------------------------------------------------
        // Wake-only CPU frequency boost
        // ------------------------------------------------------------------------

        // Called with the transition mutex held. The existing display sleep
        // lock is acquired first; this optional lock only increases CPU speed.
        esp_err_t apply_screen_control(bool awake){
#if CONFIG_PM_ENABLE
            if(awake && s_wake_cpu_lock == nullptr){
                const esp_err_t create_err = esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "display-wake", &s_wake_cpu_lock);
                if(create_err != ESP_OK) ESP_LOGW(TAG, "Display CPU boost unavailable: %s", esp_err_to_name(create_err));
            }
            if(awake && s_wake_cpu_lock != nullptr && !s_wake_cpu_lock_held){
                const esp_err_t lock_err = esp_pm_lock_acquire(s_wake_cpu_lock);
                if(lock_err == ESP_OK) s_wake_cpu_lock_held = true;
                else ESP_LOGW(TAG, "Display CPU boost acquire failed: %s", esp_err_to_name(lock_err));
            }
#endif
            // Deep-standby exit, Flow refresh, hidden redraw and DMA-fenced reveal.
            const esp_err_t err = s_screen_control(awake);
#if CONFIG_PM_ENABLE
            // Release even when the panel wake fails. A failed release is
            // retried on the next serialized transition, without reacquiring.
            if(s_wake_cpu_lock_held){
                const esp_err_t release_err = esp_pm_lock_release(s_wake_cpu_lock);
                if(release_err == ESP_OK) s_wake_cpu_lock_held = false;
                else ESP_LOGW(TAG, "Display CPU boost release failed: %s", esp_err_to_name(release_err));
            }
#endif
            return err;
        }

        // ------------------------------------------------------------------------
        // Policy scheduling
        // ------------------------------------------------------------------------

        uint32_t uptime_ms(){ return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL); }

        uint32_t next_wait_ms(){
            const uint32_t idle_ms = static_cast<uint32_t>(uptime_ms() - s_last_activity_ms.load());
            const uint32_t remaining_ms = idle_ms < s_timeout_ms ? s_timeout_ms - idle_ms : 1;
            // Screen awake: wake at its timeout. Screen off: touch and wrist
            // notifications wake immediately; retain a bounded IRQ-level fallback.
            uint32_t wait_ms = kScreenOffRecheckMs;
            if(s_screen_awake.load()){
                wait_ms = (s_charging.load() || s_keep_awake.load()) ? kChargerPollMs : remaining_ms;
            }
            if(s_charging_display && wait_ms > kChargerPollMs) wait_ms = kChargerPollMs;
            // A bounded release watchdog only while tracking a contact. No
            // additional I2C reads: all coordinate reads remain IRQ-driven.
            if(s_screen_awake.load() && touch_tracking() && wait_ms > kTouchReleaseCheckMs) wait_ms = kTouchReleaseCheckMs;
            return wait_ms;
        }

        // ------------------------------------------------------------------------
        // Display policy task
        // ------------------------------------------------------------------------

        // Policy task helpers. Only this task owns charger polling and touch health.

        // Wake for external requests (for example BLE pairing).
        void process_keep_awake(){
            if(s_keep_awake.load() && !s_screen_awake.load()){
                (void)set_screen_awake(true);
            }
        }

        // Read the PMU periodically and reconcile charging display mode.
        void process_charging(uint32_t &last_charger_poll, unsigned &charger_read_failures){
            const uint32_t poll_now = uptime_ms();
            if(s_charging_display && poll_now - last_charger_poll >= kChargerPollMs){
                last_charger_poll = poll_now;
                bool charging = false;
                if(hardware::power::read_charging(charging) == ESP_OK){
                    charger_read_failures = 0;
                    s_charging.store(charging);
                }else if(++charger_read_failures >= 3){
                    // Do not keep the display on indefinitely with unknown charger state.
                    charger_read_failures = 3;
                    s_charging.store(false);
                }
                // Reconcile the mode under the existing transition mutex. Retry
                // transient wake/brightness failures on the next poll.
                if(s_charging.load() || s_screen_awake.load()){
                    (void)set_screen_awake(true);
                }
            }
        }

        // Block until an IRQ, wrist request or scheduled policy check.
        uint32_t wait_for_events(){
            const uint32_t wait_ms = next_wait_ms();
            const TickType_t wait_ticks = pdMS_TO_TICKS(wait_ms) > 0 ? pdMS_TO_TICKS(wait_ms) : 1;
            uint32_t events = 0;
            (void)xTaskNotifyWait(0, UINT32_MAX, &events, wait_ticks);
            return events;
        }

        // Acknowledge touch hardware and forward only non-wake contacts to LVGL.
        void process_touch(uint32_t events, bool &touch_communication_ok){
            // EXT0 can wake before digital IRQ is
            // restored. A latched active-low GPIO12 still needs an I2C ACK.
            const bool pending_touch_level = !s_screen_awake.load() && gpio_get_level(hardware::board::pins::TOUCH_IRQ) == 0;
            if((events & hardware::cst9217::kTouchIrqNotification) || pending_touch_level){
                hardware::cst9217::TouchPoint point{};
                const esp_err_t err = hardware::cst9217::read_interrupt(point);
                if(err != ESP_OK){
                    if(touch_communication_ok){
                        ESP_LOGW(TAG, "Touch read/ACK failed: %s; keeping screen awake", esp_err_to_name(err));
                    }
                    touch_communication_ok = false;
                }else if(!touch_communication_ok){
                    touch_communication_ok = true;
                    ESP_LOGI(TAG, "Touch communication recovered");
                }

                // The wake contact is consumed until its release (or a quiet
                // interval); it must not activate a widget on the new screen.
                const bool wake_contact = !s_screen_awake.load();
                if(wake_contact){
                    if(set_screen_awake(true) == ESP_OK){
                        touch_report(point, err, true, uptime_ms());
                    }
                }else{
                    touch_report(point, err, false, uptime_ms());
                }
                // Start a fresh inactivity timeout after the wake command finishes.
                // Sample the clock afterward to avoid unsigned underflow.
                notify_activity();
            }
        }

        // Consume a wrist request without extending an already awake screen.
        void process_wrist_wake(){
            // Touch retains priority. Do not treat a wrist request as a touch IRQ.
            // Ignore gestures while awake so they cannot refresh the inactivity timeout.
            if(s_wrist_wake_pending.exchange(false) && !s_screen_awake.load()){
                const esp_err_t err = set_screen_awake(true);
                if(err == ESP_OK) ESP_LOGI(TAG, "Display wake requested by wrist tilt");
                else ESP_LOGW(TAG, "Wrist wake failed: %s", esp_err_to_name(err));
            }
        }

        // Expire buffered contacts and enter standby only when policy permits.
        void process_inactivity_timeout(bool touch_communication_ok){
            // Take the clock snapshot only after all touch and wrist processing.
            const uint32_t now = uptime_ms();
            touch_expire(now);
            // Fail safe: keep AMOLED on if touch ACK isn't reliable.
            if(s_screen_awake.load() && !s_charging.load() && !s_keep_awake.load() && touch_communication_ok && static_cast<uint32_t>(now - s_last_activity_ms.load()) >= s_timeout_ms){
                (void)set_screen_awake(false);
            }
        }

        // Display policy: event-driven orchestration
        void policy_task(void *){
            bool touch_communication_ok = true;
            uint32_t last_charger_poll = uptime_ms() - kChargerPollMs;
            unsigned charger_read_failures = 0;

            while(true){
                process_keep_awake();
                process_charging(last_charger_poll, charger_read_failures);
                const uint32_t events = wait_for_events();
                // Touch takes priority over wrist wake. Check timeout last.
                process_touch(events, touch_communication_ok);
                process_wrist_wake();
                process_inactivity_timeout(touch_communication_ok);
            }
        }
    } // namespace

    // ------------------------------------------------------------------------
    // Service initialization
    // ------------------------------------------------------------------------

    esp_err_t start(ScreenControl apply_screen_awake, uint32_t timeout_ms, ChargingDisplayControl charging_display){
        if(s_started) return ESP_OK;
        if(apply_screen_awake == nullptr || timeout_ms == 0) return ESP_ERR_INVALID_ARG;

        esp_err_t err = hardware::cst9217::init();
        if(err != ESP_OK) return err;

        s_transition_mutex = xSemaphoreCreateMutex();
        if(s_transition_mutex == nullptr) return ESP_ERR_NO_MEM;
#if CONFIG_PM_ENABLE && CONFIG_FREERTOS_USE_TICKLESS_IDLE && CONFIG_MCW_AUTO_LIGHT_SLEEP && CONFIG_PM_LIGHT_SLEEP_CALLBACKS
        // Acquire before the main task enables automatic light sleep.
        err = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "amoled-active", &s_display_sleep_lock);
        if(err != ESP_OK){
            vSemaphoreDelete(s_transition_mutex);
            s_transition_mutex = nullptr;
            ESP_LOGE(TAG, "Display PM lock creation failed: %s", esp_err_to_name(err));
            return err;
        }
        err = display_lock_acquire();
        if(err != ESP_OK){
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

        if(xTaskCreate(policy_task, "display_power", 8192, nullptr, 4, &s_task) != pdPASS){
            s_screen_control = nullptr;
            touch_set_awake(false);
            display_lock_release();
#if CONFIG_PM_ENABLE && CONFIG_FREERTOS_USE_TICKLESS_IDLE && CONFIG_MCW_AUTO_LIGHT_SLEEP && CONFIG_PM_LIGHT_SLEEP_CALLBACKS
            if(!s_display_sleep_lock_held){
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

    // ------------------------------------------------------------------------
    // Serialized screen transitions
    // ------------------------------------------------------------------------

    namespace {
        // All transition helpers below require the transition mutex.
        // The public function owns the mutex; helpers must never take it again.
        esp_err_t update_charging_mode(bool charging){
            const esp_err_t err = s_charging_display(charging);
            if(err == ESP_OK){
                s_charging_display_applied = charging;
                notify_activity(); // Fresh normal timeout after charge completion.
            }
            return err;
        }

        // Prevent CPU sleep before touching the panel or resuming LVGL.
        esp_err_t wake_screen(bool charging){
            esp_err_t err = display_lock_acquire();
            if(err == ESP_OK) err = apply_screen_control(true);
            if(err != ESP_OK){
                // A partially initialized panel must remain protected for retry.
                ESP_LOGW(TAG, "AMOLED wake incomplete; keeping CPU light sleep blocked");
                return err;
            }
            s_wrist_wake_pending.store(false);
            s_screen_awake.store(true);
            touch_set_awake(true);
            s_charging_display_applied = charging;
            notify_activity();
            return ESP_OK;
        }

        // Stop input before standby; release the PM lock only on success.
        esp_err_t sleep_screen(){
            touch_set_awake(false);
            const esp_err_t err = apply_screen_control(false);
            if(err != ESP_OK){
                // Failed sleep leaves the screen awake; restore fresh input.
                touch_set_awake(true);
                return err;
            }
            s_wrist_wake_pending.store(false);
            s_screen_awake.store(false);
            display_lock_release();
            return ESP_OK;
        }

        // Decide which operation is needed, using one charging snapshot.
        esp_err_t apply_transition(bool awake){
            const bool charging = s_charging.load();
            if(!awake && (charging || s_keep_awake.load())) return ESP_ERR_INVALID_STATE;
            if(awake && s_screen_awake.load() && s_charging_display && s_charging_display_applied != charging) return update_charging_mode(charging);
            if(s_screen_awake.load() != awake) return awake ? wake_screen(charging) : sleep_screen();
            return ESP_OK;
        }
    } // namespace

    // Public entry point: serialize every display transition, including errors.
    esp_err_t set_screen_awake(bool awake){
        if(s_screen_control == nullptr || s_transition_mutex == nullptr) return ESP_ERR_INVALID_STATE;
        if(xSemaphoreTake(s_transition_mutex, pdMS_TO_TICKS(3000)) != pdTRUE) return ESP_ERR_TIMEOUT;
        const esp_err_t err = apply_transition(awake);
        xSemaphoreGive(s_transition_mutex);
        return err;
    }

    // ------------------------------------------------------------------------
    // UI input and state interface
    // ------------------------------------------------------------------------

    TouchInput read_touch_input(){
        portENTER_CRITICAL(&s_touch_mux);
        const TouchInput input = s_touch_buffer.read();
        portEXIT_CRITICAL(&s_touch_mux);
        return input;
    }

    // Queue only while asleep. Wake the policy task without generating a touch read.
    void request_wrist_wake(){
        if(!s_started || s_screen_awake.load() || s_task == nullptr) return;
        s_wrist_wake_pending.store(true);
        (void)xTaskNotify(s_task, kWristWakeNotification, eSetBits);
    }

    bool is_screen_awake(){ return s_screen_awake.load(); }
    bool is_charging(){ return s_charging.load(); }
    void notify_activity(){ s_last_activity_ms.store(uptime_ms()); }

    void set_keep_awake(bool enabled){
        if(s_keep_awake.load() == enabled) return;

        notify_activity();
        s_keep_awake.store(enabled);
    }

} // namespace services::display_power
