#include "services.h"

#include "sdkconfig.h"

#include "PowerManagement.h"
#include "PositionSync.h"
#include "GPS.h"
#include "BluetoothTransport.h"
#include "DisplayPower.h"
#include "BHI260AP.h"

namespace services {
    // ------------------------------------------------------------------------
    // Before component polling
    // ------------------------------------------------------------------------

    void restore_wakup_pins(){
        power_management::restore_wakeup_pins_if_needed();
    }
    // ------------------------------------------------------------------------
    // Service polling
    // ------------------------------------------------------------------------

    void loop(){
        // Consume each wrist event, even when the display is already awake.
        const bool wrist = hardware::bhi260ap::wrist_tilt_detected();
#if defined(CONFIG_MCW_RAISE_TO_WAKE)
        if(wrist) display_power::request_wrist_wake();
#else
        (void)wrist;
#endif
        position_sync::synchronize();
    }
    // ------------------------------------------------------------------------
    // After component polling
    // ------------------------------------------------------------------------

    // Sample live state and wait only after all application components have run.
    void sleep_guard(){
        hardware::gps::status_t gps{};
        hardware::gps::get_status(gps);
        const bool screen_awake = display_power::is_screen_awake();
        const bool ble_active = hardware::bluetooth::transport().link_active();
        power_management::update_light_sleep_guard(screen_awake, gps.powered, ble_active);
        power_management::wait_for_events();
    }
}
