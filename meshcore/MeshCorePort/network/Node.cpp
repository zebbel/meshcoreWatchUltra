
#include "Node.h"

#include "esp_log.h"

#include <stdio.h>
#include <math.h>
#include <string.h>

#include "SHA256.h"
#include "MeshCoreSD.h"

namespace meshcore {

    static const char *TAG = "Node";
    static constexpr uint8_t PUBLIC_KEY_16[16] = { 0x8B, 0x33, 0x87, 0xE9, 0xC5, 0xCD, 0xEA, 0x6A, 0xC9, 0xE5, 0xED, 0xBA, 0xA1, 0x15, 0xCD, 0x72 };

    Node::Node(mesh::Radio &radio, mesh::MillisecondClock &clock, mesh::RNG &rng, mesh::RTCClock &rtc, mesh::PacketManager &manager, mesh::MeshTables &tables)
        : mesh::Mesh(radio, clock, rng, rtc, manager, tables){
        set_public_channel(channels_[0]);
    }

    void Node::set_group_callback(GroupCallback callback, void *context){
        group_callback_ = callback;
        group_context_ = context;
    }

    void Node::set_public_channel(Channel &channel){
        channel = Channel{};
        memcpy(channel.name, "Public", 7);
        memcpy(channel.value.secret, PUBLIC_KEY_16, 16);
        mesh::Utils::sha256(channel.value.hash, sizeof(channel.value.hash), PUBLIC_KEY_16, 16);
    }

    esp_err_t Node::load_channels(){
        hardware::sd::Session session;
        if(!session.ok()) return session.error();
        esp_err_t err = hardware::sd::ensure_directory("/sdcard/meshcore/channels");
        if(err != ESP_OK) return err;
        struct Saved { char name[32]; uint8_t key[16]; };
        for(int i = 0; i < CHANNEL_COUNT; ++i){
            Saved saved = {};
            char path[100] = {};
            snprintf(path, sizeof(path), "/sdcard/meshcore/channels/ch%d.bin", i);
            err = sdstore::read(path, 0x4D434348U, saved);
            if(err == ESP_ERR_NOT_FOUND) continue;
            if(err != ESP_OK || memchr(saved.name, 0, sizeof(saved.name)) == nullptr) return ESP_ERR_INVALID_CRC;
            memcpy(channels_[i].name, saved.name, sizeof(saved.name));
            memcpy(channels_[i].value.secret, saved.key, sizeof(saved.key));
            mesh::Utils::sha256(channels_[i].value.hash, sizeof(channels_[i].value.hash), saved.key, sizeof(saved.key));
        }
        return ESP_OK;
    }

    bool Node::get_channel(uint8_t index, char name[32], uint8_t secret[16]) const{
        if(index >= CHANNEL_COUNT || name == nullptr || secret == nullptr) return false;
        memset(name, 0, 32);
        memcpy(name, channels_[index].name, 32);
        memcpy(secret, channels_[index].value.secret, 16);
        return true;
    }

    bool Node::set_channel(uint8_t index, const char *name, const uint8_t secret[16]){
        if(index >= CHANNEL_COUNT || name == nullptr || secret == nullptr) return false;
        if(memchr(name, 0, 32) == nullptr) return false;
        bool any = false;
        for(int i = 0; i < 16; i++) any |= secret[i] != 0;
        // An all-zero key with an empty name is the standard clear-channel request.
        if(!any && (index == 0 || name[0] != 0)) return false;
        struct Saved { char name[32]; uint8_t key[16]; } saved = {};
        memcpy(saved.name, name, 32);
        memcpy(saved.key, secret, 16);
        char path[100] = {};
        snprintf(path, sizeof(path), "/sdcard/meshcore/channels/ch%u.bin", static_cast<unsigned>(index));
        esp_err_t err = sdstore::write(path, 0x4D434348U, saved);
        if(err != ESP_OK) return false;
        memcpy(channels_[index].name, saved.name, 32);
        memset(channels_[index].value.secret, 0, 32);
        memcpy(channels_[index].value.secret, secret, 16);
        mesh::Utils::sha256(channels_[index].value.hash, sizeof(channels_[index].value.hash), secret, 16);
        return true;
    }

    bool Node::allowPacketForward(const mesh::Packet *){
        return false; // Companion node, not a repeater.
    }

    float Node::getAirtimeBudgetFactor() const{
        return static_cast<float>(airtime_factor_milli_) / 1000.0f;
    }

