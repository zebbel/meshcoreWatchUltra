#include "ClockPage.h"

#include "Power.h"
#include "DisplayPower.h"
#include "WatchDisplay.h"

#include "lvgl.h"
#include "sdkconfig.h"

#include <atomic>
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
constexpr uint32_t kChargingBatteryRefreshMs = 10000;
constexpr time_t kEarliestTrustedTime = 1577836800; // 2020-01-01 UTC

lv_obj_t *s_time_label = nullptr;
lv_obj_t *s_battery_label = nullptr;
lv_obj_t *s_charge_label = nullptr;
bool s_charging = false;
lv_timer_t *s_update_timer = nullptr;
uint32_t s_last_battery_ms = 0;
bool s_battery_sampled = false;
bool s_battery_valid = false;
char s_last_time[6] = "";
char s_last_battery[6] = "";

// Single-producer (app_main) / single-consumer (LVGL) atomic snapshots.
// GPS flags and satellite count share one atomic word, so the UI cannot
// combine a new fix flag with an unrelated old satellite value.
constexpr uint32_t kGpsEnabled  = 1U << 0;
constexpr uint32_t kGpsFix      = 1U << 1;
constexpr uint32_t kGpsAcquiring= 1U << 2;
constexpr uint32_t kMeshActive  = 1U << 3;
constexpr uint32_t kBleLinked   = 1U << 4;
constexpr unsigned kSatShift = 8;
std::atomic<uint32_t> s_status_bits{0};
std::atomic<uint32_t> s_rx_packets{0};
lv_obj_t *s_gps_label = nullptr;
lv_obj_t *s_meshcore_label = nullptr;
lv_obj_t *s_ble_label = nullptr;
// a small diagnostic label above the clock.
lv_obj_t *s_sleep_label = nullptr;
std::atomic<uint32_t> s_sleep_state{0}; // enabled bit 8, percent bits 0-7
std::atomic<uint32_t> s_sleep_entries{0};
uint32_t s_last_displayed_status = UINT32_MAX;
uint32_t s_last_displayed_rx = UINT32_MAX;
uint32_t s_last_displayed_sleep = UINT32_MAX;
uint32_t s_last_displayed_entries = UINT32_MAX;

void update_status_labels() {
    if (!s_gps_label || !s_meshcore_label || !s_ble_label || !s_sleep_label) return;

    const uint32_t state = s_status_bits.load(std::memory_order_acquire);
    const uint32_t rx = s_rx_packets.load(std::memory_order_relaxed);
    const uint32_t sleep = s_sleep_state.load(std::memory_order_relaxed);
    const uint32_t entries = s_sleep_entries.load(std::memory_order_relaxed);
    if (state == s_last_displayed_status && rx == s_last_displayed_rx &&
        sleep == s_last_displayed_sleep && entries == s_last_displayed_entries) return;
    s_last_displayed_status = state;
    s_last_displayed_rx = rx;
    s_last_displayed_sleep = sleep;
    s_last_displayed_entries = entries;
    char text[40] = {};
    if (sleep & 0x100U) {
        std::snprintf(text, sizeof(text), "SLEEP %lu%%  %lux",
                      static_cast<unsigned long>(sleep & 0xFFU),
                      static_cast<unsigned long>(entries));
    } else {
        std::snprintf(text, sizeof(text), "SLEEP OFF");
    }
    if (std::strcmp(lv_label_get_text(s_sleep_label), text) != 0)
        lv_label_set_text(s_sleep_label, text);

    const bool gps_on = (state & kGpsEnabled) != 0;
    const bool has_fix = (state & kGpsFix) != 0;
    if (!gps_on) {
        std::snprintf(text, sizeof(text), "GPS OFF");
    } else if (has_fix) {
        const unsigned sats = (state >> kSatShift) & 0xFFU;
        if (sats) std::snprintf(text, sizeof(text), "GPS FIX %u SAT", sats);
        else std::snprintf(text, sizeof(text), "GPS FIX");
    } else if (state & kGpsAcquiring) {
        std::snprintf(text, sizeof(text), "GPS SEARCH");
    } else {
        std::snprintf(text, sizeof(text), "GPS WAIT");
    }
    if (std::strcmp(lv_label_get_text(s_gps_label), text) != 0)
        lv_label_set_text(s_gps_label, text);
    lv_obj_set_style_text_color(s_gps_label,
        lv_color_hex(has_fix ? 0x65E5A3 : (gps_on ? 0xEBC36D : 0x8291A5)), 0);

    if (state & kMeshActive) {
        // Compact 40px text near the curved screen edge. Counts are abbreviated.
        if (rx >= 1000000U)
            std::snprintf(text, sizeof(text), "MC RX %luM", static_cast<unsigned long>(rx / 1000000U));
        else if (rx >= 1000U)
            std::snprintf(text, sizeof(text), "MC RX %lu.%luK",
                          static_cast<unsigned long>(rx / 1000U),
                          static_cast<unsigned long>((rx % 1000U) / 100U));
        else
            std::snprintf(text, sizeof(text), "MC RX %lu", static_cast<unsigned long>(rx));
    } else {
        std::snprintf(text, sizeof(text), "MC OFF");
    }
    if (std::strcmp(lv_label_get_text(s_meshcore_label), text) != 0)
        lv_label_set_text(s_meshcore_label, text);
    lv_obj_set_style_text_color(s_meshcore_label,
        lv_color_hex((state & kMeshActive) ? 0xD5E8F5 : 0x8291A5), 0);

    const bool connected = (state & kBleLinked) != 0;
    const char *ble_text = connected ? "BLE LINK" : "BLE DOWN";
    if (std::strcmp(lv_label_get_text(s_ble_label), ble_text) != 0)
        lv_label_set_text(s_ble_label, ble_text);
    lv_obj_set_style_text_color(s_ble_label,
        lv_color_hex(connected ? 0x6CCCF5 : 0x8291A5), 0);
}

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

