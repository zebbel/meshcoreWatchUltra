
#include "Contacts.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "MeshCoreSD.h"
#include <dirent.h>

namespace meshcore {
    static const char *TAG = "Contacts";
    static constexpr const char *DIRECTORY = "/sdcard/meshcore/contacts";
    static constexpr uint32_t CONTACT_MAGIC = 0x4D434354U;

    static void file_path(const uint8_t pub[PUB_KEY_SIZE], char path[128]){
        char hex[PUB_KEY_SIZE * 2 + 1] = {};
        sdstore::hex_key(pub, PUB_KEY_SIZE, hex);
        snprintf(path, 128, "%s/%s.bin", DIRECTORY, hex);
    }

    static uint32_t read_u32(const uint8_t *p){
        uint32_t v;
        memcpy(&v, p, 4);
        return v;
    }

    static void put_u32(uint8_t *p, uint32_t v){
        memcpy(p, &v, 4);
    }

    static bool record_valid(const Contact &c){
        if(!c.present || c.type > 4) return false;
        if(memchr(c.name, 0, sizeof(c.name)) == nullptr) return false;
        if(c.path_len != 0xFF && !mesh::Packet::isValidPathLen(c.path_len)) return false;
        if(c.latitude_micro < -90000000 || c.latitude_micro > 90000000 || c.longitude_micro < -180000000 || c.longitude_micro > 180000000) return false;
        return true;
    }

    esp_err_t Contacts::load(){
        hardware::sd::Session session;
        if(!session.ok()) return session.error();
        esp_err_t err = hardware::sd::ensure_directory(DIRECTORY);
        if(err != ESP_OK) return err;
        DIR *dir = opendir(DIRECTORY);
        if(dir == nullptr) return ESP_FAIL;
        struct dirent *entry;
        size_t loaded = 0;
        while((entry = readdir(dir)) != nullptr){
            const size_t n = strnlen(entry->d_name, sizeof(entry->d_name));
            if(n != 68 && n != 72) continue;

            char name[80] = {};
            memcpy(name, entry->d_name, n + 1);
            // Backups are also candidates, for power-loss recovery.
            if(n == 72 && strcmp(name + 68, ".bak") == 0){ name[68] = 0; }
            else if(n != 68) continue;
            if(strcmp(name + 64, ".bin") != 0) continue;
            if(loaded >= CAPACITY){ closedir(dir); return ESP_ERR_NO_MEM; }
            char path[128] = {};
            snprintf(path, sizeof(path), "%s/%s", DIRECTORY, name);
            Contact item = {};
            err = sdstore::read(path, CONTACT_MAGIC, item);
            if(err != ESP_OK){ closedir(dir); return err; }
            if(!record_valid(item)){ closedir(dir); return ESP_ERR_INVALID_CRC; }
            char expected[128] = {};
            file_path(item.public_key, expected);
            if(strcmp(path, expected) != 0){ closedir(dir); return ESP_ERR_INVALID_CRC; }
            if(find_slot(item.public_key, PUB_KEY_SIZE) >= 0) continue;
            contacts_[loaded++] = item;
        }
        closedir(dir);
        ESP_LOGI(TAG, "Loaded %u contacts from SD", static_cast<unsigned>(loaded));
        return ESP_OK;
    }

    size_t Contacts::count() const{
        size_t n = 0;
        for(const auto &c : contacts_) if(c.present) n++;
        return n;
    }

    const Contact *Contacts::at(size_t i) const{
        return i < CAPACITY && contacts_[i].present ? &contacts_[i] : nullptr;
    }

    int Contacts::find_slot(const uint8_t *pub, size_t length) const{
        if(pub == nullptr || length == 0 || length > PUB_KEY_SIZE) return -1;
        for(size_t i = 0; i < CAPACITY; i++){
            if(contacts_[i].present && memcmp(contacts_[i].public_key, pub, length) == 0) return static_cast<int>(i);
        }
        return -1;
    }

    const Contact *Contacts::find(const uint8_t pub[PUB_KEY_SIZE], size_t length) const{
        int i = find_slot(pub, length);
        return i >= 0 ? &contacts_[i] : nullptr;
    }


    bool Contacts::save_slot(size_t idx, const Contact *value, bool deferred){
        if(idx >= CAPACITY || !hardware::sd::writable()) return false;
        const Contact old = contacts_[idx];
        if(value != nullptr && !record_valid(*value)) return false;
        char path[128] = {};
        file_path(value != nullptr ? value->public_key : old.public_key, path);
        esp_err_t err = ESP_OK;
        if(deferred){
            if(value == nullptr) return false;
            err = sdstore::write(path, CONTACT_MAGIC, *value, true);
        }else{
            // Ensure that a previously queued automatic update cannot resurrect
            // a deleted or explicitly replaced contact.
            // A manual contact change or a newly discovered contact must be
            // persisted now. Keep unrelated advert updates queued for their
            // normal 30-minute flush; cancel only this contact's stale data.
            err = hardware::sd::cancel_pending(path);
            if(err != ESP_OK) return false;
            err = value == nullptr ? hardware::sd::remove_file(path) : sdstore::write(path, CONTACT_MAGIC, *value);
            if(err == ESP_OK && old.present && value != nullptr && memcmp(old.public_key, value->public_key, PUB_KEY_SIZE) != 0){
                char old_path[128] = {};
                file_path(old.public_key, old_path);
                err = hardware::sd::remove_file(old_path);
            }
        }
        if(err != ESP_OK){ ESP_LOGE(TAG, "Cannot save SD contact: %s", esp_err_to_name(err)); return false; }
        contacts_[idx] = value != nullptr ? *value : Contact{};
        return true;
    }


    ContactUpdateResult Contacts::update_from_wire(const uint8_t *f, size_t len, uint32_t now){
        constexpr size_t REQUIRED = 1 + PUB_KEY_SIZE + 3 + MAX_PATH_SIZE + 32 + 4;
        if(f == nullptr || len < REQUIRED || len > WIRE_LENGTH || f[0] != 9){
            ESP_LOGW(TAG, "Invalid contact frame length=%u", static_cast<unsigned>(len));
            return ContactUpdateResult::INVALID;
        }
        size_t i = 1;
        Contact c = {};
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

    bool Contacts::remove(const uint8_t pub[PUB_KEY_SIZE]){
        int i = find_slot(pub, PUB_KEY_SIZE);
        return i >= 0 && save_slot(i, nullptr);
    }

    bool Contacts::reset_path(const uint8_t pub[PUB_KEY_SIZE]){
        int i = find_slot(pub, PUB_KEY_SIZE);
        if(i < 0) return false;
        Contact c = contacts_[i];
        c.path_len = 0xFF;
        memset(c.path, 0, sizeof(c.path));
        return save_slot(i, &c);
    }

    bool Contacts::observe_advert(const mesh::Identity &id, uint32_t timestamp, const uint8_t *data, size_t len, uint32_t now, bool auto_add, uint8_t mask){
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
        Contact c = existing >= 0 ? contacts_[idx] : Contact{};
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
        // Auto-discovered NEW contacts must be persisted immediately.
        // Existing contacts only update their queued SD snapshot. A new
        // advert replaces the previous queued snapshot for that public key.
        return save_slot(idx, &c, existing >= 0);
    }

    size_t Contacts::to_wire(uint8_t f[WIRE_LENGTH], const Contact &c) const{
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
