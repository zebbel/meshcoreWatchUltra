
#include "Contacts.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include <stdio.h>
#include <string.h>
#include <memory>

#include "MeshCoreSD.h"

namespace meshcore {
    static const char *TAG = "Contacts";
    static constexpr const char *DATABASE = "/sdcard/meshcore/contacts.bin";
    static constexpr uint32_t DATABASE_MAGIC = 0x4244434DU; // "MCDB" in little endian
    static constexpr uint32_t DATABASE_VERSION = 1;
    static constexpr size_t HEADER_BYTES = 20;
    static constexpr size_t CHECKSUM_OFFSET = 16;
    static constexpr size_t RECORD_BYTES = Contacts::WIRE_LENGTH;
    static constexpr size_t MAX_DATABASE_BYTES = HEADER_BYTES + Contacts::CAPACITY * RECORD_BYTES;
    static constexpr uint32_t DEFER_MS = 30U * 60U * 1000U;
    static constexpr uint32_t RETRY_MS = 30U * 1000U;

    // Only temporary serialization storage; the existing Contacts table is
    // still the sole live RAM database. Do not put the file on a task stack.
    using Buffer = std::unique_ptr<uint8_t, decltype(&heap_caps_free)>;
    static Buffer allocate_buffer(size_t bytes){
        return Buffer(static_cast<uint8_t *>(heap_caps_malloc(
            bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)), &heap_caps_free);
    }

    static uint32_t monotonic_ms(){
        return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
    }

    static uint32_t read_u32(const uint8_t *p){
        return static_cast<uint32_t>(p[0]) |
            (static_cast<uint32_t>(p[1]) << 8U) |
            (static_cast<uint32_t>(p[2]) << 16U) |
            (static_cast<uint32_t>(p[3]) << 24U);
    }

