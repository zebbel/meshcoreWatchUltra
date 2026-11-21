
#include "WatchContacts.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "nvs.h"

namespace watch {
    static const char *TAG = "WatchContacts";
    static constexpr char NAMESPACE[] = "meshcontacts";

    static void slot_key(int i, char dest[8]){
        snprintf(dest, 8, "peer%02d", i);
    }

    static uint32_t read_u32(const uint8_t *p){
        uint32_t v;
        memcpy(&v, p, 4);
        return v;
    }

    static void put_u32(uint8_t *p, uint32_t v){
        memcpy(p, &v, 4);
    }

    static bool record_valid(const WatchContact &c){
        if(!c.present || c.type > 4) return false;
        if(memchr(c.name, 0, sizeof(c.name)) == nullptr) return false;
        if(c.path_len != 0xFF && !mesh::Packet::isValidPathLen(c.path_len)) return false;
        if(c.latitude_micro < -90000000 || c.latitude_micro > 90000000 || c.longitude_micro < -180000000 || c.longitude_micro > 180000000) return false;
        return true;
    }

    esp_err_t WatchContacts::load(){
        nvs_handle_t h = 0;

        // Load only from the NEW contacts partition.
        esp_err_t err = nvs_open_from_partition("contacts", NAMESPACE, NVS_READWRITE, &h);

        if(err != ESP_OK){
            ESP_LOGE(TAG, "Cannot open contacts partition: %s", esp_err_to_name(err));
            return err;
        }

        for(size_t i = 0; i < CAPACITY; i++){
            char key[8];
            slot_key(static_cast<int>(i), key);

            WatchContact item = {};
            size_t size = sizeof(item);

            err = nvs_get_blob(h, key, &item, &size);

            if(err == ESP_ERR_NVS_NOT_FOUND)
                continue;

            if(err != ESP_OK || size != sizeof(item) || !record_valid(item)){

                ESP_LOGE(TAG, "Invalid contact record %s", key);

                nvs_close(h);
                return ESP_ERR_INVALID_CRC;
            }

            contacts_[i] = item;
        }

        nvs_close(h);

        ESP_LOGI(TAG, "Loaded %u contacts from new NVS", static_cast<unsigned>(count()));

        return ESP_OK;
    }

    size_t WatchContacts::count() const{
        size_t n = 0;
        for(const auto &c : contacts_) if(c.present) n++;
        return n;
    }

    const WatchContact *WatchContacts::at(size_t i) const{
        return i < CAPACITY && contacts_[i].present ? &contacts_[i] : nullptr;
    }

    int WatchContacts::find_slot(const uint8_t *pub, size_t length) const{
        if(pub == nullptr || length == 0 || length > PUB_KEY_SIZE) return -1;
        for(size_t i = 0; i < CAPACITY; i++){
            if(contacts_[i].present && memcmp(contacts_[i].public_key, pub, length) == 0) return static_cast<int>(i);
        }
        return -1;
    }

    const WatchContact *WatchContacts::find(const uint8_t pub[PUB_KEY_SIZE], size_t length) const{
        int i = find_slot(pub, length);
        return i >= 0 ? &contacts_[i] : nullptr;
    }


    bool WatchContacts::save_slot(size_t idx, const WatchContact *value){
        if(idx >= CAPACITY)
            return false;

        char key[8];
        slot_key(static_cast<int>(idx), key);

        nvs_handle_t h = 0;

        // Write only to the NEW contacts partition.
        esp_err_t err = nvs_open_from_partition("contacts", NAMESPACE, NVS_READWRITE, &h);

        if(err != ESP_OK){
            ESP_LOGE(TAG, "Cannot open contacts storage: %s", esp_err_to_name(err));
            return false;
        }

        if(value != nullptr){
            err = nvs_set_blob(h, key, value, sizeof(*value));
        }else{
            err = nvs_erase_key(h, key);
        }

        if(err == ESP_OK)
            err = nvs_commit(h);

        nvs_close(h);

        if(err != ESP_OK){
            ESP_LOGE(TAG, "Contact write failed %s: %s", key, esp_err_to_name(err));
            return false;
        }

        contacts_[idx] = value ? *value : WatchContact{};

        return true;
    }