    int Node::calcRxDelay(float score, uint32_t air_time) const{
        float base = static_cast<float>(rx_delay_milli_) / 1000.0f;
        if(base <= 1.0f) return 0;
        float factor = powf(base, 0.85f - score) - 1.0f;
        float delay = factor * air_time;
        return delay > 0 ? static_cast<int>(delay) : 0;
    }

    int Node::searchPeersByHash(const uint8_t *hash){
        for(int &i : matching_slots_) i = -1;
        if(contacts_ == nullptr || hash == nullptr) return 0;
        int n = 0;
        for(size_t idx = 0; idx < Contacts::CAPACITY && n < 4; idx++){
            const auto *peer = contacts_->at(idx);
            if(peer != nullptr && peer->public_key[0] == hash[0]) matching_slots_[n++] = idx;
        }
        return n;
    }

    void Node::getPeerSharedSecret(uint8_t *out, int matching_index){
        if(out == nullptr) return;
        memset(out, 0, PUB_KEY_SIZE);
        if(contacts_ == nullptr || matching_index < 0 || matching_index >= 4) return;
        int idx = matching_slots_[matching_index];
        if(idx < 0) return;
        const auto *contact = contacts_->at(idx);
        if(contact == nullptr) return;
        self_id.calcSharedSecret(out, contact->public_key);
    }

    void Node::send_scoped_flood(mesh::Packet *packet, uint32_t delay){
        if(packet == nullptr) return;
        bool scoped = false;
        for(uint8_t b : scope_key_) scoped |= b != 0;
        if(!scoped){
            sendFlood(packet, delay, path_hash_size_);
            return;
        }
        SHA256 mac;
        mac.resetHMAC(scope_key_, sizeof(scope_key_));
        uint8_t type = packet->getPayloadType();
        mac.update(&type, 1);
        mac.update(packet->payload, packet->payload_len);
        uint16_t code = 0;
        mac.finalizeHMAC(scope_key_, sizeof(scope_key_), reinterpret_cast<uint8_t *>(&code), sizeof(code));
        if(code == 0) code = 1;
        if(code == 0xFFFF) code = 0xFFFE;
        uint16_t codes[2] = {code, 0};
        sendFlood(packet, codes, delay, path_hash_size_);
    }

    bool Node::send_private_text(const Contact &contact, uint32_t timestamp, const uint8_t *text, size_t len, uint32_t &ack_hash, uint32_t &timeout_ms){
        if(!contact.present || !text || len == 0 || len > 150 || timestamp == 0 || _mgr->getOutboundTotal() >= 20) return false;
        uint8_t plain[5 + 150] = {};
        memcpy(plain, &timestamp, 4);
        plain[4] = 0; // TXT_TYPE_PLAIN, first attempt
        memcpy(&plain[5], text, len);
        mesh::Utils::sha256(reinterpret_cast<uint8_t *>(&ack_hash), 4, plain, 5 + len, self_id.pub_key, PUB_KEY_SIZE);
        uint8_t secret[PUB_KEY_SIZE] = {};
        self_id.calcSharedSecret(secret, contact.public_key);
        mesh::Identity dest(contact.public_key);
        auto *packet = createDatagram(PAYLOAD_TYPE_TXT_MSG, dest, secret, plain, 5 + len);
        memset(secret, 0, sizeof(secret));
        if(packet == nullptr) return false;
        timeout_ms = 20000 + 4 * _radio->getEstAirtimeFor(packet->getRawLength());
        send_scoped_flood(packet);
        return true;
    }

    void Node::onPeerDataRecv(mesh::Packet *packet, uint8_t type, int sender_idx, const uint8_t *secret, uint8_t *data, size_t len){
        (void)secret;
        if(type != PAYLOAD_TYPE_TXT_MSG || len < 6 || sender_idx < 0 || sender_idx >= 4 || contacts_ == nullptr || matching_slots_[sender_idx] < 0) return;
        const auto *contact = contacts_->at(matching_slots_[sender_idx]);
        if(contact == nullptr) return;
        uint8_t txt_type = data[4] >> 2;
        if(txt_type != 0 && txt_type != 1 && txt_type != 3) return;
        uint32_t timestamp = 0;
        memcpy(&timestamp, data, 4);
        size_t n = len - 5;
        const uint8_t *text = &data[5];
        const uint8_t *end = static_cast<const uint8_t *>(memchr(text, 0, n));
        if(end != nullptr) n = end - text;
        if(peer_callback_ != nullptr){
            peer_callback_(peer_context_, packet, contact->public_key, txt_type, timestamp, text, n);
        }
        // Plain text messages are acknowledged by the actual recipient.
        if(txt_type == 0){
            uint8_t ack[4] = {};
            mesh::Utils::sha256(ack, 4, data, 5 + n, contact->public_key, PUB_KEY_SIZE);
            auto *pkt = createAck(ack, 4);
            if(pkt != nullptr) send_scoped_flood(pkt, 500);
        }
    }

