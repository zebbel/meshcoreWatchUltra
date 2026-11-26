#include "PlatformAdapters.h"

#include "esp_log.h"

#include "HardwareSystem.h"
#include "TimeService.h"

namespace meshcore {

    // ------------------------------------------------------------------------
    // Monotonic clock
    // ------------------------------------------------------------------------

    unsigned long ClockAdapter::getMillis(){
        return static_cast<unsigned long>(hardware::system::micros() / 1000ULL);
    }

    // ------------------------------------------------------------------------
    // UTC clock adapter
    // ------------------------------------------------------------------------

    uint32_t RTCAdapter::getCurrentTime(){
        return services::time_service::utc_time();
    }

    void RTCAdapter::setCurrentTime(uint32_t unix_seconds){
        const esp_err_t err = services::time_service::set_utc_time(unix_seconds);
        if(err != ESP_OK) ESP_LOGW("RTCAdapter", "Companion time was not stored to RTC: %s", esp_err_to_name(err));
    }

    // ------------------------------------------------------------------------
    // Hardware random source
    // ------------------------------------------------------------------------

    void RandomAdapter::random(uint8_t *dest, size_t length){
        if(dest != nullptr && length > 0) hardware::system::random_bytes(dest, length);
    }

    // ------------------------------------------------------------------------
    // Board interface
    // ------------------------------------------------------------------------

    uint16_t BoardAdapter::getBattMilliVolts(){
        return 0; // TODO: implement AXP2101 battery-voltage measurement.
    }

    const char *BoardAdapter::getManufacturerName() const{
        return "LilyGO";
    }

    void BoardAdapter::reboot(){
        hardware::system::restart();
    }

    uint8_t BoardAdapter::getStartupReason() const{
        return BD_STARTUP_NORMAL;
    }

}
