
#include "IdentityStore.h"

#include <string.h>
#include "ed_25519.h"
#include "esp_log.h"
#include "Storage.h"

namespace meshcore {

    static const char *TAG = "IdentityStore";

    esp_err_t load_or_create_identity(mesh::LocalIdentity &identity, mesh::RNG &rng){
        hardware::storage::Handle handle;
        esp_err_t err = hardware::storage::open("meshcore", hardware::storage::Access::read_write, handle);
        if(err != ESP_OK) return err;

        uint8_t keypair[PRV_KEY_SIZE + PUB_KEY_SIZE] = {};
        size_t size = sizeof(keypair);
        err = hardware::storage::read_blob(handle, "keypair", keypair, &size);

        if(err == ESP_OK){
            if(size != sizeof(keypair)){
                hardware::storage::close(handle);
                ESP_LOGE(TAG, "Incorrect stored identity length. Will not overwrite it");
                return ESP_ERR_INVALID_SIZE;
            }

            uint8_t derived_pub[PUB_KEY_SIZE] = {};
            ed25519_derive_pub(derived_pub, keypair);
            if(memcmp(derived_pub, keypair + PRV_KEY_SIZE, PUB_KEY_SIZE) != 0 || !mesh::LocalIdentity::validatePrivateKey(keypair)){
                memset(keypair, 0, sizeof(keypair));
                hardware::storage::close(handle);
                ESP_LOGE(TAG, "Stored identity is invalid. Will not regenerate automatically");
                return ESP_ERR_INVALID_CRC;
            }

            identity.readFrom(keypair, sizeof(keypair));
            memset(keypair, 0, sizeof(keypair));
            hardware::storage::close(handle);
            ESP_LOGI(TAG, "Restored persistent identity %02X%02X%02X%02X", identity.pub_key[0], identity.pub_key[1], identity.pub_key[2], identity.pub_key[3]);
            return ESP_OK;
        }

        if(hardware::storage::is_not_found(err)){
            hardware::storage::close(handle);
            return err;
        }

        for(int tries = 0; tries < 16; tries++){
            mesh::LocalIdentity candidate(&rng);
            if(candidate.pub_key[0] == 0x00 || candidate.pub_key[0] == 0xFF) continue;

            if(candidate.writeTo(keypair, sizeof(keypair)) != sizeof(keypair)){
                hardware::storage::close(handle);
                return ESP_FAIL;
            }
            err = hardware::storage::write_blob(handle, "keypair", keypair, sizeof(keypair));
            if(err == ESP_OK) err = hardware::storage::commit(handle);
            memset(keypair, 0, sizeof(keypair));

            if(err != ESP_OK){
                hardware::storage::close(handle);
                return err;
            }

            identity = candidate;
            hardware::storage::close(handle);
            ESP_LOGI(TAG, "Generated and saved identity %02X%02X%02X%02X", identity.pub_key[0], identity.pub_key[1], identity.pub_key[2], identity.pub_key[3]);
            return ESP_OK;
        }

        hardware::storage::close(handle);
        return ESP_FAIL;
    }

}
