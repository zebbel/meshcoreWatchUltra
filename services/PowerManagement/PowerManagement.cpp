#include "PowerManagement.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "board_pins.h"
#include <atomic>
#include <stdint.h>

namespace services::power_management {
static const char *TAG = "PowerManagement";
void configure_startup_scaling() {
#if CONFIG_PM_ENABLE
    // Enable DFS first; automatic light sleep starts after wake hardware is ready.
    esp_pm_config_t pm = {};
    pm.max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    pm.min_freq_mhz = 80;
    pm.light_sleep_enable = false;
    const esp_err_t pm_err = esp_pm_configure(&pm);
    if (pm_err == ESP_OK)
        ESP_LOGI(TAG, "Startup CPU scaling: %d -> 80 MHz; light sleep configured after initialization", pm.max_freq_mhz);
    else
        ESP_LOGW(TAG, "Dynamic CPU scaling unavailable: %s", esp_err_to_name(pm_err));
#endif
}

#if CONFIG_PM_ENABLE && CONFIG_FREERTOS_USE_TICKLESS_IDLE && CONFIG_MCW_AUTO_LIGHT_SLEEP && CONFIG_PM_LIGHT_SLEEP_CALLBACKS
// Kconfig booleans set to 'n' are undefined; use a real compile-time bool.
#if defined(CONFIG_MCW_CONNECTED_BLE_LIGHT_SLEEP)
static constexpr bool kConnectedBleLightSleep = true;
#else
static constexpr bool kConnectedBleLightSleep = false;
#endif
// keep GPS UART, LVGL/panel traffic awake;
// allow tickless idle only when screen and GNSS are both inactive.
static esp_pm_lock_handle_t s_sleep_guard = nullptr;
static bool s_sleep_guard_held = false;
static bool s_auto_light_sleep = false;
static std::atomic<bool> s_restore_wake_pins{false};
static std::atomic<uint32_t> s_light_sleep_exits{0};
// Cumulative statistics are RAM-only, reset on reboot, and do not write flash.
// 32-bit milliseconds cover ~49 days; enough for unattended battery trials.
static std::atomic<uint32_t> s_sleep_total_ms{0};
static std::atomic<uint32_t> s_sleep_entries{0};
static std::atomic<uint32_t> s_sleep_longest_ms{0};
static std::atomic<uint32_t> s_sleep_stats_start_ms{0};

// PM callbacks run in a critical path: no logs, allocations, I2C or SPI.
// The callback argument is the *actual* sleep duration in microseconds.
static esp_err_t IRAM_ATTR on_light_sleep_exit(int64_t slept_us, void *) {
    s_restore_wake_pins.store(true, std::memory_order_release);
    if (slept_us > 0) {
        const uint32_t ms = static_cast<uint32_t>(slept_us / 1000);
        s_light_sleep_exits.fetch_add(1, std::memory_order_relaxed);
        s_sleep_total_ms.fetch_add(ms, std::memory_order_relaxed);
        s_sleep_entries.fetch_add(1, std::memory_order_relaxed);
        uint32_t old = s_sleep_longest_ms.load(std::memory_order_relaxed);
        while (ms > old && !s_sleep_longest_ms.compare_exchange_weak(
                   old, ms, std::memory_order_relaxed)) {}
    }
    return ESP_OK;
}
static esp_pm_sleep_cbs_register_config_t s_sleep_callbacks = {
    .enter_cb = nullptr,
    .exit_cb = on_light_sleep_exit,
    .enter_cb_user_arg = nullptr,
    .exit_cb_user_arg = nullptr,
    .enter_cb_prior = 0,
    .exit_cb_prior = 0,
};

static void disable_light_sleep(const char *reason, esp_err_t err) {
    ESP_LOGE(TAG, "%s (%s); reverting to DFS without light sleep", reason, esp_err_to_name(err));
    esp_pm_config_t cfg = {};
    cfg.max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    cfg.min_freq_mhz = 80;
    cfg.light_sleep_enable = false;
    (void)esp_pm_configure(&cfg);
    s_auto_light_sleep = false;
    (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_EXT0);
    (void)esp_sleep_disable_ext1_wakeup_io(1ULL << board::pins::LORA_DIO1);
}

void start_light_sleep(bool meshcore_enabled, bool display_ready, bool radio_receiving) {
    // No sleep if wake hardware / continuous RX could not be initialized.
    if (!meshcore_enabled || !display_ready || !radio_receiving) {
        ESP_LOGW(TAG, "Auto light sleep not enabled (LoRa RX or touch/display unavailable)");
        return;
    }
    esp_err_t err = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0,
                                      "watch-active", &s_sleep_guard);
    if (err != ESP_OK) { ESP_LOGW(TAG, "PM lock unavailable: %s", esp_err_to_name(err)); return; }
    err = esp_pm_lock_acquire(s_sleep_guard);
    if (err != ESP_OK) { ESP_LOGW(TAG, "PM lock acquire failed: %s", esp_err_to_name(err)); return; }
    s_sleep_guard_held = true;