// LVGL scale factors use 256 for 1x. Keep native fonts where available.
#if LV_FONT_MONTSERRAT_48
constexpr int kChargingPercentScale = 512; // 48px -> approximately 96px
#elif LV_FONT_MONTSERRAT_36
constexpr int kChargingPercentScale = 640; // 36px -> approximately 90px
#elif LV_FONT_MONTSERRAT_28
constexpr int kChargingPercentScale = 768; // 28px -> approximately 84px
#else
constexpr int kChargingPercentScale = 512;
#endif
#if LV_FONT_MONTSERRAT_40 || LV_FONT_MONTSERRAT_48 || LV_FONT_MONTSERRAT_36
constexpr int kChargingIconScale = 256;
#else
constexpr int kChargingIconScale = 512;
#endif

void set_label_scale(lv_obj_t *label, int scale) {
    lv_obj_set_style_transform_pivot_x(label, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(label, LV_PCT(50), 0);
    lv_obj_set_style_transform_scale_x(label, scale, 0);
    lv_obj_set_style_transform_scale_y(label, scale, 0);
}

void set_battery_presentation(bool charging) {
    if (charging) {
#if LV_FONT_MONTSERRAT_48
        lv_obj_set_style_text_font(s_battery_label, &lv_font_montserrat_48, 0);
#elif LV_FONT_MONTSERRAT_36
        lv_obj_set_style_text_font(s_battery_label, &lv_font_montserrat_36, 0);
#elif LV_FONT_MONTSERRAT_28
        lv_obj_set_style_text_font(s_battery_label, &lv_font_montserrat_28, 0);
#else
        lv_obj_set_style_text_font(s_battery_label, LV_FONT_DEFAULT, 0);
#endif
#if LV_FONT_MONTSERRAT_40
        lv_obj_set_style_text_font(s_charge_label, &lv_font_montserrat_40, 0);
#elif LV_FONT_MONTSERRAT_48
        lv_obj_set_style_text_font(s_charge_label, &lv_font_montserrat_48, 0);
#elif LV_FONT_MONTSERRAT_36
        lv_obj_set_style_text_font(s_charge_label, &lv_font_montserrat_36, 0);
#elif LV_FONT_MONTSERRAT_28
        lv_obj_set_style_text_font(s_charge_label, &lv_font_montserrat_28, 0);
#else
        lv_obj_set_style_text_font(s_charge_label, LV_FONT_DEFAULT, 0);
#endif
        set_label_scale(s_battery_label, kChargingPercentScale);
        set_label_scale(s_charge_label, kChargingIconScale);
    } else {
#if LV_FONT_MONTSERRAT_28
        lv_obj_set_style_text_font(s_battery_label, &lv_font_montserrat_28, 0);
        set_label_scale(s_battery_label, 256);
#else
        lv_obj_set_style_text_font(s_battery_label, LV_FONT_DEFAULT, 0);
        set_label_scale(s_battery_label, 512);
#endif
#if LV_FONT_MONTSERRAT_14
        lv_obj_set_style_text_font(s_charge_label, &lv_font_montserrat_14, 0);
#else
        lv_obj_set_style_text_font(s_charge_label, LV_FONT_DEFAULT, 0);
#endif
        set_label_scale(s_charge_label, 256);
    }
}

void align_charge_icon() {
    if (!s_charge_label || !s_battery_label || !s_charging) return;
    lv_obj_update_layout(s_battery_label);
    lv_obj_update_layout(s_charge_label);
    // Center the entire visual group, not just the percentage's unscaled box.
    // Cap the percentage scale if a custom font would make the group too wide.
    constexpr int gap = 12;
    const int icon_width = (lv_obj_get_width(s_charge_label) * kChargingIconScale + 255) / 256;
    const int text_width = lv_obj_get_width(s_battery_label);
    if (text_width <= 0) return;
    const int available = lv_obj_get_width(lv_screen_active()) - 40 - gap - icon_width;
    const int fit_scale = available > 0 ? available * 256 / text_width : 256;
    const int scale = fit_scale < kChargingPercentScale ? fit_scale : kChargingPercentScale;
    if (scale <= 0) return;
    if (lv_obj_get_style_transform_scale_x(s_battery_label, LV_PART_MAIN) != scale) {
        set_label_scale(s_battery_label, scale);
    }
    const int battery_width = (text_width * scale + 255) / 256;
    lv_obj_align(s_battery_label, LV_ALIGN_CENTER, -(icon_width + gap) / 2, 0);
    lv_obj_align(s_charge_label, LV_ALIGN_CENTER, (battery_width + gap) / 2, 0);
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
        align_charge_icon();
    }
}

