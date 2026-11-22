#pragma once

#include <stddef.h>
#include <stdint.h>

namespace hardware::system {
    int64_t micros();
    uint32_t millis();
    void random_bytes(uint8_t *out, size_t length);
    void enable_boot_random();
    void disable_boot_random();
    void restart();
    uint32_t storage_partition_kib();
}