    uint32_t Node::getCADFailMaxDuration() const{
        return 60000; // Retry busy channels for up to a minute.
    }

    int Node::searchChannelsByHash(const uint8_t *hash, mesh::GroupChannel channels[], int max_matches){
        if(hash == nullptr || channels == nullptr || max_matches < 1) return 0;
        int matches = 0;
        for(int i = 0; i < CHANNEL_COUNT && matches < max_matches; i++){
            if(channels_[i].name[0] != 0 && memcmp(hash, channels_[i].value.hash, PATH_HASH_SIZE) == 0){
                channels[matches++] = channels_[i].value;
            }
        }
        return matches;
    }

    bool Node::send_channel_text(uint8_t channel, uint32_t timestamp, const char *name, const uint8_t *text, size_t length){
        if(channel >= CHANNEL_COUNT || channels_[channel].name[0] == 0 || timestamp == 0 || name == nullptr || text == nullptr || length == 0) return false;
        if(_mgr->getOutboundTotal() >= 20) return false;
        size_t name_length = strnlen(name, 32);
        if(name_length == 0 || name_length > 31) return false;
        const size_t prefix = name_length + 2; // "Name: "
        if(length > 160 || prefix + length > 160) return false;

        uint8_t data[5 + 160] = {};
        memcpy(data, &timestamp, 4);
        data[4] = 0; // TXT_TYPE_PLAIN
        memcpy(&data[5], name, name_length);
        data[5 + name_length] = ':';
        data[5 + name_length + 1] = ' ';
        memcpy(&data[5 + prefix], text, length);
        mesh::Packet *packet = createGroupDatagram(PAYLOAD_TYPE_GRP_TXT, channels_[channel].value, data, 5 + prefix + length);
        if(packet == nullptr) return false;
        send_scoped_flood(packet);
        return true;
    }

    bool Node::send_self_advert(const char *name, bool flood, int32_t lat_micro, int32_t lon_micro, bool with_location){
        if(name == nullptr) return false;
        size_t length = strnlen(name, MAX_ADVERT_DATA_SIZE);
        const size_t overhead = with_location ? 9 : 1;
        if(length == 0) return false;
        if(length > MAX_ADVERT_DATA_SIZE - overhead){
            length = MAX_ADVERT_DATA_SIZE - overhead;
            // Do not leave a truncated UTF-8 tail in an advertised name.
            while(length > 0 && (static_cast<uint8_t>(name[length]) & 0xC0) == 0x80) length--;
            if(length == 0) return false;
        }
        uint8_t data[MAX_ADVERT_DATA_SIZE] = {};
        data[0] = with_location ? 0x91 : 0x81; // Chat, name, optional lat/lon
        if(with_location){
            memcpy(&data[1], &lat_micro, 4);
            memcpy(&data[5], &lon_micro, 4);
        }
        memcpy(&data[overhead], name, length);
        mesh::Packet *packet = createAdvert(self_id, data, length + overhead);
        if(packet == nullptr) return false;
        if(flood) send_scoped_flood(packet);
        else sendZeroHop(packet);
        return true;
    }

    void Node::onGroupDataRecv(mesh::Packet *packet, uint8_t type, const mesh::GroupChannel &channel, uint8_t *data, size_t length){
        int index = -1;
        for(int i = 0; i < CHANNEL_COUNT; i++){
            if(channels_[i].name[0] != 0 && memcmp(channel.secret, channels_[i].value.secret, sizeof(channel.secret)) == 0){
                index = i;
                break;
            }
        }
        if(index < 0) return;
        if(type != PAYLOAD_TYPE_GRP_TXT || length < 6 || data[4] != 0) return;
        uint32_t timestamp = 0;
        memcpy(&timestamp, data, 4);
        const uint8_t *text = &data[5];
        size_t text_length = length - 5;
        const uint8_t *end = static_cast<const uint8_t *>(memchr(text, 0, text_length));
        if(end != nullptr) text_length = end - text;
        if(text_length > 160) text_length = 160;
        if(group_callback_ != nullptr){
            group_callback_(group_context_, packet, static_cast<uint8_t>(index), timestamp, text, text_length);
        }
    }

