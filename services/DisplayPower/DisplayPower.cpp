#include "DisplayPower.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "AXP2101.h"
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
        constexpr uint32_t screen_off_recheck_ms = 250;
        // Wrist requests use a separate notification bit from touch IRQs.
        constexpr uint32_t wrist_wake_notification = 1U << 1;
        static_assert((wrist_wake_notification & hardware::cst9217::touch_irq_notification) == 0, "Display task notification bits must not overlap");

        constexpr uint32_t charger_poll_ms = 1000;
        constexpr uint32_t touch_release_check_ms = 25;
        // Callbacks implemented by WatchDisplay: policy does not render pixels.
        ScreenControl screen_control = nullptr;
        // Shared policy state: atomic values cross FreeRTOS task boundaries.
        std::atomic<bool> charging_active{false};
        TaskHandle_t task_handle = nullptr;
        std::atomic<bool> screen_awake{true};
        // Main-task request; notification wakes the policy task immediately.
        std::atomic<bool> wrist_wake_pending{false};
        std::atomic<bool> started{false};
        // EEZ owns all inactivity policy. Its setter queues a command only.
        std::atomic<bool> sleep_requested{false};
        std::atomic<bool> external_wake_pending{false};
        std::atomic<int32_t> touch_events{0};
        std::atomic<int32_t> wrist_events{0};
        std::atomic<int32_t> wake_events{0};
        // Only the display task changes this revision; input wins over a
        // standby command captured in the same service iteration.
        uint32_t input_revision = 0;
        constexpr uint32_t sleep_request_notification = 1U << 4;
        constexpr uint32_t external_wake_notification = 1U << 5;
        // Side buttons: event counters survive a paused EEZ renderer.
        constexpr uint32_t button_debounce_ms = 30;
        constexpr uint32_t button_poll_ms = 25;
        static_assert(((sleep_request_notification | external_wake_notification) & (wrist_wake_notification | hardware::cst9217::touch_irq_notification)) == 0, "Display notification collision");
        std::atomic<int32_t> boot_count{0};
        std::atomic<int32_t> power_count{0};
        std::atomic<bool> boot_ready{false};
        std::atomic<bool> power_ready{false};
        // Debounce state belongs exclusively to the display policy task.
        bool boot_latched = false;
        bool boot_release_pending = false;
        uint32_t boot_release_ms = 0;

        // Touch reports pass from the policy task to LVGL through this buffer.
        detail::TouchInputBuffer touch_buffer;
        portMUX_TYPE touch_mux = portMUX_INITIALIZER_UNLOCKED;

        // ------------------------------------------------------------------------
        // Touch buffer synchronization
        // ------------------------------------------------------------------------

        // Only RAM operations are performed inside the critical section.
        void touch_set_awake(bool awake){
            portENTER_CRITICAL(&touch_mux);
            touch_buffer.set_awake(awake);
            portEXIT_CRITICAL(&touch_mux);
        }
        bool touch_tracking(){
            portENTER_CRITICAL(&touch_mux);
            const bool tracking = touch_buffer.tracking();
            portEXIT_CRITICAL(&touch_mux);
            return tracking;
        }
        void touch_report(const hardware::cst9217::TouchPoint &point, esp_err_t err, bool wake_contact, uint32_t now){
            portENTER_CRITICAL(&touch_mux);
            if(wake_contact){
                if(!point.report_valid || point.pressed || err != ESP_OK) touch_buffer.suppress_wake(now);
            }else if(err != ESP_OK){
                touch_buffer.cancel(now);
            }else if(point.report_valid){
                touch_buffer.report(point.x, point.y, point.pressed, now);
            }
            portEXIT_CRITICAL(&touch_mux);
        }
        void touch_expire(uint32_t now){
            portENTER_CRITICAL(&touch_mux);
            touch_buffer.expire(now);
            portEXIT_CRITICAL(&touch_mux);
        }

        // ------------------------------------------------------------------------
        // Transition synchronization and ESP-IDF power locks
        // ------------------------------------------------------------------------

        // Hold a dedicated display PM lock during panel transitions, before the main task can observe a wake.
        // Serialize screen transitions, including external wake calls.
        SemaphoreHandle_t transition_mutex = nullptr;
#if CONFIG_PM_ENABLE
        // Created lazily under the transition mutex; retained for service lifetime.
        esp_pm_lock_handle_t wake_cpu_lock = nullptr;
        bool wake_cpu_lock_held = false;