void timer_cb(lv_timer_t *) {
    // Keep LVGL from invalidating screen regions while the panel is off.
    // The active screen is invalidated by WatchDisplay on the next wake-up.
    if (!services::display_power::is_screen_awake()) return;

    if (!s_charging) {
        update_status_labels();
        update_time();
    }
    const uint32_t now = lv_tick_get();
    const uint32_t interval = s_charging ? kChargingBatteryRefreshMs : kBatteryRefreshMs;
    if (!s_battery_sampled || static_cast<uint32_t>(now - s_last_battery_ms) >= interval) {
        s_battery_sampled = true;
        s_last_battery_ms = now;
        update_battery();
    }
}
} // namespace

void refresh_clock_battery() {
    update_battery();
    s_battery_sampled = true;
    s_last_battery_ms = lv_tick_get();
}

void set_clock_charging_mode(bool charging) {
    if (!s_battery_label || !s_charge_label) return;
    if (s_charging != charging) {
        s_charging = charging;
        lv_obj_t *normal_labels[] = {s_time_label, s_sleep_label, s_gps_label,
                                     s_meshcore_label, s_ble_label};
        for (auto *label : normal_labels) {
            if (!label) continue;
            if (charging) lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_remove_flag(label, LV_OBJ_FLAG_HIDDEN);
        }
        set_battery_presentation(charging);
        if (charging) {
            lv_obj_align(s_battery_label, LV_ALIGN_CENTER, 0, 0);
            lv_obj_remove_flag(s_charge_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_charge_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_align(s_battery_label, LV_ALIGN_TOP_MID, 0, 50);
            update_status_labels();
            update_time();
        }
    }
    refresh_clock_battery();
    align_charge_icon();
}

void publish_watch_status(const watch::WatchStatus &status) {
    uint32_t bits = 0;
    if (status.gps_enabled) bits |= kGpsEnabled;
    if (status.gps_has_fix) bits |= kGpsFix;
    if (status.gps_acquiring) bits |= kGpsAcquiring;
    if (status.meshcore_active) bits |= kMeshActive;
    if (status.ble_connected) bits |= kBleLinked;
    bits |= static_cast<uint32_t>(status.gps_satellites) << kSatShift;
    s_rx_packets.store(status.meshcore_rx_packets, std::memory_order_relaxed);
    s_sleep_state.store((status.pm_enabled ? 0x100U : 0U) |
                        static_cast<uint32_t>(status.sleep_percent),
                        std::memory_order_relaxed);
    s_sleep_entries.store(status.sleep_entries, std::memory_order_relaxed);
    s_status_bits.store(bits, std::memory_order_release);
}

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

    // Keep the percentage centered; the small bolt sits to its right.
    s_charge_label = lv_label_create(screen);
    lv_label_set_text(s_charge_label, LV_SYMBOL_CHARGE);
    lv_obj_set_style_text_color(s_charge_label, lv_color_hex(0xEBC36D), 0);
#if LV_FONT_MONTSERRAT_14
    lv_obj_set_style_text_font(s_charge_label, &lv_font_montserrat_14, 0);
#else
    lv_obj_set_style_text_font(s_charge_label, LV_FONT_DEFAULT, 0);
#endif
    lv_obj_add_flag(s_charge_label, LV_OBJ_FLAG_HIDDEN);

    // Screen is 410x502. Small sleep stats fit between battery and clock.
    s_sleep_label = lv_label_create(screen);
    lv_label_set_text(s_sleep_label, "SLEEP --");
    lv_obj_set_style_text_color(s_sleep_label, lv_color_hex(0x8291A5), 0);
#if LV_FONT_MONTSERRAT_28
    lv_obj_set_style_text_font(s_sleep_label, &lv_font_montserrat_28, 0);
#endif
    lv_obj_align(s_sleep_label, LV_ALIGN_TOP_MID, 0, 116);

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

    // Compact status area below the central clock, clear of rounded edges.
    s_gps_label = lv_label_create(screen);
    lv_label_set_text(s_gps_label, "GPS WAIT");
    lv_obj_set_style_text_color(s_gps_label, lv_color_hex(0x8291A5), 0);
#if LV_FONT_MONTSERRAT_40
    lv_obj_set_style_text_font(s_gps_label, &lv_font_montserrat_40, 0);
#else
    // If a custom LVGL config disables the 40px font, scale the existing font.
    lv_obj_set_style_transform_pivot_x(s_gps_label, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(s_gps_label, LV_PCT(50), 0);
    lv_obj_set_style_transform_scale_x(s_gps_label, 512, 0);
    lv_obj_set_style_transform_scale_y(s_gps_label, 512, 0);
#endif
    lv_obj_align(s_gps_label, LV_ALIGN_CENTER, 0, 96);

    s_meshcore_label = lv_label_create(screen);
    lv_label_set_text(s_meshcore_label, "MC OFF");
    lv_obj_set_style_text_color(s_meshcore_label, lv_color_hex(0x8291A5), 0);
#if LV_FONT_MONTSERRAT_40
    lv_obj_set_style_text_font(s_meshcore_label, &lv_font_montserrat_40, 0);
#else
    // If a custom LVGL config disables the 40px font, scale the existing font.
    lv_obj_set_style_transform_pivot_x(s_meshcore_label, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(s_meshcore_label, LV_PCT(50), 0);
    lv_obj_set_style_transform_scale_x(s_meshcore_label, 512, 0);
    lv_obj_set_style_transform_scale_y(s_meshcore_label, 512, 0);
#endif
    lv_obj_align(s_meshcore_label, LV_ALIGN_CENTER, 0, 145);

    s_ble_label = lv_label_create(screen);
    lv_label_set_text(s_ble_label, "BLE DOWN");
    lv_obj_set_style_text_color(s_ble_label, lv_color_hex(0x8291A5), 0);
#if LV_FONT_MONTSERRAT_40
    lv_obj_set_style_text_font(s_ble_label, &lv_font_montserrat_40, 0);
#else
    // If a custom LVGL config disables the 40px font, scale the existing font.
    lv_obj_set_style_transform_pivot_x(s_ble_label, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(s_ble_label, LV_PCT(50), 0);
    lv_obj_set_style_transform_scale_x(s_ble_label, 512, 0);
    lv_obj_set_style_transform_scale_y(s_ble_label, 512, 0);
#endif
    lv_obj_align(s_ble_label, LV_ALIGN_CENTER, 0, 194);

    // Initial update and light periodic refresh, all in the LVGL task.
    timer_cb(nullptr);
    s_update_timer = lv_timer_create(timer_cb, 1000, nullptr);
}
} // namespace ui::watch_display
