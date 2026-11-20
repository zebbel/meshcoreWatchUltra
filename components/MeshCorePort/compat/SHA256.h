
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "psa/crypto.h"

// Compatibility wrapper for the original MeshCore SHA256 API.
// Uses ESP-IDF 6.x PSA Crypto instead of the removed mbedTLS SHA256 API.
class SHA256 {
public:
    SHA256(){
        check(psa_crypto_init(), "Crypto initialization");
        check(psa_hash_setup(&operation_, PSA_ALG_SHA_256), "SHA256 setup");
    }

    ~SHA256(){
        psa_hash_abort(&operation_);
    }

    SHA256(const SHA256 &) = delete;
    SHA256 &operator=(const SHA256 &) = delete;

    void update(const void *data, size_t length){
        if(length == 0) return;

        check(
            psa_hash_update(
                &operation_,
                static_cast<const uint8_t *>(data),
                length
            ),
            "SHA256 update"
        );
    }

    void finalize(uint8_t *output, size_t length){
        uint8_t digest[32] = {};
        size_t digest_length = 0;

        check(
            psa_hash_finish(
                &operation_,
                digest,
                sizeof(digest),
                &digest_length
            ),
            "SHA256 finish"
        );

        if(digest_length != sizeof(digest)){
            ESP_LOGE("SHA256", "Invalid SHA256 digest length");
            abort();
        }

        if(length > sizeof(digest)){
            length = sizeof(digest);
        }

        if(length > 0 && output != nullptr){
            memcpy(output, digest, length);
        }

        memset(digest, 0, sizeof(digest));
    }

private:
    psa_hash_operation_t operation_ = PSA_HASH_OPERATION_INIT;

    static void check(psa_status_t status, const char *operation){
        if(status != PSA_SUCCESS){
            ESP_LOGE("SHA256", "%s failed: %ld",
                     operation, static_cast<long>(status));
            abort();
        }
    }
};
