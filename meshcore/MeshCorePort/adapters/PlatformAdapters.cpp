
#include "PlatformAdapters.h"
#include "HardwareSystem.h"





namespace meshcore {

    unsigned long ClockAdapter::getMillis(){
        return static_cast<unsigned long>(hardware::system::micros() / 1000ULL);
    }

    uint32_t RTCAdapter::getCurrentTime(){
        if(base_seconds_ == 0) return 0;

        return base_seconds_ + static_cast<uint32_t>((hardware::system::micros() - base_uptime_us_) / 1000000LL);
    }

    void RTCAdapter::setCurrentTime(uint32_t unix_seconds){
        base_uptime_us_ = hardware::system::micros();
        base_seconds_ = unix_seconds;
    }

    void RandomAdapter::random(uint8_t *dest, size_t length){
        if(dest != nullptr && length > 0) hardware::system::random_bytes(dest, length);
    }

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
