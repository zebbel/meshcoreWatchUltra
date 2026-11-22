
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace meshcore {

    // Standard MeshCore telemetry permission bits.
    static constexpr uint8_t TELEM_BASE = 0x01;
    static constexpr uint8_t TELEM_LOCATION = 0x02;

    class Telemetry {
    public:
        size_t encode(uint8_t permissions, uint8_t *buffer, size_t capacity);

        uint16_t battery_millivolts() const;
    };

}