    // Different IRQ polarities; EXT0 (touch, low) and EXT1 (LoRa, high)
    // leave the digital GPIO interrupt trigger types untouched.
    err = esp_sleep_enable_ext0_wakeup(board::pins::TOUCH_IRQ, 0);
    if (err == ESP_OK)
        err = esp_sleep_enable_ext1_wakeup_io(1ULL << board::pins::LORA_DIO1,
                                             ESP_EXT1_WAKEUP_ANY_HIGH);
    if (err != ESP_OK) { disable_light_sleep("Wake source setup failed", err); return; }
    err = esp_pm_light_sleep_register_cbs(&s_sleep_callbacks);
    if (err != ESP_OK) { disable_light_sleep("Light sleep callback setup failed", err); return; }

    // The main application loop already has an IRQ-assisted 50-ms timeout;
    // ESP-IDF schedules its own wake timer for the earliest RTOS deadline.
    esp_pm_config_t cfg = {};
    cfg.max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    cfg.min_freq_mhz = 80;
    cfg.light_sleep_enable = true;
    err = esp_pm_configure(&cfg);
    if (err != ESP_OK) { disable_light_sleep("esp_pm_configure failed", err); return; }
    s_auto_light_sleep = true;
    s_sleep_stats_start_ms.store(static_cast<uint32_t>(esp_timer_get_time() / 1000ULL),
                                 std::memory_order_relaxed);
    ESP_LOGI(TAG, "Auto light sleep enabled; touch/LoRa wake and display/GPS guards; "
                  "connected BLE light sleep %s",
             kConnectedBleLightSleep ? "allowed" : "blocked");
}

// RTC pads used for EXT0/EXT1 must return to digital GPIO mode after sleep.
// LoRa IRQ is level-latched until SX1262 clears it, so the 50-ms application
// timeout remains an independent fallback if the edge occurs while asleep.
void restore_wakeup_pins_if_needed() {
    if (!s_auto_light_sleep || !s_restore_wake_pins.exchange(false, std::memory_order_acq_rel)) return;
    esp_err_t err = rtc_gpio_hold_dis(board::pins::TOUCH_IRQ);
    if (err == ESP_OK) err = rtc_gpio_hold_dis(board::pins::LORA_DIO1);
    if (err == ESP_OK) err = rtc_gpio_deinit(board::pins::TOUCH_IRQ);
    if (err == ESP_OK) err = rtc_gpio_deinit(board::pins::LORA_DIO1);
    if (err == ESP_OK) err = gpio_set_intr_type(board::pins::TOUCH_IRQ, GPIO_INTR_NEGEDGE);
    if (err == ESP_OK) err = gpio_set_intr_type(board::pins::LORA_DIO1, GPIO_INTR_POSEDGE);
    if (err == ESP_OK) err = gpio_intr_enable(board::pins::TOUCH_IRQ);
    if (err == ESP_OK) err = gpio_intr_enable(board::pins::LORA_DIO1);
    if (err != ESP_OK) disable_light_sleep("GPIO restoration failed", err);
}

