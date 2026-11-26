#include "IdentityStore.h"

#include "esp_log.h"

#include <string.h>
#include <dirent.h>

#include "MeshCoreSD.h"

#include "ed_25519.h"

namespace meshcore {
    static const char *TAG = "IdentityStore";
    static constexpr const char *PATH = "/sdcard/meshcore/identity.bin";
    static constexpr uint32_t MAGIC = 0x4D434944U;

    // ------------------------------------------------------------------------
    // Load existing identity or initialize an empty store
    // ------------------------------------------------------------------------

    esp_err_t load_or_create_identity(mesh::LocalIdentity &identity, mesh::RNG &rng){
        hardware::sd::Session session;
        if(!session.ok()) return session.error();
        esp_err_t err = hardware::sd::ensure_directory(sdstore::ROOT);
        if(err != ESP_OK) return err;
        struct Keypair { uint8_t bytes[PRV_KEY_SIZE + PUB_KEY_SIZE] = {}; } keypair;
        err = sdstore::read(PATH, MAGIC, keypair);
        if(err == ESP_OK){
            uint8_t derived[PUB_KEY_SIZE] = {};
            ed25519_derive_pub(derived, keypair.bytes);
            if(memcmp(derived, keypair.bytes + PRV_KEY_SIZE, PUB_KEY_SIZE) != 0 || !mesh::LocalIdentity::validatePrivateKey(keypair.bytes)){
                memset(&keypair, 0, sizeof(keypair));
                return ESP_ERR_INVALID_CRC;
            }
            identity.readFrom(keypair.bytes, sizeof(keypair.bytes));
            memset(&keypair, 0, sizeof(keypair));
            ESP_LOGI(TAG, "Identity restored from SD: %02X%02X%02X%02X", identity.pub_key[0], identity.pub_key[1], identity.pub_key[2], identity.pub_key[3]);
            return ESP_OK;
        }
        if(err != ESP_ERR_NOT_FOUND){
            ESP_LOGE(TAG, "Invalid or unreadable identity; refusing to regenerate: %s", esp_err_to_name(err));
            return err;
        }
        DIR *dir = opendir(sdstore::ROOT);
        if(dir == nullptr) return ESP_FAIL;
        bool empty = true;
        struct dirent *entry;
        while((entry = readdir(dir)) != nullptr){
            if(strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0){ empty = false; break; }
        }
        closedir(dir);
        if(!empty){
            ESP_LOGE(TAG, "Missing identity on populated SD; refusing to generate a replacement");
            return ESP_ERR_INVALID_STATE;
        }
        for(int tries = 0; tries < 16; ++tries){
            mesh::LocalIdentity candidate(&rng);
            if(candidate.pub_key[0] == 0 || candidate.pub_key[0] == 0xFF) continue;
            if(candidate.writeTo(keypair.bytes, sizeof(keypair.bytes)) != sizeof(keypair.bytes)) return ESP_FAIL;
            err = sdstore::write(PATH, MAGIC, keypair);
            memset(&keypair, 0, sizeof(keypair));
            if(err != ESP_OK) return err;
            identity = candidate;
            ESP_LOGI(TAG, "Identity created on SD: %02X%02X%02X%02X", identity.pub_key[0], identity.pub_key[1], identity.pub_key[2], identity.pub_key[3]);
            return ESP_OK;
        }
        return ESP_FAIL;
    }
}