    static void put_u32(uint8_t *p, uint32_t v){
        for(unsigned i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8U * i));
    }

    static bool record_valid(const Contact &c){
        if(!c.present || c.type > 4) return false;
        if(memchr(c.name, 0, sizeof(c.name)) == nullptr) return false;
        if(c.path_len != 0xFF && !mesh::Packet::isValidPathLen(c.path_len)) return false;
        if(c.latitude_micro < -90000000 || c.latitude_micro > 90000000 || c.longitude_micro < -180000000 || c.longitude_micro > 180000000) return false;
        return true;
    }

    // Decode exactly one complete contact response record. Unlike incoming
    // partial wire commands, stored records must already be well formed.
    static bool decode_record(const uint8_t *f, Contact &c){
        if(f[0] != 3) return false;
        size_t i = 1;
        c = Contact{};
        memcpy(c.public_key, &f[i], PUB_KEY_SIZE); i += PUB_KEY_SIZE;
        c.type = f[i++]; c.flags = f[i++]; c.path_len = f[i++];
        memcpy(c.path, &f[i], MAX_PATH_SIZE); i += MAX_PATH_SIZE;
        memcpy(c.name, &f[i], sizeof(c.name)); i += sizeof(c.name);
        c.advert_time = read_u32(&f[i]); i += 4;
        c.latitude_micro = static_cast<int32_t>(read_u32(&f[i])); i += 4;
        c.longitude_micro = static_cast<int32_t>(read_u32(&f[i])); i += 4;
        c.lastmod = read_u32(&f[i]); i += 4;
        c.present = true;
        return i == RECORD_BYTES && record_valid(c);
    }

    // CRC covers the header (including count and format) and payload, with
    // the checksum field itself set to zero during calculation.
    static uint32_t database_crc(uint8_t *bytes, size_t length){
        const uint32_t saved = read_u32(bytes + CHECKSUM_OFFSET);
        put_u32(bytes + CHECKSUM_OFFSET, 0);
        const uint32_t crc = sdstore::crc32(bytes, length);
        put_u32(bytes + CHECKSUM_OFFSET, saved);
        return crc;
    }

    static esp_err_t validate_database(uint8_t *bytes, size_t length, size_t &count){
        count = 0;
        if(length < HEADER_BYTES || read_u32(bytes) != DATABASE_MAGIC ||
           read_u32(bytes + 4) != DATABASE_VERSION ||
           read_u32(bytes + 12) != RECORD_BYTES) return ESP_ERR_INVALID_CRC;
        const uint32_t records = read_u32(bytes + 8);
        if(records > Contacts::CAPACITY ||
           length != HEADER_BYTES + static_cast<size_t>(records) * RECORD_BYTES ||
           read_u32(bytes + CHECKSUM_OFFSET) != database_crc(bytes, length))
            return ESP_ERR_INVALID_CRC;
        // Validate every record and reject duplicate full public keys before
        // exposing any data to the live contact table. No extra RAM table.
        for(size_t i = 0; i < records; ++i){
            const uint8_t *record = bytes + HEADER_BYTES + i * RECORD_BYTES;
            Contact item{};
            if(!decode_record(record, item)) return ESP_ERR_INVALID_CRC;
            for(size_t j = 0; j < i; ++j){
                if(memcmp(record + 1, bytes + HEADER_BYTES + j * RECORD_BYTES + 1,
                          PUB_KEY_SIZE) == 0) return ESP_ERR_INVALID_CRC;
            }
        }
        count = records;
        return ESP_OK;
    }

    esp_err_t Contacts::load(){
        const int64_t start_us = esp_timer_get_time();
        auto buffer = allocate_buffer(MAX_DATABASE_BYTES);
        if(!buffer) return ESP_ERR_NO_MEM;
        size_t length = 0;
        const esp_err_t read_err = hardware::sd::read_file(
            DATABASE, buffer.get(), MAX_DATABASE_BYTES, length);
        size_t loaded = 0;
        if(read_err == ESP_OK){
            const esp_err_t err = validate_database(buffer.get(), length, loaded);
            if(err != ESP_OK) return err;
        }else if(read_err != ESP_ERR_NOT_FOUND){
            // A damaged file is not equivalent to an empty address book.
            return read_err;
        }
        // A genuinely missing database starts empty. No legacy scan or import.
        for(auto &contact : contacts_) contact = Contact{};
        for(size_t i = 0; i < loaded; ++i){
            (void)decode_record(buffer.get() + HEADER_BYTES + i * RECORD_BYTES, contacts_[i]);
        }
        dirty_ = false;
        retry_pending_ = false;
        ESP_LOGI(TAG, "Loaded %u contacts from database in %lu ms",
                 static_cast<unsigned>(loaded),
                 static_cast<unsigned long>((esp_timer_get_time() - start_us) / 1000ULL));
        return ESP_OK;
    }

    esp_err_t Contacts::save_database(){
        const size_t records = count();
        const size_t length = HEADER_BYTES + records * RECORD_BYTES;
        auto buffer = allocate_buffer(length);
        if(!buffer) return ESP_ERR_NO_MEM;
        uint8_t *bytes = buffer.get();
        put_u32(bytes, DATABASE_MAGIC);
        put_u32(bytes + 4, DATABASE_VERSION);
        put_u32(bytes + 8, static_cast<uint32_t>(records));
        put_u32(bytes + 12, static_cast<uint32_t>(RECORD_BYTES));
        put_u32(bytes + CHECKSUM_OFFSET, 0);
        size_t offset = HEADER_BYTES;
        for(const auto &contact : contacts_){
            if(!contact.present) continue;
            if(to_wire(bytes + offset, contact) != RECORD_BYTES) return ESP_ERR_INVALID_CRC;
            offset += RECORD_BYTES;
        }
        put_u32(bytes + CHECKSUM_OFFSET, database_crc(bytes, length));
        size_t validated = 0;
        if(validate_database(bytes, length, validated) != ESP_OK || validated != records)
            return ESP_ERR_INVALID_CRC;
        // The existing SD service writes a temporary file, flushes/fsyncs,
        // and rotates the previous main file to .bak before installing it.
        const esp_err_t err = hardware::sd::write_file(DATABASE, bytes, length);
        if(err == ESP_OK){
            dirty_ = false;
            retry_pending_ = false;
        }
        return err;
    }

    esp_err_t Contacts::flush_pending(){
        if(!dirty_) return ESP_OK;
        return save_database();
    }

    void Contacts::flush_due(){
        if(!dirty_) return;
        const uint32_t now = monotonic_ms();
        if(static_cast<uint32_t>(now - dirty_since_ms_) < DEFER_MS) return;
        if(retry_pending_ && static_cast<uint32_t>(now - last_attempt_ms_) < RETRY_MS) return;
        last_attempt_ms_ = now;
        const esp_err_t err = flush_pending();
        if(err != ESP_OK){
            retry_pending_ = true;
            ESP_LOGW(TAG, "Contact database batch save failed: %s", esp_err_to_name(err));
        }
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
        if(value != nullptr && !record_valid(*value)) return false;
        const Contact old = contacts_[idx];
        if(deferred && (value == nullptr || !old.present)) return false;
        contacts_[idx] = value != nullptr ? *value : Contact{};
        if(deferred){
            // The first unsaved change starts the 30-minute batch window;
            // subsequent advertisements update RAM without extending it.
            if(!dirty_){
                dirty_since_ms_ = monotonic_ms();
                retry_pending_ = false;
            }
            dirty_ = true;
            return true;
        }
        // Persist the entire snapshot for explicit edits and new contacts.
        // Any pending advert updates ride along, so no stale queued write
        // can resurrect a deleted contact. Failed edits roll back in RAM.
        const esp_err_t err = save_database();
        if(err != ESP_OK){
            contacts_[idx] = old;
            ESP_LOGE(TAG, "Cannot save contact database: %s", esp_err_to_name(err));
            return false;
        }
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
        // New contacts persist immediately; existing advert changes are
        // coalesced in the live RAM table until the next database save.
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