void update_light_sleep_guard(bool screen_awake, bool gps_powered, bool ble_active) {
    if (!s_auto_light_sleep || s_sleep_guard == nullptr) return;
    // the BLE controller owns its own PM
    // locks during connection events. Do not keep an app-wide NO_LIGHT_SLEEP
    // lock for the whole BLE link. The display service has a separate lock
    // held throughout panel reset, LVGL redraw and active display time.
    const bool ble_guard = !kConnectedBleLightSleep &&
                           ble_active;
    const bool must_stay_awake = screen_awake ||
                                 gps_powered || ble_guard;
    if (must_stay_awake == s_sleep_guard_held) return;
    const esp_err_t err = must_stay_awake
        ? esp_pm_lock_acquire(s_sleep_guard)
        : esp_pm_lock_release(s_sleep_guard);
    if (err == ESP_OK) s_sleep_guard_held = must_stay_awake;
    else disable_light_sleep("PM lock transition failed", err);
}

void report_light_sleep_exits(bool screen_awake, bool gps_powered, bool ble_active) {
    if (!s_auto_light_sleep) return;
    static uint32_t last_report = 0;
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
    if (last_report == 0) { last_report = now; return; }
    if (static_cast<uint32_t>(now - last_report) < 60000U) return;
    last_report = now;
    const uint32_t exits = s_light_sleep_exits.exchange(0, std::memory_order_relaxed);
    const uint32_t total = s_sleep_total_ms.load(std::memory_order_relaxed);
    const uint32_t entries = s_sleep_entries.load(std::memory_order_relaxed);
    const uint32_t longest = s_sleep_longest_ms.load(std::memory_order_relaxed);
    const uint32_t elapsed = now - s_sleep_stats_start_ms.load(std::memory_order_relaxed);
    const uint32_t percent = elapsed ?
        static_cast<uint32_t>((static_cast<uint64_t>(total) * 100U) / elapsed) : 0;
    ESP_LOGD(TAG, "PM/60s exits=%lu; total=%lums/%lums (%lu%%), entries=%lu, "
                  "avg=%lums, longest=%lums; disp=%d gps=%d BLE=%d",
             static_cast<unsigned long>(exits), static_cast<unsigned long>(total),
             static_cast<unsigned long>(elapsed),
             static_cast<unsigned long>(percent > 100 ? 100 : percent),
             static_cast<unsigned long>(entries),
             static_cast<unsigned long>(entries ? total / entries : 0),
             static_cast<unsigned long>(longest),
             screen_awake,
             gps_powered,
             ble_active);
    if (entries == 0) ESP_LOGD(TAG, "No light sleep recorded; USB or active services may prevent sleep");
}
#else
void start_light_sleep(bool, bool, bool) {
    ESP_LOGW(TAG, "Auto light sleep disabled in firmware config; DFS remains active");
}
void restore_wakeup_pins_if_needed() {}
void update_light_sleep_guard(bool, bool, bool) {}
void report_light_sleep_exits(bool, bool, bool) {}
#endif

SleepStats sleep_stats() {
    SleepStats snapshot{};
#if CONFIG_PM_ENABLE && CONFIG_FREERTOS_USE_TICKLESS_IDLE && CONFIG_MCW_AUTO_LIGHT_SLEEP && CONFIG_PM_LIGHT_SLEEP_CALLBACKS
    snapshot.pm_enabled = s_auto_light_sleep;
    if (s_auto_light_sleep) {
        const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
        const uint32_t elapsed = now - s_sleep_stats_start_ms.load(std::memory_order_relaxed);
        const uint32_t total = s_sleep_total_ms.load(std::memory_order_relaxed);
        const uint32_t percent = elapsed ?
            static_cast<uint32_t>((static_cast<uint64_t>(total) * 100U) / elapsed) : 0;
        snapshot.sleep_percent = static_cast<uint8_t>(percent > 100 ? 100 : percent);
        snapshot.sleep_entries = s_sleep_entries.load(std::memory_order_relaxed);
    }
#endif
    return snapshot;
}

} // namespace services::power_management
