#pragma once

namespace services::power_management {
    void configure_startup_scaling();
    // Call from the main application task after LoRa and BLE initialization.
    void configure_task_wakeups(bool meshcore_enabled);
    // Wait for notifications or the fallback timeout on that same task.
    void wait_for_events();
    void start_light_sleep(bool meshcore_enabled, bool display_ready, bool radio_receiving);
    void restore_wakeup_pins_if_needed();
    void update_light_sleep_guard(bool screen_awake, bool gps_powered, bool ble_active);
}
