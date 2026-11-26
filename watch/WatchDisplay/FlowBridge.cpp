#include "FlowBridge.h"
#include "Power.h"
#include "DisplayPower.h"
#include "WatchDisplay.h"
#include "WatchStatus.h"
#include "CO5300Panel.h"
#include "BluetoothTransport.h"
#include "esp_log.h"
#include "lvgl.h"
#include "sdkconfig.h"
#include "ui.h"
#include "vars.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <climits>
#include "screens.h"
#include "eez-flow.h"

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
std::atomic<uint32_t> s_pairing_pin{0};
char s_pairing_pin_text[7] = "";
bool s_pairing_page_active = false;
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
void update_pairing_page() {
    const bool pairing = s_pairing_pin.load() != 0;

    if (pairing && !s_pairing_page_active) {
        s_pairing_page_active = true;

        eez_flow_push_screen(
            SCREEN_ID_PAIRING,
            LV_SCR_LOAD_ANIM_NONE,
            0, 0
        );
    } else if (!pairing && s_pairing_page_active) {
        s_pairing_page_active = false;

        if (eez_flow_get_current_screen() == SCREEN_ID_PAIRING) {
            eez_flow_pop_screen(
                LV_SCR_LOAD_ANIM_NONE,
                0, 0
            );
        }
    }
}
void tick(lv_timer_t *) {
    if (!services::display_power::is_screen_awake()) return;

    const uint32_t now = lv_tick_get();

    if (now - s_last_clock >= 1000U)
        sample_time();

    if (now - s_last_battery >=
        (s_charging ? kChargingBatteryMs : kBatteryMs))
        sample_battery();

    update_pairing_page();

    ui_tick();
}
// All runtime access is under the existing LVGL lock / timer context.
// A short bounded burst processes pending Watch/IsTrue/LVGL actions before
// the existing hidden-panel redraw. No C++ navigation or animation.
void settle_flow() {
    for (unsigned i = 0; i < 8; ++i) ui_tick();
}
} // namespace

// EEZ vars.h uses C linkage. Sensor variables are read-only. Brightness
// is writable through the existing display owner, with the LVGL lock held.
extern "C" {
bool get_var_is_charging() { return s_charging; }
void set_var_is_charging(bool) {}
int32_t get_var_battery_percent() { return s_battery; }
void set_var_battery_percent(int32_t) {}
const char *get_var_clock_time() { return s_time; }
void set_var_clock_time(const char *) {}
bool get_var_gps_enabled() { return watch::status::get().gps_enabled; }
void set_var_gps_enabled(bool) {}
bool get_var_gps_fix() { return watch::status::get().gps_has_fix; }
void set_var_gps_fix(bool) {}
int32_t get_var_gps_satellites() { return watch::status::get().gps_satellites; }
void set_var_gps_satellites(int32_t) {}
bool get_var_ble_connected() { return watch::status::get().ble_connected; }
void set_var_ble_connected(bool) {}
bool get_var_meshcore_active() { return watch::status::get().meshcore_active; }
void set_var_meshcore_active(bool) {}
int32_t get_var_meshcore_rx_packets() {
    const uint32_t count = watch::status::get().meshcore_rx_packets;
    return count > INT32_MAX ? INT32_MAX : static_cast<int32_t>(count);
}
void set_var_meshcore_rx_packets(int32_t) {}
int32_t get_var_brightness() { return hardware::co5300::brightness(); }
void set_var_brightness(int32_t value) {
    // Do not narrow an unchecked signed value, or turn the UI black.
    if (value < 1 || value > 100) return;
    if (value == hardware::co5300::brightness()) return;
    // Native setters execute under the LVGL lock, including hidden wake.
    const esp_err_t err = hardware::co5300::set_brightness(static_cast<uint8_t>(value));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "EEZ brightness change failed: %s", esp_err_to_name(err));
    }
}
const char *get_var_ble_pairing_pin() {
    const uint32_t pin = s_pairing_pin.load();

    if (pin >= 100000 && pin <= 999999) {
        std::snprintf(
            s_pairing_pin_text,
            sizeof(s_pairing_pin_text),
            "%06lu",
            static_cast<unsigned long>(pin)
        );
    } else {
        s_pairing_pin_text[0] = '\0';
    }

    return s_pairing_pin_text;
}

void set_var_ble_pairing_pin(const char *) {}
}

namespace watch::display {
void update_bluetooth_pairing() {
    const uint32_t pin =
        hardware::bluetooth::transport().active_pairing_pin();

    if(pin == s_pairing_pin.load()) return;

    s_pairing_pin.store(pin);
    services::display_power::set_keep_awake(pin != 0);
}

void refresh_flow(bool charging) {
    if (!s_timer) return;

    s_charging = charging;

    sample_battery();
    sample_time();

    update_pairing_page();

    settle_flow();
}
esp_err_t init_flow() {
    if (s_timer) return ESP_OK;
    setenv("TZ", CONFIG_MCW_WATCH_CLOCK_TIMEZONE, 1);
    tzset();
    // Seed native values before the generated runtime evaluates expressions.
    s_charging = services::display_power::is_charging();
    sample_time();
    sample_battery();
    lv_obj_t *initial_screen = lv_screen_active();
    ui_init();
    settle_flow();
    // The designer, not C++, owns page names and navigation policy.
    if (eez_flow_is_stopped() || !lv_screen_active() || lv_screen_active() == initial_screen) {
        ESP_LOGE(TAG, "EEZ Flow did not load a generated screen");
        return ESP_ERR_INVALID_STATE;
    }
    s_timer = lv_timer_create(tick, kFlowTickMs, nullptr);
    if (!s_timer) return ESP_ERR_NO_MEM;
    if (hardware::co5300::brightness() == 0) ESP_LOGW(TAG, "EEZ has not set brightness yet; panel remains blank until a valid request");
    ESP_LOGI(TAG, "EEZ Flow ready (50 ms tick; native hardware variables)");
    return ESP_OK;
}
} // namespace watch::display