    void Node::logRx(mesh::Packet *packet, int length, float score){
        packets_received_++;
    }

    void Node::logRxRaw(float snr, float rssi, const uint8_t raw[], int length){
        if(raw_rx_callback_ == nullptr)
            return;

        if(raw == nullptr || length <= 0 || length > MAX_TRANS_UNIT)
            return;

        // Forward the original received LoRa bytes.
        // Do not modify routing or packet processing.
        raw_rx_callback_(raw_rx_context_, snr, rssi, raw, static_cast<size_t>(length));
    }

    void Node::cache_advert_path(const mesh::Packet *packet, const mesh::Identity &identity){
        if(packet == nullptr || packet->path_len > 255) return;

        const uint8_t encoded_length = static_cast<uint8_t>(packet->path_len);
        if(!mesh::Packet::isValidPathLen(encoded_length)) return;

        const size_t path_bytes = packet->getPathByteLen();
        if(path_bytes > MAX_PATH_SIZE) return;

        const uint32_t now = static_cast<uint32_t>(_ms->getMillis());

        AdvertPath *slot = nullptr;
        AdvertPath *oldest = nullptr;
        uint32_t oldest_age = 0;

        for(auto &entry : advert_paths_){
            if(entry.valid && static_cast<uint32_t>(now - entry.seen_ms) >= ADVERT_PATH_MAX_AGE_MS) entry.valid = false;

            if(entry.valid && memcmp(entry.public_key, identity.pub_key, PUB_KEY_SIZE) == 0){
                slot = &entry;
                break;
            }

            if(!entry.valid){
                if(slot == nullptr) slot = &entry;
                continue;
            }

            const uint32_t age = static_cast<uint32_t>(now - entry.seen_ms);

            if(oldest == nullptr || age > oldest_age){
                oldest = &entry;
                oldest_age = age;
            }
        }

        if(slot == nullptr) slot = oldest;
        if(slot == nullptr) return;

        memcpy(slot->public_key, identity.pub_key, PUB_KEY_SIZE);
        memset(slot->path, 0, sizeof(slot->path));

        if(path_bytes > 0) memcpy(slot->path, packet->path, path_bytes);

        slot->path_len = encoded_length;
        slot->received_at = getRTCClock()->getCurrentTime();
        slot->seen_ms = now;
        slot->valid = true;

        ESP_LOGD(TAG, "Cached advertisement path: %02X%02X%02X%02X, hops=%u", identity.pub_key[0], identity.pub_key[1], identity.pub_key[2], identity.pub_key[3], packet->getPathHashCount());
    }

    bool Node::get_advert_path(const uint8_t public_key[PUB_KEY_SIZE], uint32_t &received_at, uint8_t &path_len, uint8_t path[MAX_PATH_SIZE]) const{
        if(public_key == nullptr || path == nullptr) return false;

        const uint32_t now = static_cast<uint32_t>(_ms->getMillis());

        for(const auto &entry : advert_paths_){
            if(!entry.valid) continue;
            if(memcmp(entry.public_key, public_key, PUB_KEY_SIZE) != 0) continue;
            if(static_cast<uint32_t>(now - entry.seen_ms) >= ADVERT_PATH_MAX_AGE_MS) return false;
            if(!mesh::Packet::isValidPathLen(entry.path_len)) return false;

            received_at = entry.received_at;
            path_len = entry.path_len;
            memcpy(path, entry.path, MAX_PATH_SIZE);
            return true;
        }

        return false;
    }
   
    void Node::onAdvertRecv(mesh::Packet *packet, const mesh::Identity &identity, uint32_t timestamp, const uint8_t *data, size_t length){
        valid_adverts_++;

        cache_advert_path(packet, identity);

        if(advert_callback_ != nullptr) advert_callback_(advert_context_, identity, timestamp, data, length);
    }

    void Node::onAckRecv(mesh::Packet *, uint32_t ack){
        acks_received_++;
        if(ack_callback_ != nullptr) ack_callback_(ack_context_, ack);
    }

}
