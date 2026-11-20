
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "psa/crypto.h"

// Compatibility adapter for the AES128 interface used by
// upstream MeshCore Utils.cpp.
class AES128 {
public:
    AES128(){
        check(psa_crypto_init(), "PSA initialization");
    }

    ~AES128(){
        if(key_id_ != 0){
            psa_destroy_key(key_id_);
        }
    }

    AES128(const AES128 &) = delete;
    AES128 &operator=(const AES128 &) = delete;

    void setKey(const uint8_t *key, size_t length){
        if(key == nullptr || length != 16){
            ESP_LOGE("MeshAES", "Invalid AES-128 key");
            abort();
        }

        if(key_id_ != 0){
            check(psa_destroy_key(key_id_), "Destroy prior key");
            key_id_ = 0;
        }

        psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;

        psa_set_key_type(&attrs, PSA_KEY_TYPE_AES);
        psa_set_key_bits(&attrs, 128);
        psa_set_key_usage_flags(
            &attrs,
            PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT
        );
        psa_set_key_algorithm(&attrs, PSA_ALG_ECB_NO_PADDING);

        psa_status_t status = psa_import_key(
            &attrs,
            key,
            length,
            &key_id_
        );

        psa_reset_key_attributes(&attrs);
        check(status, "Import AES key");
    }

    void encryptBlock(uint8_t *dest, const uint8_t *src){
        crypt_block(dest, src, true);
    }

    void decryptBlock(uint8_t *dest, const uint8_t *src){
        crypt_block(dest, src, false);
    }

private:
    psa_key_id_t key_id_ = 0;

    static void check(psa_status_t status, const char *what){
        if(status != PSA_SUCCESS){
            ESP_LOGE(
                "MeshAES",
                "%s failed: %ld",
                what,
                static_cast<long>(status)
            );
            abort();
        }
    }

    void crypt_block(uint8_t *dest, const uint8_t *src, bool encrypt){
        if(key_id_ == 0 || dest == nullptr || src == nullptr){
            abort();
        }

        uint8_t output[32] = {};
        size_t output_length = 0;

        psa_status_t status = encrypt ?
            psa_cipher_encrypt(
                key_id_,
                PSA_ALG_ECB_NO_PADDING,
                src,
                16,
                output,
                sizeof(output),
                &output_length
            ) :
            psa_cipher_decrypt(
                key_id_,
                PSA_ALG_ECB_NO_PADDING,
                src,
                16,
                output,
                sizeof(output),
                &output_length
            );

        check(status, encrypt ? "AES encrypt" : "AES decrypt");

        if(output_length != 16){
            abort();
        }

        memcpy(dest, output, 16);
        memset(output, 0, sizeof(output));
    }
};
