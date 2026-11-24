#pragma once
#include <stdint.h>
namespace services::power_management {
struct SleepStats {
    bool pm_enabled = false;
    uint8_t sleep_percent = 0;
    uint32_t sleep_entries = 0;
};
void configure_startup_scaling();
void start_light_sleep(bool meshcore_enabled, bool display_ready, bool radio_receiving);
void restore_wakeup_pins_if_needed();
void update_light_sleep_guard(bool screen_awake, bool gps_powered, bool ble_active);
void report_light_sleep_exits(bool screen_awake, bool gps_powered, bool ble_active);
SleepStats sleep_stats();
}