    ContactUpdateResult WatchContacts::update_from_wire(const uint8_t *f, size_t len, uint32_t now){
        constexpr size_t REQUIRED = 1 + PUB_KEY_SIZE + 3 + MAX_PATH_SIZE + 32 + 4;
        if(f == nullptr || len < REQUIRED || len > WIRE_LENGTH || f[0] != 9){
            ESP_LOGW(TAG, "Invalid contact frame length=%u", static_cast<unsigned>(len));
            return ContactUpdateResult::INVALID;
        }
        size_t i = 1;
        WatchContact c = {};
        memcpy(c.public_key, &f[i], PUB_KEY_SIZE); i += PUB_KEY_SIZE;
        c.type = f[i++]; c.flags = f[i++]; c.path_len = f[i++];
        memcpy(c.path, &f[i], MAX_PATH_SIZE); i += MAX_PATH_SIZE;
        memcpy(c.name, &f[i], sizeof(c.name)); i += sizeof(c.name);
        // Wire names are fixed-width, not necessarily NUL terminated.
        c.name[sizeof(c.name) - 1] = 0;
        c.advert_time = read_u32(&f[i]); i += 4;
        if(len >= i + 8){
            c.latitude_micro = static_cast<int32_t>(read_u32(&f[i])); i += 4;
            c.longitude_micro = static_cast<int32_t>(read_u32(&f[i])); i += 4;
        }
        c.lastmod = len >= i + 4 ? read_u32(&f[i]) : now;
        if(c.lastmod == 0) c.lastmod = now ? now : 1;
        c.present = true;
        if(!record_valid(c)) return ContactUpdateResult::INVALID;
        int idx = find_slot(c.public_key, PUB_KEY_SIZE);
        if(idx < 0){
            for(size_t j = 0; j < CAPACITY; j++) if(!contacts_[j].present){ idx = j; break; }
        }
        if(idx < 0) return ContactUpdateResult::FULL;
        if(!save_slot(idx, &c)) return ContactUpdateResult::STORAGE_ERROR;
        return ContactUpdateResult::SAVED;
    }

    bool WatchContacts::remove(const uint8_t pub[PUB_KEY_SIZE]){
        int i = find_slot(pub, PUB_KEY_SIZE);
        return i >= 0 && save_slot(i, nullptr);
    }

    bool WatchContacts::reset_path(const uint8_t pub[PUB_KEY_SIZE]){
        int i = find_slot(pub, PUB_KEY_SIZE);
        if(i < 0) return false;
        WatchContact c = contacts_[i];
        c.path_len = 0xFF;
        memset(c.path, 0, sizeof(c.path));
        return save_slot(i, &c);
    }

    bool WatchContacts::observe_advert(const mesh::Identity &id, uint32_t timestamp, const uint8_t *data, size_t len, uint32_t now, bool auto_add, uint8_t mask){
        if(data == nullptr || len < 1) return false;
        uint8_t type = data[0] & 0x0F;
        if(type == 0 || type > 4) return false;
        int existing = find_slot(id.pub_key, PUB_KEY_SIZE);
        if(existing < 0 && !auto_add) return false;
        if(existing < 0 && mask != 0 && ((mask & (1 << type)) == 0)) return false;
        int idx = existing;
        if(idx < 0){
            for(size_t i = 0; i < CAPACITY; i++) if(!contacts_[i].present){ idx = i; break; }
        }
        if(idx < 0) return false;
        WatchContact c = existing >= 0 ? contacts_[idx] : WatchContact{};
        memcpy(c.public_key, id.pub_key, PUB_KEY_SIZE);
        c.type = type;
        size_t at = 1;
        if(data[0] & 0x10){
            if(at + 8 > len) return false;
            c.latitude_micro = static_cast<int32_t>(read_u32(&data[at])); at += 4;
            c.longitude_micro = static_cast<int32_t>(read_u32(&data[at])); at += 4;
        }
        if(data[0] & 0x20) at += 2;
        if(data[0] & 0x40) at += 2;
        if(at > len) return false;
        if(data[0] & 0x80){
            size_t n = len - at;
            if(n > 31) n = 31;
            memset(c.name, 0, sizeof(c.name));
            memcpy(c.name, &data[at], n);
        }
        c.present = true;
        if(!record_valid(c)) return false;
        if(existing >= 0 && timestamp == c.advert_time) return true; // avoid repeated flash writes
        c.advert_time = timestamp;
        c.lastmod = now ? now : 1;
        return save_slot(idx, &c);
    }

    size_t WatchContacts::to_wire(uint8_t f[WIRE_LENGTH], const WatchContact &c) const{
        if(!record_valid(c)) return 0;
        size_t i = 0;
        f[i++] = 3; // RESP_CODE_CONTACT
        memcpy(&f[i], c.public_key, PUB_KEY_SIZE); i += PUB_KEY_SIZE;
        f[i++] = c.type; f[i++] = c.flags; f[i++] = c.path_len;
        memcpy(&f[i], c.path, MAX_PATH_SIZE); i += MAX_PATH_SIZE;
        memcpy(&f[i], c.name, 32); i += 32;
        put_u32(&f[i], c.advert_time); i += 4;
        put_u32(&f[i], static_cast<uint32_t>(c.latitude_micro)); i += 4;
        put_u32(&f[i], static_cast<uint32_t>(c.longitude_micro)); i += 4;
        put_u32(&f[i], c.lastmod); i += 4;
        return i;
    }
}
