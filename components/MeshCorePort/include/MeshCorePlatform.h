
#pragma once

#include <stdint.h>
#include "MeshCore.h"
#include "Dispatcher.h"
#include "Utils.h"

namespace watch {

    class WatchClock final : public mesh::MillisecondClock {
    public:
        unsigned long getMillis() override;
    };

    class WatchRTC final : public mesh::RTCClock {
    public:
        uint32_t getCurrentTime() override;
        void setCurrentTime(uint32_t unix_seconds) override;

    private:
        uint32_t base_seconds_ = 0;
        int64_t base_uptime_us_ = 0;
    };

    class WatchRNG final : public mesh::RNG {
    public:
        void random(uint8_t *dest, size_t length) override;
    };

    class WatchBoard final : public mesh::MainBoard {
    public:
        uint16_t getBattMilliVolts() override;
        const char *getManufacturerName() const override;
        void reboot() override;
        uint8_t getStartupReason() const override;
    };

}
