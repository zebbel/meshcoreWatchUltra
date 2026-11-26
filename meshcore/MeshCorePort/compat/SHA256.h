
#pragma once

#include "esp_log.h"
#include "psa/crypto.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

// SHA256 and HMAC compatibility for the original MeshCore Crypto API.
// Backed by ESP-IDF PSA Crypto.
class SHA256 {
public:
    SHA256(){
        check(psa_crypto_init(), "PSA initialization");
        check(psa_hash_setup(&hash_, PSA_ALG_SHA_256), "SHA256 setup");
    }

    ~SHA256(){
        psa_hash_abort(&hash_);
        psa_mac_abort(&mac_);

        if(mac_key_ != 0){
            psa_destroy_key(mac_key_);
        }
    }

    SHA256(const SHA256 &) = delete;
    SHA256 &operator=(const SHA256 &) = delete;

    void resetHMAC(const uint8_t *key, size_t length){
        if(key == nullptr || length == 0){
            abort();
        }

        psa_hash_abort(&hash_);
        psa_mac_abort(&mac_);

        if(mac_key_ != 0){
            check(psa_destroy_key(mac_key_), "Destroy old HMAC key");
            mac_key_ = 0;
        }

        psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;

        psa_set_key_type(&attrs, PSA_KEY_TYPE_HMAC);
        psa_set_key_bits(&attrs, length * 8);
        psa_set_key_usage_flags(&attrs, PSA_KEY_USAGE_SIGN_MESSAGE);
        psa_set_key_algorithm(&attrs, PSA_ALG_HMAC(PSA_ALG_SHA_256));

        psa_status_t status = psa_import_key(&attrs, key, length, &mac_key_);

        psa_reset_key_attributes(&attrs);
        check(status, "HMAC key import");

        check(psa_mac_sign_setup(&mac_, mac_key_, PSA_ALG_HMAC(PSA_ALG_SHA_256)), "HMAC setup");

        hmac_mode_ = true;
    }

    void update(const void *data, size_t length){
        if(length == 0) return;
        if(data == nullptr) abort();

        if(hmac_mode_){
            check(psa_mac_update(&mac_, static_cast<const uint8_t *>(data), length), "HMAC update");
        }else{
            check(psa_hash_update(&hash_, static_cast<const uint8_t *>(data), length), "SHA256 update");
        }
    }

    void finalize(uint8_t *output, size_t length){
        if(hmac_mode_ || length > 32 || (length > 0 && output == nullptr)){
            abort();
        }

        uint8_t digest[32] = {};
        size_t digest_length = 0;

        check(psa_hash_finish(&hash_, digest, sizeof(digest), &digest_length), "SHA256 finish");

        if(digest_length != sizeof(digest)){
            abort();
        }

        if(length > 0){
            memcpy(output, digest, length);
        }

        memset(digest, 0, sizeof(digest));
    }

    void finalizeHMAC(const uint8_t *key, size_t key_length, uint8_t *output, size_t length){
        (void)key;
        (void)key_length;

        if(!hmac_mode_ || length > 32 || (length > 0 && output == nullptr)){
            abort();
        }

        uint8_t mac[32] = {};
        size_t mac_length = 0;

        check(psa_mac_sign_finish(&mac_, mac, sizeof(mac), &mac_length), "HMAC finish");

        if(mac_length != sizeof(mac)){
            abort();
        }

        if(length > 0){
            memcpy(output, mac, length);
        }

        memset(mac, 0, sizeof(mac));

        hmac_mode_ = false;

        check(psa_destroy_key(mac_key_), "Destroy HMAC key");

        mac_key_ = 0;
    }

private:
    psa_hash_operation_t hash_ = PSA_HASH_OPERATION_INIT;
    psa_mac_operation_t mac_ = PSA_MAC_OPERATION_INIT;

    psa_key_id_t mac_key_ = 0;
    bool hmac_mode_ = false;

    static void check(psa_status_t status, const char *what){
        if(status != PSA_SUCCESS){
            ESP_LOGE("MeshSHA256", "%s failed: %ld", what, static_cast<long>(status));
            abort();
        }
    }
};
