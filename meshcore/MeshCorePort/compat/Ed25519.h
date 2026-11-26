#pragma once

#include <stddef.h>
#include <stdint.h>

// Compatibility interface for upstream MeshCore Identity.cpp.
// Implementation is provided separately to avoid conflicting
// ED25519_H include guards between MeshCore and Monocypher.
class Ed25519 {
public:
    static bool verify(const uint8_t signature[64], const uint8_t public_key[32], const void *message, size_t length);
};
