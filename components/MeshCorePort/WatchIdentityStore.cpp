
#include "WatchIdentityStore.h"

#include <string.h>
#include "ed_25519.h"
#include "esp_log.h"
#include "nvs.h"

namespace watch {

    static const char *TAG = "WatchIdentity";

    esp_err_t load_or_create_identity(mesh::LocalIdentity &identity, mesh::RNG &rng){
        nvs_handle_t handle;
        esp_err_t err = nvs_open("meshcore", NVS_READWRITE, &handle);
        if(err != ESP_OK) return err;

        uint8_t keypair[PRV_KEY_SIZE + PUB_KEY_SIZE] = {};
        size_t size = sizeof(keypair);
        err = nvs_get_blob(handle, "keypair", keypair, &size);

        if(err == ESP_OK){
            if(size != sizeof(keypair)){
                nvs_close(handle);
                ESP_LOGE(TAG, "Incorrect stored identity length. Will not overwrite it");
                return ESP_ERR_INVALID_SIZE;
            }

            uint8_t derived_pub[PUB_KEY_SIZE] = {};
            ed25519_derive_pub(derived_pub, keypair);
            if(memcmp(derived_pub, keypair + PRV_KEY_SIZE, PUB_KEY_SIZE) != 0 || !mesh::LocalIdentity::validatePrivateKey(keypair)){
                memset(keypair, 0, sizeof(keypair));
                nvs_close(handle);
                ESP_LOGE(TAG, "Stored identity is invalid. Will not regenerate automatically");
                return ESP_ERR_INVALID_CRC;
            }

            identity.readFrom(keypair, sizeof(keypair));
            memset(keypair, 0, sizeof(keypair));
            nvs_close(handle);
            ESP_LOGI(TAG, "Restored persistent identity %02X%02X%02X%02X", identity.pub_key[0], identity.pub_key[1], identity.pub_key[2], identity.pub_key[3]);
            return ESP_OK;
        }

        if(err != ESP_ERR_NVS_NOT_FOUND){
            nvs_close(handle);
            return err;
        }

        for(int tries = 0; tries < 16; tries++){
            mesh::LocalIdentity candidate(&rng);
            if(candidate.pub_key[0] == 0x00 || candidate.pub_key[0] == 0xFF) continue;

            if(candidate.writeTo(keypair, sizeof(keypair)) != sizeof(keypair)){
                nvs_close(handle);
                return ESP_FAIL;
            }
            err = nvs_set_blob(handle, "keypair", keypair, sizeof(keypair));
            if(err == ESP_OK) err = nvs_commit(handle);
            memset(keypair, 0, sizeof(keypair));

            if(err != ESP_OK){
                nvs_close(handle);
                return err;
            }

            identity = candidate;
            nvs_close(handle);
            ESP_LOGI(TAG, "Generated and saved identity %02X%02X%02X%02X", identity.pub_key[0], identity.pub_key[1], identity.pub_key[2], identity.pub_key[3]);
            return ESP_OK;
        }

        nvs_close(handle);
        return ESP_FAIL;
    }

}
