#include "ClockPage.h"
#include "Power.h"
#include "DisplayPower.h"
#include "WatchDisplay.h"
#include "esp_log.h"
#include "lvgl.h"
#include "sdkconfig.h"
#include "ui.h"
#include "vars.h"
#include "screens.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <climits>

#ifndef CONFIG_MCW_WATCH_CLOCK_TIMEZONE
#define CONFIG_MCW_WATCH_CLOCK_TIMEZONE "CET-1CEST,M3.5.0,M10.5.0/3"
#endif
namespace {
constexpr uint32_t kFlowTickMs = 50;
constexpr uint32_t kBatteryMs = 60000;
constexpr uint32_t kChargingBatteryMs = 10000;
constexpr time_t kEarliestTrustedTime = 1577836800;
constexpr const char *TAG = "WatchFlow";
lv_timer_t *s_timer = nullptr;
bool s_charging = false;
int32_t s_battery = -1;
char s_time[6] = "--:--";
uint32_t s_last_battery = 0;
uint32_t s_last_clock = 0;
std::atomic<uint32_t> s_status{0};
std::atomic<uint32_t> s_rx{0};
std::atomic<int32_t> s_brightness{25};
constexpr uint32_t GPS_ENABLED=1U, GPS_FIX=2U, BLE=4U, MESH=8U;
void sample_time() {
    const time_t epoch = std::time(nullptr);
    struct tm local{};
    if (epoch >= kEarliestTrustedTime && localtime_r(&epoch, &local) &&
        local.tm_hour >= 0 && local.tm_hour < 24 && local.tm_min >= 0 && local.tm_min < 60) {
        std::snprintf(s_time, sizeof(s_time), "%02u:%02u",
            static_cast<unsigned>(local.tm_hour), static_cast<unsigned>(local.tm_min));
    } else {
        std::snprintf(s_time, sizeof(s_time), "--:--");
    }
    s_last_clock = lv_tick_get();
}
void sample_battery() {
    hardware::power::battery_info_t battery{};
    if (hardware::power::read_battery(battery) == ESP_OK && battery.percent_valid &&
        static_cast<unsigned>(battery.percentage) <= 100U) {
        s_battery = static_cast<int32_t>(battery.percentage);
    }
    s_last_battery = lv_tick_get();
}
void tick(lv_timer_t *) {
    if (!services::display_power::is_screen_awake()) return;
    const uint32_t now = lv_tick_get();
    if (now - s_last_clock >= 1000U) sample_time();
    if (now - s_last_battery >= (s_charging ? kChargingBatteryMs : kBatteryMs)) sample_battery();
    ui_tick();
}
// All runtime access is under the existing LVGL lock / timer context.
// A short bounded burst processes pending Watch/IsTrue/LVGL actions before
// the existing hidden-panel redraw. No C++ navigation or animation.
void settle_flow() {
    for (unsigned i = 0; i < 8; ++i) ui_tick();
}
} // namespace

// EEZ vars.h uses C linkage. Native hardware variables are read-only:
// the generated table requires setters even when the UI only reads them.
extern "C" {
bool get_var_is_charging() { return s_charging; }
void set_var_is_charging(bool) {}
int32_t get_var_battery_percent() { return s_battery; }
void set_var_battery_percent(int32_t) {}
const char *get_var_clock_time() { return s_time; }
void set_var_clock_time(const char *) {}
bool get_var_gps_enabled() { return (s_status.load() & GPS_ENABLED) != 0; }
void set_var_gps_enabled(bool) {}
bool get_var_gps_fix() { return (s_status.load() & GPS_FIX) != 0; }
void set_var_gps_fix(bool) {}
int32_t get_var_gps_satellites() { return (s_status.load() >> 8) & 255U; }
void set_var_gps_satellites(int32_t) {}
bool get_var_ble_connected() { return (s_status.load() & BLE) != 0; }
void set_var_ble_connected(bool) {}
bool get_var_meshcore_active() { return (s_status.load() & MESH) != 0; }
void set_var_meshcore_active(bool) {}
int32_t get_var_meshcore_rx_packets() {
    const uint32_t count = s_rx.load();
    return count > INT32_MAX ? INT32_MAX : static_cast<int32_t>(count);
}
void set_var_meshcore_rx_packets(int32_t) {}
int32_t get_var_sleep_percent() { return (s_status.load() >> 16) & 255U; }
void set_var_sleep_percent(int32_t) {}
int32_t get_var_brightness() { return s_brightness.load(); }
void set_var_brightness(int32_t) {}
}

namespace ui::watch_display {
void publish_watch_status(const watch::WatchStatus &status) {
    const uint32_t bits = (status.gps_enabled ? GPS_ENABLED : 0U) |
        (status.gps_has_fix ? GPS_FIX : 0U) | (status.ble_connected ? BLE : 0U) |
        (status.meshcore_active ? MESH : 0U) |
        (static_cast<uint32_t>(status.gps_satellites) << 8) |
        (static_cast<uint32_t>(status.sleep_percent) << 16);
    s_rx.store(status.meshcore_rx_packets);
    s_status.store(bits);
}
void publish_ui_brightness(uint8_t percent) { s_brightness.store(percent); }
void refresh_clock_battery() {
    if (s_timer) sample_battery();
}
void set_clock_charging_mode(bool charging) {
    if (!s_timer) return;
    const bool changed = s_charging != charging;
    s_charging = charging;
    sample_battery();
    sample_time();
    // Also called during hidden wake with the display service still reporting
    // asleep. Explicitly tick here, rather than relying on the periodic timer.
    settle_flow();
    if (changed) {
        ESP_LOGI(TAG, "Charging state published to EEZ: %s", charging ? "true" : "false");
        if (lv_screen_active() != (charging ? objects.charging : objects.main)) {
            ESP_LOGW(TAG, "EEZ navigation not settled; check page Watch actions (no native fallback)");
        }
    }
}
esp_err_t render_clock_page() {
    if (s_timer) return ESP_OK;
    setenv("TZ", CONFIG_MCW_WATCH_CLOCK_TIMEZONE, 1);
    tzset();
    // Seed native values before the generated runtime evaluates expressions.
    s_charging = services::display_power::is_charging();
    sample_time();
    sample_battery();
    ui_init();
    settle_flow();
    if (!objects.main || !objects.charging || objects.main == objects.charging) {
        ESP_LOGE(TAG, "EEZ main/charging pages missing; enable creation at startup");
        return ESP_ERR_INVALID_STATE;
    }
    s_timer = lv_timer_create(tick, kFlowTickMs, nullptr);
    if (!s_timer) return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "EEZ Flow ready (50 ms tick; native hardware variables)");
    return ESP_OK;
}
} // namespace ui::watch_display
