
#pragma once

#include <stdint.h>

#include "MeshCore.h"
#include "Dispatcher.h"
#include "Utils.h"

namespace meshcore {

    class ClockAdapter final : public mesh::MillisecondClock {
    public:
        unsigned long getMillis() override;
    };

    class RTCAdapter final : public mesh::RTCClock {
    public:
        uint32_t getCurrentTime() override;
        void setCurrentTime(uint32_t unix_seconds) override;

    };

    class RandomAdapter final : public mesh::RNG {
    public:
        void random(uint8_t *dest, size_t length) override;
    };

    class BoardAdapter final : public mesh::MainBoard {
    public:
        uint16_t getBattMilliVolts() override;
        const char *getManufacturerName() const override;
        void reboot() override;
        uint8_t getStartupReason() const override;
    };

}
