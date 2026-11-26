
#include "Ed25519.h"

#include "monocypher-ed25519.h"

bool Ed25519::verify(const uint8_t signature[64], const uint8_t public_key[32], const void *message, size_t length){
    if(signature == nullptr || public_key == nullptr || (message == nullptr && length != 0)) return false;
    return crypto_ed25519_check(signature, public_key, static_cast<const uint8_t *>(message), length) == 0;
}
