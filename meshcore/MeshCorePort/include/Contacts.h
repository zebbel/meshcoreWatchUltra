#pragma once
#include "esp_err.h"

#include <stddef.h>
#include <stdint.h>

#include "Packet.h"
#include "Identity.h"

namespace meshcore {

    struct Contact {
        uint8_t public_key[PUB_KEY_SIZE] = {};
        uint8_t type = 0;
        uint8_t flags = 0;
        uint8_t path_len = 0xFF;
        uint8_t path[MAX_PATH_SIZE] = {};
        char name[32] = {};
        uint32_t advert_time = 0;
        int32_t latitude_micro = 0;
        int32_t longitude_micro = 0;
        uint32_t lastmod = 0;
        bool present = false;
    };

    enum class ContactUpdateResult : uint8_t {
        SAVED, INVALID, FULL, STORAGE_ERROR
    };

    class Contacts {
    public:
        static constexpr size_t CAPACITY = 350;
        static constexpr size_t WIRE_LENGTH = 1 + PUB_KEY_SIZE + 1 + 1 + 1 + MAX_PATH_SIZE + 32 + 4 + 4 + 4 + 4;
        // ------------------------------------------------------------------------
        // Database lifecycle and deferred persistence
        // ------------------------------------------------------------------------

        esp_err_t load();
        // Called only by the MeshCore application task, like all mutations.
        void flush_due();
        esp_err_t flush_pending();
        // ------------------------------------------------------------------------
        // Contact access and updates
        // ------------------------------------------------------------------------

        size_t count() const;
        const Contact *at(size_t index) const;
        const Contact *find(const uint8_t pub[PUB_KEY_SIZE], size_t prefix_len = PUB_KEY_SIZE) const;
        ContactUpdateResult update_from_wire(const uint8_t *wire, size_t length, uint32_t now);
        bool remove(const uint8_t pub[PUB_KEY_SIZE]);
        bool reset_path(const uint8_t pub[PUB_KEY_SIZE]);
        bool observe_advert(const mesh::Identity &id, uint32_t timestamp, const uint8_t *app_data, size_t length, uint32_t now, bool allow_auto_add, uint8_t autoadd_mask);
        size_t to_wire(uint8_t result[WIRE_LENGTH], const Contact &contact) const;
    private:
        Contact contacts_[CAPACITY] = {};
        bool dirty_ = false;
        bool retry_pending_ = false;
        uint32_t dirty_since_ms_ = 0;
        uint32_t last_attempt_ms_ = 0;
        esp_err_t save_database();
        bool save_slot(size_t idx, const Contact *record, bool deferred = false);
        int find_slot(const uint8_t *pub, size_t prefix_len) const;
    };
}
