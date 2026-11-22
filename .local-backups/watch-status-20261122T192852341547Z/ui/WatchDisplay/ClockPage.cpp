#include "ClockPage.h"

#include "Power.h"
#include "DisplayPower.h"

#include "lvgl.h"
#include "sdkconfig.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#ifndef CONFIG_MCW_WATCH_CLOCK_TIMEZONE
// Defaults to Berlin time (including European summer time) until configurable
// timezone settings are exposed in the watch UI.
#define CONFIG_MCW_WATCH_CLOCK_TIMEZONE "CET-1CEST,M3.5.0,M10.5.0/3"
#endif

namespace ui::watch_display {
namespace {
constexpr uint32_t kBatteryRefreshMs = 60000;
constexpr time_t kEarliestTrustedTime = 1577836800; // 2020-01-01 UTC

lv_obj_t *s_time_label = nullptr;
lv_obj_t *s_battery_label = nullptr;
lv_timer_t *s_update_timer = nullptr;
uint32_t s_last_battery_ms = 0;
bool s_battery_sampled = false;
bool s_battery_valid = false;
char s_last_time[6] = "";
char s_last_battery[6] = "";

void update_time() {
    if (!s_time_label) return;
    char text[6] = "--:--";
    const time_t epoch = std::time(nullptr); // Set by RTC, GPS or BLE SET_TIME.
    if (epoch >= kEarliestTrustedTime) {
        struct tm local{};
        if (localtime_r(&epoch, &local) != nullptr) {
            std::snprintf(text, sizeof(text), "%02d:%02d", local.tm_hour, local.tm_min);
        }
    }
    if (std::strcmp(text, s_last_time) != 0) {
        lv_label_set_text(s_time_label, text);
        std::strcpy(s_last_time, text);
    }
}

void update_battery() {
    if (!s_battery_label) return;
    hardware::power::battery_info_t battery{};
    const esp_err_t err = hardware::power::read_battery(battery);
    if (err != ESP_OK || !battery.percent_valid) {
        // Keep the last valid percentage during transient I2C errors.
        if (s_battery_valid) return;
        lv_label_set_text(s_battery_label, "--%");
        return;
    }
    s_battery_valid = true;
    char text[6];
    std::snprintf(text, sizeof(text), "%u%%", static_cast<unsigned>(battery.percentage));
    if (std::strcmp(text, s_last_battery) != 0) {
        lv_label_set_text(s_battery_label, text);
        std::strcpy(s_last_battery, text);
    }
}

void timer_cb(lv_timer_t *) {
    // Keep LVGL from invalidating screen regions while the panel is off.
    // The active screen is invalidated by WatchDisplay on the next wake-up.
    if (!services::display_power::is_screen_awake()) return;

    update_time();
    const uint32_t now = lv_tick_get();
    if (!s_battery_sampled || static_cast<uint32_t>(now - s_last_battery_ms) >= kBatteryRefreshMs) {
        s_battery_sampled = true;
        s_last_battery_ms = now;
        update_battery();
    }
}
} // namespace

void render_clock_page() {
    // The project has one LVGL owner. This is invoked with the adapter locked.
    if (s_update_timer != nullptr) return;

    // Watch RTC and MeshCore stay in UTC. Only the displayed time is local.
    setenv("TZ", CONFIG_MCW_WATCH_CLOCK_TIMEZONE, 1);
    tzset();

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    // Keep battery text well below the curved upper edge of the watch face.
    s_battery_label = lv_label_create(screen);
    lv_label_set_text(s_battery_label, "--%");
    lv_obj_set_style_text_color(s_battery_label, lv_color_hex(0xCDD8DF), 0);
    // The original default Montserrat 14px is doubled to a sharp 28px font.
#if LV_FONT_MONTSERRAT_28
    lv_obj_set_style_text_font(s_battery_label, &lv_font_montserrat_28, 0);
#else
    // Fallback if the 28px font was disabled in a custom LVGL configuration.
    lv_obj_set_style_transform_pivot_x(s_battery_label, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(s_battery_label, LV_PCT(50), 0);
    lv_obj_set_style_transform_scale_x(s_battery_label, 512, 0);
    lv_obj_set_style_transform_scale_y(s_battery_label, 512, 0);
#endif
    lv_obj_align(s_battery_label, LV_ALIGN_TOP_MID, 0, 50);

    s_time_label = lv_label_create(screen);
    lv_label_set_text(s_time_label, "--:--");
    lv_obj_set_style_text_color(s_time_label, lv_color_hex(0xFFFFFF), 0);
#if LV_FONT_MONTSERRAT_48
    lv_obj_set_style_text_font(s_time_label, &lv_font_montserrat_48, 0);
#elif LV_FONT_MONTSERRAT_36
    lv_obj_set_style_text_font(s_time_label, &lv_font_montserrat_36, 0);
#elif LV_FONT_MONTSERRAT_28
    lv_obj_set_style_text_font(s_time_label, &lv_font_montserrat_28, 0);
#endif
    // LVGL 9 provides built-in Montserrat fonts up to 48px. Scale the
    // existing time label by exactly 2x for a visually 96px clock.
    // Use the label center as pivot to retain perfect screen centering.
    lv_obj_set_style_transform_pivot_x(s_time_label, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(s_time_label, LV_PCT(50), 0);
    lv_obj_set_style_transform_scale_x(s_time_label, 512, 0);
    lv_obj_set_style_transform_scale_y(s_time_label, 512, 0);
    lv_obj_align(s_time_label, LV_ALIGN_CENTER, 0, 0);

    // Initial update and light periodic refresh, all in the LVGL task.
    timer_cb(nullptr);
    s_update_timer = lv_timer_create(timer_cb, 1000, nullptr);
}
} // namespace ui::watch_display
