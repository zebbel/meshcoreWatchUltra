
#include "MeshCorePlatform.h"

#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"

namespace watch {

    unsigned long WatchClock::getMillis(){
        return static_cast<unsigned long>(esp_timer_get_time() / 1000ULL);
    }

    uint32_t WatchRTC::getCurrentTime(){
        if(base_seconds_ == 0) return 0;

        return base_seconds_ + static_cast<uint32_t>((esp_timer_get_time() - base_uptime_us_) / 1000000LL);
    }

    void WatchRTC::setCurrentTime(uint32_t unix_seconds){
        base_uptime_us_ = esp_timer_get_time();
        base_seconds_ = unix_seconds;
    }

    void WatchRNG::random(uint8_t *dest, size_t length){
        if(dest != nullptr && length > 0) esp_fill_random(dest, length);
    }

    uint16_t WatchBoard::getBattMilliVolts(){
        return 0; // TODO: implement AXP2101 battery-voltage measurement.
    }

    const char *WatchBoard::getManufacturerName() const{
        return "LilyGO";
    }

    void WatchBoard::reboot(){
        esp_restart();
    }

    uint8_t WatchBoard::getStartupReason() const{
        return BD_STARTUP_NORMAL;
    }

}
