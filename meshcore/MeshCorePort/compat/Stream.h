#pragma once

#include <stddef.h>
#include <stdint.h>

// Minimal interface needed by upstream Identity.cpp and Utils.cpp.
// Not a complete Arduino Stream implementation.
class Stream {
public:
    virtual ~Stream() = default;

    virtual size_t readBytes(uint8_t *buffer, size_t length) = 0;
    virtual size_t write(const uint8_t *buffer, size_t length) = 0;
    virtual size_t print(const char *value) = 0;
    virtual size_t print(char value) = 0;
    virtual size_t println() = 0;
};
