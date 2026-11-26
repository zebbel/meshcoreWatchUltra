#include "services.h"

#include "PowerManagement.h"
#include "PositionSync.h"
#include "GPS.h"
#include "BluetoothTransport.h"
#include "DisplayPower.h"

namespace services {
void restore_wakup_pins(){
    power_management::restore_wakeup_pins_if_needed();
}
void loop(){
    position_sync::synchronize();
}
void sleep_guard(){
    hardware::gps::status_t gps{};
    hardware::gps::get_status(gps);
    const bool screen_awake = display_power::is_screen_awake();
    const bool ble_active = hardware::bluetooth::transport().link_active();
    power_management::update_light_sleep_guard(screen_awake, gps.powered, ble_active);
    power_management::wait_for_events();
}
}