#endif
#if CONFIG_PM_ENABLE && CONFIG_FREERTOS_USE_TICKLESS_IDLE && CONFIG_MCW_AUTO_LIGHT_SLEEP && CONFIG_PM_LIGHT_SLEEP_CALLBACKS
        esp_pm_lock_handle_t display_sleep_lock = nullptr;
        bool display_sleep_lock_held = false; // protected by transition mutex

        esp_err_t display_lock_acquire(){
            if(display_sleep_lock_held) return ESP_OK;
            const esp_err_t err = esp_pm_lock_acquire(display_sleep_lock);
            if(err == ESP_OK){
                display_sleep_lock_held = true;
            }else{
                ESP_LOGE(TAG, "Display PM lock acquire failed: %s", esp_err_to_name(err));
            }
            return err;
        }

        void display_lock_release(){
            if(!display_sleep_lock_held) return;
            const esp_err_t err = esp_pm_lock_release(display_sleep_lock);
            if(err == ESP_OK){
                display_sleep_lock_held = false;
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
            if(awake && wake_cpu_lock == nullptr){
                const esp_err_t create_err = esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "display-wake", &wake_cpu_lock);
                if(create_err != ESP_OK) ESP_LOGW(TAG, "Display CPU boost unavailable: %s", esp_err_to_name(create_err));
            }
            if(awake && wake_cpu_lock != nullptr && !wake_cpu_lock_held){
                const esp_err_t lock_err = esp_pm_lock_acquire(wake_cpu_lock);
                if(lock_err == ESP_OK) wake_cpu_lock_held = true;
                else ESP_LOGW(TAG, "Display CPU boost acquire failed: %s", esp_err_to_name(lock_err));
            }
#endif
            // Deep-standby exit, Flow refresh, hidden redraw and DMA-fenced reveal.
            const esp_err_t err = screen_control(awake);
#if CONFIG_PM_ENABLE
            // Release even when the panel wake fails. A failed release is
            // retried on the next serialized transition, without reacquiring.
            if(wake_cpu_lock_held){
                const esp_err_t release_err = esp_pm_lock_release(wake_cpu_lock);
                if(release_err == ESP_OK) wake_cpu_lock_held = false;
                else ESP_LOGW(TAG, "Display CPU boost release failed: %s", esp_err_to_name(release_err));
            }
#endif
            return err;
        }

        // ------------------------------------------------------------------------
        // Hardware event scheduling
        // ------------------------------------------------------------------------

        uint32_t uptime_ms(){ return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL); }


        // Poll hardware only; there is no display inactivity timer here.
        uint32_t next_wait_ms(){
            uint32_t wait_ms = screen_off_recheck_ms;
            if(screen_awake.load() && touch_tracking() && wait_ms > touch_release_check_ms) wait_ms = touch_release_check_ms;
            if((boot_ready.load() || power_ready.load()) && wait_ms > button_poll_ms) wait_ms = button_poll_ms;
            return wait_ms;
        }

        // Sequence values remain representable by EEZ's signed Integer type.
        void increment_event(std::atomic<int32_t> &counter){
            const int32_t previous = counter.load();
            counter.store(previous == INT32_MAX ? 1 : previous + 1);
        }

        // Publish the event before waking the panel so the hidden Flow refresh
        // can select a page without briefly displaying the previous one.
        void record_input(std::atomic<int32_t> &counter){
            ++input_revision;
            sleep_requested.store(false);
            increment_event(counter);
        }

        // ------------------------------------------------------------------------
        // Hardware event handlers
        // ------------------------------------------------------------------------

        // Only the display task owns charger polling and touch health.


        // Publish charger state; wake on charge start so EEZ can react.
        void process_charging(uint32_t &last_charger_poll, unsigned &charger_read_failures){
            const uint32_t now = uptime_ms();
            if(now - last_charger_poll < charger_poll_ms) return;
            last_charger_poll = now;
            bool charging = false;
            if(hardware::power::read_charging(charging) == ESP_OK){
                charger_read_failures = 0;
                const bool previous = charging_active.exchange(charging);
                if(charging && !previous){
                    ++input_revision;
                    sleep_requested.store(false);
                    (void)set_screen_awake(true);
                }
            }else if(++charger_read_failures >= 3){
                charger_read_failures = 3;
                charging_active.store(false);
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
            const bool pending_touch_level = !screen_awake.load() && gpio_get_level(hardware::board::pins::TOUCH_IRQ) == 0;
            if((events & hardware::cst9217::touch_irq_notification) || pending_touch_level){
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
                const bool wake_contact = !screen_awake.load();
                // Count a wake contact even without coordinates. While awake,
                // count valid pressed reports (including movement), not releases.
                if(wake_contact || (err == ESP_OK && point.report_valid && point.pressed)) record_input(touch_events);
                if(wake_contact){
                    if(set_screen_awake(true) == ESP_OK){
                        touch_report(point, err, true, uptime_ms());
                    }
                }else{
                    touch_report(point, err, false, uptime_ms());
                }
            }
        }

        // Forward wrist events even while awake; EEZ chooses whether to react.
        void process_wrist_wake(){
            if(!wrist_wake_pending.exchange(false)) return;
            record_input(wrist_events);
            if(!screen_awake.load()){
                const esp_err_t err = set_screen_awake(true);
                if(err != ESP_OK) ESP_LOGW(TAG, "Wrist wake failed: %s", esp_err_to_name(err));
            }
        }

        // Pairing can wake the display, but does not select or lock a page.
        void process_external_wake(){
            if(!external_wake_pending.exchange(false)) return;
            ++input_revision;
            sleep_requested.store(false);
            (void)set_screen_awake(true);
        }


        // ------------------------------------------------------------------------
        // Side buttons: GPIO polling, PMU acknowledgement and debounce
        // ------------------------------------------------------------------------

        // Configure active-low inputs for polling. We intentionally avoid
        // registering additional GPIO ISRs here because the previous ISR-based
        // implementation caused unstable startup behavior on the watch.
        bool init_button_pin(gpio_num_t pin){
            gpio_config_t cfg{};
            cfg.pin_bit_mask = 1ULL << pin;
            cfg.mode = GPIO_MODE_INPUT;
            cfg.pull_up_en = GPIO_PULLUP_ENABLE;
            cfg.intr_type = GPIO_INTR_DISABLE;
            esp_err_t err = gpio_config(&cfg);
            if(err != ESP_OK){
                ESP_LOGW(TAG, "Button GPIO%d setup failed: %s", static_cast<int>(pin), esp_err_to_name(err));
                return false;
            }
#if CONFIG_PM_ENABLE && CONFIG_FREERTOS_USE_TICKLESS_IDLE && CONFIG_MCW_AUTO_LIGHT_SLEEP && CONFIG_PM_LIGHT_SLEEP_CALLBACKS
            // Digital GPIO light-sleep wake supports active-low buttons without
            // changing the existing EXT0 touch or EXT1 LoRa/sensor configuration.
            err = gpio_wakeup_enable(pin, GPIO_INTR_LOW_LEVEL);
            if(err == ESP_OK) err = esp_sleep_enable_gpio_wakeup();
            if(err != ESP_OK){
                (void)gpio_wakeup_disable(pin);
                ESP_LOGW(TAG, "Button GPIO%d sleep wake unavailable: %s", static_cast<int>(pin), esp_err_to_name(err));
            }
#endif
            return true;
        }

        // Run inside the policy task before its first wait. Ignore a BOOT key
        // held during startup so the first user release does not create an event.
        void init_buttons(){
            boot_ready.store(init_button_pin(hardware::board::pins::BOOT_BUTTON));
            boot_latched = boot_ready.load() && gpio_get_level(hardware::board::pins::BOOT_BUTTON) == 0;
            if(hardware::axp2101::configure_power_key_irq() == ESP_OK){
                power_ready.store(init_button_pin(hardware::board::pins::PMU_IRQ));
            }else{
                ESP_LOGW(TAG, "Power-key IRQ configuration failed");
            }
            ESP_LOGI(TAG, "Buttons ready (polled): boot=%d power=%d", boot_ready.load(), power_ready.load());
        }

        // Publish before the hidden Flow refresh so the first press is visible.
        void publish_button(std::atomic<int32_t> &counter){
            record_input(counter);
            const esp_err_t err = set_screen_awake(true);
            if(err != ESP_OK) ESP_LOGW(TAG, "Button display wake failed: %s", esp_err_to_name(err));
        }

        bool consume_boot_button(){
            if(!boot_ready.load()) return false;
            const bool low = gpio_get_level(hardware::board::pins::BOOT_BUTTON) == 0;
            const uint32_t now = uptime_ms();
            if(boot_latched){
                // Require a stable release before accepting another press.
                if(low){
                    boot_release_pending = false;
                }else if(!boot_release_pending){
                    boot_release_pending = true;
                    boot_release_ms = now;
                }else if(now - boot_release_ms >= button_debounce_ms){
                    boot_latched = false;
                    boot_release_pending = false;
                }
                return false;
            }
            if(!low) return false;
            boot_latched = true;
            boot_release_pending = false;
            return true;
        }

        void process_buttons(){
            const bool boot = consume_boot_button();
            bool power = false;
            if(power_ready.load() && gpio_get_level(hardware::board::pins::PMU_IRQ) == 0){
                const esp_err_t err = hardware::axp2101::consume_power_key(power);
                if(err != ESP_OK) ESP_LOGW(TAG, "Power-key IRQ read failed: %s", esp_err_to_name(err));
            }
            if(boot) publish_button(boot_count);
            if(power) publish_button(power_count);
        }

        // Hardware event dispatcher: EEZ alone decides when to request standby.
        void policy_task(void *){
            init_buttons();
            bool touch_communication_ok = true;
            uint32_t last_charger_poll = uptime_ms() - charger_poll_ms;
            unsigned charger_read_failures = 0;

            while(true){
                const uint32_t events = wait_for_events();
                // Capture commands before processing inputs. A new input in
                // this iteration supersedes a pending standby command.
                const bool sleep = sleep_requested.exchange(false);
                const uint32_t revision = input_revision;
                process_touch(events, touch_communication_ok);
                process_wrist_wake();
                process_buttons();
                process_charging(last_charger_poll, charger_read_failures);
                process_external_wake();
                touch_expire(uptime_ms());
                if(sleep && revision == input_revision && screen_awake.load()){
                    // Preserve the hardware safety guard against an unusable
                    // touch controller. No charging or pairing UI policy here.
                    if(touch_communication_ok){
                        const esp_err_t err = set_screen_awake(false);
                        if(err != ESP_OK) ESP_LOGW(TAG, "EEZ standby failed: %s", esp_err_to_name(err));
                    }else{
                        ESP_LOGW(TAG, "EEZ standby refused: touch communication unavailable");
                    }
                }
            }
        }
    } // namespace

    // ------------------------------------------------------------------------
    // Service initialization
    // ------------------------------------------------------------------------

    esp_err_t start(ScreenControl apply_screen_awake){
        if(started) return ESP_OK;
        if(apply_screen_awake == nullptr) return ESP_ERR_INVALID_ARG;

        esp_err_t err = hardware::cst9217::init();
        if(err != ESP_OK) return err;

        transition_mutex = xSemaphoreCreateMutex();
        if(transition_mutex == nullptr) return ESP_ERR_NO_MEM;
#if CONFIG_PM_ENABLE && CONFIG_FREERTOS_USE_TICKLESS_IDLE && CONFIG_MCW_AUTO_LIGHT_SLEEP && CONFIG_PM_LIGHT_SLEEP_CALLBACKS
        // Acquire before the main task enables automatic light sleep.
        err = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "amoled-active", &display_sleep_lock);
        if(err != ESP_OK){
            vSemaphoreDelete(transition_mutex);
            transition_mutex = nullptr;
            ESP_LOGE(TAG, "Display PM lock creation failed: %s", esp_err_to_name(err));
            return err;
        }
        err = display_lock_acquire();
        if(err != ESP_OK){
            esp_pm_lock_delete(display_sleep_lock);
            display_sleep_lock = nullptr;
            vSemaphoreDelete(transition_mutex);
            transition_mutex = nullptr;
            return err;
        }
