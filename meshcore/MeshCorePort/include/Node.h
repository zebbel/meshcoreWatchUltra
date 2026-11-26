#pragma once

#include "esp_err.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "Contacts.h"

#include "Mesh.h"

namespace meshcore {

    class Node final : public mesh::Mesh {
    public:
        using GroupCallback = void (*)(void *ctx, const mesh::Packet *packet, uint8_t channel_index, uint32_t timestamp, const uint8_t *text, size_t length);
        using PeerCallback = void (*)(void *, const mesh::Packet *, const uint8_t pub[32], uint8_t text_type, uint32_t timestamp, const uint8_t *text, size_t length);
        using AdvertCallback = void (*)(void *, const mesh::Identity &, uint32_t timestamp, const uint8_t *, size_t);
        using AckCallback = void (*)(void *, uint32_t);
        using RawRxCallback = void (*)(void *ctx, float snr, float rssi, const uint8_t *raw, size_t length);
        static constexpr int CHANNEL_COUNT = 40;

        Node(mesh::Radio &radio, mesh::MillisecondClock &clock, mesh::RNG &rng, mesh::RTCClock &rtc, mesh::PacketManager &manager, mesh::MeshTables &tables);

        void set_group_callback(GroupCallback callback, void *context);
        void set_contacts(Contacts *contacts){ contacts_ = contacts; }
        void set_peer_callback(PeerCallback fn, void *ctx){ peer_callback_ = fn; peer_context_ = ctx; }
        void set_advert_callback(AdvertCallback fn, void *ctx){ advert_callback_ = fn; advert_context_ = ctx; }
        void set_ack_callback(AckCallback fn, void *ctx){ ack_callback_ = fn; ack_context_ = ctx; }
        void set_raw_rx_callback(RawRxCallback fn, void *ctx){
            raw_rx_callback_ = fn;
            raw_rx_context_ = ctx;
        }
        void set_tuning(uint32_t rx_milli, uint32_t airtime_milli){
            rx_delay_milli_ = rx_milli; airtime_factor_milli_ = airtime_milli;
        }
        void set_path_hash_mode(uint8_t mode){ path_hash_size_ = mode + 1; }
        void set_scope_key(const uint8_t key[16]){
            if(key != nullptr) memcpy(scope_key_, key, sizeof(scope_key_));
            else memset(scope_key_, 0, sizeof(scope_key_));
        }
        int pendingOutboundCount() const { return _mgr->getOutboundTotal(); }
        uint16_t errorFlags() const { return _err_flags; }
        bool send_private_text(const Contact &contact, uint32_t timestamp, const uint8_t *text, size_t len, uint32_t &ack_hash, uint32_t &timeout_ms);
        bool send_channel_text(uint8_t channel, uint32_t timestamp, const char *name, const uint8_t *text, size_t length);
        esp_err_t load_channels();
        bool set_channel(uint8_t index, const char *name, const uint8_t secret[16]);
        bool get_channel(uint8_t index, char name[32], uint8_t secret[16]) const;
        bool send_self_advert(const char *name, bool flood, int32_t lat_micro = 0, int32_t lon_micro = 0, bool with_location = false);
        uint32_t packetCount() const { return packets_received_; }
        uint32_t validAdvertCount() const { return valid_adverts_; }
        uint32_t ackCount() const { return acks_received_; }
        bool get_advert_path(const uint8_t public_key[PUB_KEY_SIZE], uint32_t &received_at, uint8_t &path_len, uint8_t path[MAX_PATH_SIZE]) const;

    protected:
        bool allowPacketForward(const mesh::Packet *packet) override;
        float getAirtimeBudgetFactor() const override;
        int calcRxDelay(float score, uint32_t airtime) const override;
        int searchPeersByHash(const uint8_t *hash) override;
        void getPeerSharedSecret(uint8_t *out, int matching_index) override;
        void onPeerDataRecv(mesh::Packet *packet, uint8_t type, int sender_idx, const uint8_t *secret, uint8_t *data, size_t len) override;
        uint32_t getCADFailMaxDuration() const override;
        void logRx(mesh::Packet *packet, int length, float score) override;
        void logRxRaw(float snr, float rssi, const uint8_t raw[], int length) override;

        int searchChannelsByHash(const uint8_t *hash, mesh::GroupChannel channels[], int max_matches) override;
        void onGroupDataRecv(mesh::Packet *packet, uint8_t type, const mesh::GroupChannel &channel, uint8_t *data, size_t length) override;
        void onAdvertRecv(mesh::Packet *packet, const mesh::Identity &identity, uint32_t timestamp, const uint8_t *app_data, size_t app_data_length) override;
        void onAckRecv(mesh::Packet *packet, uint32_t ack_crc) override;

    private:
        struct Channel {
            char name[32] = {};
            mesh::GroupChannel value = {};
        };
        Channel channels_[CHANNEL_COUNT] = {};

        static constexpr size_t ADVERT_PATH_CACHE_SIZE = 64;
        static constexpr uint32_t ADVERT_PATH_MAX_AGE_MS = 24U * 60U * 60U * 1000U;

        struct AdvertPath {
            uint8_t public_key[PUB_KEY_SIZE] = {};
            uint8_t path[MAX_PATH_SIZE] = {};
            uint8_t path_len = 0;
            uint32_t received_at = 0;
            uint32_t seen_ms = 0;
            bool valid = false;
        };

        AdvertPath advert_paths_[ADVERT_PATH_CACHE_SIZE] = {};

        void cache_advert_path(const mesh::Packet *packet, const mesh::Identity &identity);

        static void set_public_channel(Channel &channel);
        GroupCallback group_callback_ = nullptr;
        void *group_context_ = nullptr;
        uint32_t packets_received_ = 0;
        uint32_t valid_adverts_ = 0;
        uint32_t acks_received_ = 0;
        Contacts *contacts_ = nullptr;
        int matching_slots_[4] = {-1, -1, -1, -1};
        PeerCallback peer_callback_ = nullptr;
        void *peer_context_ = nullptr;
        AdvertCallback advert_callback_ = nullptr;
        void *advert_context_ = nullptr;
        AckCallback ack_callback_ = nullptr;
        void *ack_context_ = nullptr;
        RawRxCallback raw_rx_callback_ = nullptr;
        void *raw_rx_context_ = nullptr;
        uint32_t rx_delay_milli_ = 1000;
        uint32_t airtime_factor_milli_ = 9000;
        uint8_t path_hash_size_ = 1;
        uint8_t scope_key_[16] = {};
        void send_scoped_flood(mesh::Packet *packet, uint32_t delay = 0);
    };

}