#endif

        screen_control = apply_screen_awake;
        screen_awake.store(true);
        touch_set_awake(true);

        if(xTaskCreate(policy_task, "display_power", 8192, nullptr, 4, &task_handle) != pdPASS){
            screen_control = nullptr;
            touch_set_awake(false);
            display_lock_release();
#if CONFIG_PM_ENABLE && CONFIG_FREERTOS_USE_TICKLESS_IDLE && CONFIG_MCW_AUTO_LIGHT_SLEEP && CONFIG_PM_LIGHT_SLEEP_CALLBACKS
            if(!display_sleep_lock_held){
                esp_pm_lock_delete(display_sleep_lock);
                display_sleep_lock = nullptr;
            }
#endif
            vSemaphoreDelete(transition_mutex);
            transition_mutex = nullptr;
            return ESP_ERR_NO_MEM;
        }
        hardware::cst9217::set_notification_task(task_handle);
        started = true;
        ESP_LOGI(TAG, "Display hardware ready; standby controlled by EEZ");
        return ESP_OK;
    }

    // ------------------------------------------------------------------------
    // Serialized screen transitions
    // ------------------------------------------------------------------------

    namespace {
        // All transition helpers below require the transition mutex.
        // The public function owns the mutex; helpers must never take it again.

        // Prevent CPU sleep before touching the panel or resuming LVGL.
        esp_err_t wake_screen(){
            esp_err_t err = display_lock_acquire();
            if(err == ESP_OK){
                // Count the start of a wake cycle, before EEZ's hidden redraw.
                sleep_requested.store(false);
                increment_event(wake_events);
                err = apply_screen_control(true);
            }
            if(err != ESP_OK){
                // A partially initialized panel must remain protected for retry.
                ESP_LOGW(TAG, "AMOLED wake incomplete; keeping CPU light sleep blocked");
                return err;
            }
            screen_awake.store(true);
            touch_set_awake(true);
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
            screen_awake.store(false);
            sleep_requested.store(false);
            display_lock_release();
            return ESP_OK;
        }

        // Serialize the requested hardware state; no UI mode or timer policy.
        esp_err_t apply_transition(bool awake){
            if(screen_awake.load() != awake) return awake ? wake_screen() : sleep_screen();
            return ESP_OK;
        }
    } // namespace

    // Public entry point: serialize every display transition, including errors.
    esp_err_t set_screen_awake(bool awake){
        if(screen_control == nullptr || transition_mutex == nullptr) return ESP_ERR_INVALID_STATE;
        if(xSemaphoreTake(transition_mutex, pdMS_TO_TICKS(3000)) != pdTRUE) return ESP_ERR_TIMEOUT;
        const esp_err_t err = apply_transition(awake);
        xSemaphoreGive(transition_mutex);
        return err;
    }

    // ------------------------------------------------------------------------
    // UI input and state interface
    // ------------------------------------------------------------------------

    TouchInput read_touch_input(){
        portENTER_CRITICAL(&touch_mux);
        const TouchInput input = touch_buffer.read();
        portEXIT_CRITICAL(&touch_mux);
        return input;
    }

    // Queue a gesture in task context; never perform panel I/O here.
    void request_wrist_wake(){
        if(!started.load() || task_handle == nullptr) return;
        wrist_wake_pending.store(true);
        (void)xTaskNotify(task_handle, wrist_wake_notification, eSetBits);
    }

    void recheck_buttons(){
        if(!started || task_handle == nullptr) return;
        if((boot_ready.load() && gpio_get_level(hardware::board::pins::BOOT_BUTTON) == 0) || (power_ready.load() && gpio_get_level(hardware::board::pins::PMU_IRQ) == 0)){
            (void)xTaskNotify(task_handle, 0, eNoAction);
        }
    }

    int32_t boot_button_count(){ return boot_count.load(); }
    int32_t power_button_count(){ return power_count.load(); }

    bool is_screen_awake(){ return screen_awake.load(); }
    bool is_charging(){ return charging_active.load(); }

    int32_t touch_count(){ return touch_events.load(); }
    int32_t wrist_count(){ return wrist_events.load(); }
    int32_t display_wake_count(){ return wake_events.load(); }

    bool sleep_request_pending(){ return sleep_requested.load(); }

    // Called by EEZ under the LVGL lock. Never pause or lock the renderer here.
    void request_sleep(bool requested){
        if(!requested){ sleep_requested.store(false); return; }
        // Reject stale timer actions during startup or the hidden wake refresh.
        if(!started.load() || !screen_awake.load() || task_handle == nullptr) return;
        sleep_requested.store(true);
        (void)xTaskNotify(task_handle, sleep_request_notification, eSetBits);
    }

    // An incoming pairing request may need the UI, even when it is asleep.
    void request_external_wake(){
        if(!started.load() || task_handle == nullptr) return;
        external_wake_pending.store(true);
        (void)xTaskNotify(task_handle, external_wake_notification, eSetBits);
    }

} // namespace services::display_power
