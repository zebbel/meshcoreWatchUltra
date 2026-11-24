
#include "Companion.h"
#include "GPS.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "HardwareSystem.h"

#include "MeshCoreSD.h"
#include "SDStorage.h"

namespace meshcore {

    static const char *TAG = "Companion";

    enum : uint8_t {
        CMD_APP_START=1, CMD_SEND_TXT=2, CMD_SEND_CHANNEL_TXT=3,
        CMD_GET_CONTACTS=4, CMD_GET_TIME=5, CMD_SET_TIME=6,
        CMD_SEND_ADVERT=7, CMD_SET_NAME=8, CMD_ADD_CONTACT=9,
        CMD_SYNC_MESSAGE=10, CMD_SET_RADIO=11, CMD_SET_POWER=12,
        CMD_RESET_PATH=13, CMD_SET_LOCATION=14, CMD_REMOVE_CONTACT=15,
        CMD_SHARE_CONTACT=16, CMD_EXPORT_CONTACT=17, CMD_IMPORT_CONTACT=18,
        CMD_REBOOT=19, CMD_GET_BATT=20, CMD_SET_TUNING=21,
        CMD_DEVICE_QUERY=22, CMD_GET_CONTACT_KEY=30, CMD_GET_CHANNEL=31,
        CMD_SET_CHANNEL=32, CMD_SET_BLE_PIN=37, CMD_SET_OTHER=38,
        CMD_SEND_TELEMETRY_REQ=39,
        CMD_GET_CUSTOM_VARS=40,
        CMD_SET_CUSTOM_VAR=41,
        CMD_GET_ADVERT_PATH=42,
        CMD_GET_TUNING=43, CMD_GET_STATS=56, CMD_SET_AUTOADD=58,
        CMD_GET_AUTOADD=59, CMD_GET_REPEAT_FREQ=60, CMD_SET_HASH_MODE=61,
        CMD_SET_TEMP_SCOPE=54, CMD_SET_DEFAULT_SCOPE=63,
        CMD_GET_DEFAULT_SCOPE=64
    };

    enum : uint8_t {
        RESP_OK=0, RESP_ERROR=1, RESP_CONTACTS_START=2,
        RESP_CONTACT=3, RESP_CONTACTS_END=4, RESP_SELF=5,
        RESP_SENT=6, RESP_PEER_V3=16, RESP_GROUP_V3=17,
        RESP_PEER_OLD=7, RESP_GROUP_OLD=8, RESP_CURR_TIME=9,
        RESP_NO_MORE=10, RESP_EXPORT_CONTACT=11, RESP_BATT=12,
        RESP_DEVICE=13, RESP_CHANNEL=18,
        RESP_CUSTOM_VARS=21,
        RESP_ADVERT_PATH=22,
        RESP_TUNING=23,
        RESP_STATS=24, RESP_AUTOADD=25, RESP_REPEAT_FREQ=26,
        RESP_DEFAULT_SCOPE=28,  
        PUSH_ADVERT=0x80,
        PUSH_ACK=0x82,
        PUSH_MSGS=0x83,
        PUSH_LOG_RX=0x88,
        PUSH_NEW_ADVERT=0x8A,
        PUSH_TELEMETRY=0x8B
    };

    enum : uint8_t {
        ERR_UNSUPPORTED=1, ERR_NOT_FOUND=2, ERR_FULL=3,
        ERR_BAD_STATE=4, ERR_IO=5, ERR_ILLEGAL=6
    };

    static void put_u32(uint8_t *p, uint32_t v){
        memcpy(p, &v, 4);
    }

    static uint32_t get_u32(const uint8_t *p){
        uint32_t v = 0;
        memcpy(&v, p, 4);
        return v;
    }

    static void put_u16(uint8_t *p, uint16_t v){
        memcpy(p, &v, 2);
    }

    static uint32_t time_ms(mesh::MillisecondClock &clock){
        return static_cast<uint32_t>(clock.getMillis());
    }

    Companion::Companion(hardware::bluetooth::Transport &ble, Node &mesh, RadioAdapter &radio, SettingsStore &settings, Contacts &contacts, RTCAdapter &rtc, mesh::MillisecondClock &clock, Telemetry &telemetry)
        : ble_(ble), mesh_(mesh), radio_(radio),
          settings_(settings), contacts_(contacts),
          rtc_(rtc), clock_(clock), telemetry_(telemetry){

        snprintf(node_name_, sizeof(node_name_), "Watch-%02X%02X%02X%02X", mesh_.self_id.pub_key[0], mesh_.self_id.pub_key[1], mesh_.self_id.pub_key[2], mesh_.self_id.pub_key[3]);

        struct NameRecord { char name[32]; } saved_name = {};
        const esp_err_t name_result = sdstore::read("/sdcard/meshcore/node.bin", 0x4D434E4DU, saved_name);
        if(name_result == ESP_OK && memchr(saved_name.name, 0, sizeof(saved_name.name)) != nullptr){
            snprintf(node_name_, sizeof(node_name_), "%s", saved_name.name);
        }else if(name_result == ESP_ERR_NOT_FOUND){
            // First boot: persist the generated name after identity initialization.
            NameRecord initial = {};
            snprintf(initial.name, sizeof(initial.name), "%s", node_name_);
            if(sdstore::write("/sdcard/meshcore/node.bin", 0x4D434E4DU, initial) != ESP_OK){
                storage_ready_ = false;
                ESP_LOGW(TAG, "Unable to save initial companion node name");
            }
        }else{
            storage_ready_ = false;
            ESP_LOGE(TAG, "Invalid or unreadable companion name: %s", esp_err_to_name(name_result));
        }

        mesh_.set_contacts(&contacts_);
        mesh_.set_group_callback(group_callback, this);
        mesh_.set_peer_callback(peer_callback, this);
        mesh_.set_advert_callback(advert_callback, this);
        mesh_.set_ack_callback(ack_callback, this);
        mesh_.set_raw_rx_callback(raw_rx_callback, this);
    }

    void Companion::group_callback(void *ctx, const mesh::Packet *p, uint8_t ch, uint32_t ts, const uint8_t *txt, size_t n){

        static_cast<Companion *>(ctx)-> on_group_message(p, ch, ts, txt, n);
    }

    void Companion::peer_callback(void *ctx, const mesh::Packet *p, const uint8_t pub[32], uint8_t type, uint32_t ts, const uint8_t *txt, size_t n){

        static_cast<Companion *>(ctx)-> on_peer_message(p, pub, type, ts, txt, n);
    }

    void Companion::advert_callback(void *ctx, const mesh::Identity &id, uint32_t ts, const uint8_t *data, size_t n){

        static_cast<Companion *>(ctx)-> on_advert(id, ts, data, n);
    }

    void Companion::ack_callback(void *ctx, uint32_t crc){
        static_cast<Companion *>(ctx)->on_ack(crc);
    }

    void Companion::raw_rx_callback(void *ctx, float snr, float rssi, const uint8_t *raw, size_t length){
        if(ctx == nullptr)
            return;

        static_cast<Companion *>(ctx)->on_raw_rx(snr, rssi, raw, length);
    }

    void Companion::enqueue(const uint8_t *data, size_t len){
        if(data == nullptr || len == 0 || len > hardware::bluetooth::Transport::MAX_FRAME) return;

        if(message_count_ >= MESSAGE_QUEUE){
            ESP_LOGW(TAG, "RAM inbox full; new message not queued");
            return;
        }

        auto &f = messages_[(message_head_ + message_count_) % MESSAGE_QUEUE];

        memcpy(f.data, data, len);
        f.length = len;
        message_count_++;

        const uint8_t push = PUSH_MSGS;
        ble_.send(&push, 1);
    }

    void Companion::on_group_message(const mesh::Packet *pkt, uint8_t ch, uint32_t ts, const uint8_t *text, size_t n){

        uint8_t f[hardware::bluetooth::Transport::MAX_FRAME] = {};
        size_t i = 0;

        if(target_version_ >= 3){
            f[i++] = RESP_GROUP_V3;
            f[i++] = static_cast<uint8_t>(static_cast<int8_t>(pkt->getSNR() * 4));
            f[i++] = 0;
            f[i++] = 0;
        }else{
            f[i++] = RESP_GROUP_OLD;
        }

        f[i++] = ch;
        f[i++] = pkt->isRouteFlood() ? pkt->path_len : 0xFF;
        f[i++] = 0;

        put_u32(&f[i], ts);
        i += 4;

        if(n > sizeof(f) - i)
            n = sizeof(f) - i;

        memcpy(&f[i], text, n);
        i += n;

        enqueue(f, i);
    }

    void Companion::on_peer_message(const mesh::Packet *pkt, const uint8_t pub[32], uint8_t type, uint32_t ts, const uint8_t *text, size_t n){

        uint8_t f[hardware::bluetooth::Transport::MAX_FRAME] = {};
        size_t i = 0;

        if(target_version_ >= 3){
            f[i++] = RESP_PEER_V3;
            f[i++] = static_cast<uint8_t>(static_cast<int8_t>(pkt->getSNR() * 4));
            f[i++] = 0;
            f[i++] = 0;
        }else{
            f[i++] = RESP_PEER_OLD;
        }

        memcpy(&f[i], pub, 6);
        i += 6;

        f[i++] = pkt->isRouteFlood() ? pkt->path_len : 0xFF;
        f[i++] = type;

        put_u32(&f[i], ts);
        i += 4;

        if(n > sizeof(f) - i)
            n = sizeof(f) - i;

        memcpy(&f[i], text, n);
        i += n;

        enqueue(f, i);
    }

    void Companion::on_advert(const mesh::Identity &id, uint32_t ts, const uint8_t *data, size_t n){

        const auto *previous = contacts_.find(id.pub_key);
        bool is_new = previous == nullptr;

        const auto &cfg = settings_.get();
        bool manual = (cfg.manual_contact_mode & 1) != 0;
        bool autoadd = !manual || (cfg.autoadd_config & 0x1e) != 0;

        uint8_t mask = manual ? cfg.autoadd_config : 0;

        if(!contacts_.observe_advert(id, ts, data, n, rtc_.getCurrentTime(), autoadd, mask)){
            return;
        }

        const auto *contact = contacts_.find(id.pub_key);
        if(contact == nullptr)
            return;

        if(is_new){
            send_contact(*contact, PUSH_NEW_ADVERT);
        }else{
            uint8_t f[1 + PUB_KEY_SIZE] = {PUSH_ADVERT};
            memcpy(&f[1], id.pub_key, PUB_KEY_SIZE);
            ble_.send(f, sizeof(f));
        }
    }

    void Companion::on_ack(uint32_t crc){
        for(auto &a : pending_acks_){
            if(a.start_ms != 0 && a.crc == crc){
                uint8_t f[9] = {PUSH_ACK};

                put_u32(&f[1], crc);
                put_u32(&f[5], time_ms(clock_) - a.start_ms);

                ble_.send(f, sizeof(f));
                a = PendingAck{};
                break;
            }
        }
    }

    void Companion::on_raw_rx(float snr, float rssi, const uint8_t *raw, size_t length){
        if(raw == nullptr || length == 0)
            return;

        // MeshCore raw RX notification format:
        //
        // Byte 0: 0x88
        // Byte 1: SNR multiplied by 4 (signed int8)
        // Byte 2: RSSI in dBm (signed int8)
        // Byte 3+: Original LoRa packet

        constexpr size_t HEADER_SIZE = 3;

        if(length > hardware::bluetooth::Transport::MAX_FRAME - HEADER_SIZE)
            return;

        // Use only valid BLE signal byte ranges.
        const float snr_scaled = snr * 4.0f;

        const int snr_value = snr_scaled < -128.0f ? -128 :
            snr_scaled > 127.0f ? 127 :
            static_cast<int>(snr_scaled);

        const int rssi_value = rssi < -128.0f ? -128 :
            rssi > 127.0f ? 127 :
            static_cast<int>(rssi);

        uint8_t frame[hardware::bluetooth::Transport::MAX_FRAME] = {};

        frame[0] = PUSH_LOG_RX;

        frame[1] = static_cast<uint8_t>(static_cast<int8_t>(snr_value));

        frame[2] = static_cast<uint8_t>(static_cast<int8_t>(rssi_value));

        memcpy(&frame[HEADER_SIZE], raw, length);

        const size_t frame_length = HEADER_SIZE + length;

        // Best-effort only:
        // never interfere with normal companion traffic.
        ble_.send_best_effort(frame, frame_length);
    }

    void Companion::reply_error(uint8_t code, const uint8_t *cmd, size_t len){
        ESP_LOGD(TAG, "BLE command=%u length=%u", cmd[0], static_cast<unsigned>(len));
        ESP_LOGW(TAG, "Companion command rejected: error=%u", static_cast<unsigned>(code));

        const uint8_t f[2] = {RESP_ERROR, code};
        ble_.send(f, 2);
    }

    void Companion::reply_ok(){
        const uint8_t f[1] = {RESP_OK};
        ble_.send(f, 1);
    }

    bool Companion::save_name(){
        struct NameRecord { char name[32]; } saved = {};
        snprintf(saved.name, sizeof(saved.name), "%s", node_name_);
        return sdstore::write("/sdcard/meshcore/node.bin", 0x4D434E4DU, saved) == ESP_OK;
    }

    bool Companion::update_settings(const Settings &next, bool radio_change){

        if(!next.valid()){
            ESP_LOGW(TAG, "Invalid settings: freq=%lu kHz BW=%lu Hz SF=%u CR=%u TX=%d dBm", static_cast<unsigned long>(next.freq_khz), static_cast<unsigned long>(next.bandwidth_hz), next.sf, next.cr, next.tx_power_dbm);
            return false;
        }

        const Settings before = settings_.get();

        const bool radio_changed = radio_change && (next.freq_khz != before.freq_khz || next.bandwidth_hz != before.bandwidth_hz || next.sf != before.sf || next.cr != before.cr || next.tx_power_dbm != before.tx_power_dbm);

        if(radio_change && !radio_changed)
            return true;

        if(radio_changed && mesh_.pendingOutboundCount() != 0){
            ESP_LOGW(TAG, "Cannot change active radio while outbound packets are queued");
            return false;
        }

        if(radio_changed && !radio_.applyConfig(next.radio())){
            ESP_LOGW(TAG, "Radio driver rejected requested configuration");
            return false;
        }

        if(settings_.save(next) == ESP_OK){
            mesh_.set_tuning(next.rx_delay_milli, next.airtime_factor_milli);
            mesh_.set_path_hash_mode(next.path_hash_mode);
            mesh_.set_scope_key(next.default_scope_key);
            return true;
        }

        ESP_LOGE(TAG, "Unable to persist settings to SD Card");

        if(radio_changed && !radio_.applyConfig(before.radio())){
            ESP_LOGE(TAG, "Failed restoring radio after SD Card error");
        }

        return false;
    }

    void Companion::handle_app_start(){
        const auto &s = settings_.get();

        uint8_t f[hardware::bluetooth::Transport::MAX_FRAME] = {};
        size_t i = 0;

        f[i++] = RESP_SELF;
        f[i++] = 1;

        f[i++] = static_cast<uint8_t>(s.tx_power_dbm);
        f[i++] = 14;

        memcpy(&f[i], mesh_.self_id.pub_key, PUB_KEY_SIZE);
        i += PUB_KEY_SIZE;

        put_u32(&f[i], static_cast<uint32_t>(s.latitude_micro));
        i += 4;

        put_u32(&f[i], static_cast<uint32_t>(s.longitude_micro));
        i += 4;

        f[i++] = s.multi_acks;
        f[i++] = s.advert_loc_policy;
        f[i++] = s.telemetry_modes;
        f[i++] = s.manual_contact_mode;

        put_u32(&f[i], s.freq_khz);
        i += 4;

        put_u32(&f[i], s.bandwidth_hz);
        i += 4;

        f[i++] = s.sf;
        f[i++] = s.cr;

        const size_t n = strnlen(node_name_, sizeof(node_name_));

        memcpy(&f[i], node_name_, n);
        i += n;

        ble_.send(f, i);
    }

    void Companion::handle_device_query(){
        uint8_t f[96] = {};
        size_t i = 0;

        f[i++] = RESP_DEVICE;
        f[i++] = 14;
        f[i++] = Contacts::CAPACITY / 2;
        f[i++] = Node::CHANNEL_COUNT;

        put_u32(&f[i], ble_.pairing_pin());
        i += 4;

        snprintf(reinterpret_cast<char *>(&f[i]), 12, "%s", "2026-11-20");
        i += 12;

        snprintf(reinterpret_cast<char *>(&f[i]), 40, "%s", "LilyGO T-Watch Ultra");
        i += 40;

        snprintf(reinterpret_cast<char *>(&f[i]), 20, "%s", "watch-ble-m6");
        i += 20;

        f[i++] = 0;
        f[i++] = settings_.get().path_hash_mode;

        ble_.send(f, i);
    }

    void Companion::handle_get_channel(uint8_t index){
        char name[32] = {};
        uint8_t secret[16] = {};

        if(!mesh_.get_channel(index, name, secret)){
            return;
        }

        uint8_t f[50] = {RESP_CHANNEL, index};

        memcpy(&f[2], name, 32);
        memcpy(&f[34], secret, 16);

        ble_.send(f, sizeof(f));
    }

    bool Companion::send_contact(const Contact &c, uint8_t code){

        uint8_t f[Contacts::WIRE_LENGTH] = {};
        size_t n = contacts_.to_wire(f, c);

        if(n == 0)
            return false;

        f[0] = code;
        return ble_.send(f, n);
    }

    void Companion::pump_contacts(){
        if(!contact_sync_)
            return;

        for(;
            contact_index_ < Contacts::CAPACITY;
            contact_index_++){

            const Contact *peer = contacts_.at(contact_index_);

            if(peer == nullptr || peer->lastmod <= contact_since_){
                continue;
            }

            if(!send_contact(*peer))
                return;

            if(peer->lastmod > last_mod_)
                last_mod_ = peer->lastmod;

            contact_index_++;
            return;
        }

        uint8_t f[5] = {RESP_CONTACTS_END};
        put_u32(&f[1], last_mod_);

        if(ble_.send(f, sizeof(f)))
            contact_sync_ = false;
    }

    void Companion::handle(const uint8_t *cmd, size_t len){

        if(cmd == nullptr || len == 0)
            return;

        const auto &before = settings_.get();

        switch(cmd[0]){

            case CMD_APP_START:
                if(len < 8){
                    reply_error(ERR_ILLEGAL , cmd, len);
                    return;
                }
                contact_sync_ = false;
                handle_app_start();
                return;

            case CMD_DEVICE_QUERY:
                if(len < 2){
                    reply_error(ERR_ILLEGAL , cmd, len);
                    return;
                }
                target_version_ = cmd[1];
                handle_device_query();
                return;

            case CMD_GET_CONTACTS: {
                if(contact_sync_){
                    reply_error(ERR_BAD_STATE, cmd, len);
                    return;
                }

                contact_since_ = len >= 5 ? get_u32(&cmd[1]) : 0;

                contact_index_ = 0;
                last_mod_ = 0;

                uint8_t f[5] = {RESP_CONTACTS_START};
                put_u32(&f[1], contacts_.count());

                if(!ble_.send(f, sizeof(f))){
                    reply_error(ERR_FULL, cmd, len);
                    return;
                }

                contact_sync_ = true;
                return;
            }

            case CMD_GET_CONTACT_KEY:
                if(len != 1 + PUB_KEY_SIZE){
                    reply_error(ERR_ILLEGAL , cmd, len);
                    return;
                }

                if(auto *c = contacts_.find(&cmd[1]))
                    send_contact(*c);
                else
                    reply_error(ERR_NOT_FOUND , cmd, len);
                return;

            case CMD_ADD_CONTACT: {
                switch(contacts_.update_from_wire(cmd, len, rtc_.getCurrentTime())){

                    case ContactUpdateResult::SAVED:
                        reply_ok();
                        return;
                    case ContactUpdateResult::INVALID:
                        reply_error(ERR_ILLEGAL , cmd, len);
                        return;
                    case ContactUpdateResult::FULL:
                        reply_error(ERR_FULL, cmd, len);
                        return;
                    case ContactUpdateResult::STORAGE_ERROR:
                        reply_error(ERR_IO, cmd, len);
                        return;
                }

                reply_error(ERR_IO, cmd, len);
                return;
            }

            case CMD_IMPORT_CONTACT: {
                if(len < 1 + 2 + PUB_KEY_SIZE + 4 + SIGNATURE_SIZE + 1){
                    reply_error(ERR_ILLEGAL , cmd, len);
                    return;
                }

                mesh::Packet packet;

                if(!packet.readFrom(&cmd[1], static_cast<uint8_t>(len - 1)) || packet.getPayloadType() != PAYLOAD_TYPE_ADVERT || packet.payload_len < PUB_KEY_SIZE + 4 + SIGNATURE_SIZE + 1){

                    reply_error(ERR_ILLEGAL , cmd, len);
                    return;
                }

                const uint8_t *payload = packet.payload;

                const size_t header_len = PUB_KEY_SIZE + 4 + SIGNATURE_SIZE;

                const size_t advert_len = packet.payload_len - header_len;

                if(advert_len > MAX_ADVERT_DATA_SIZE){
                    reply_error(ERR_ILLEGAL , cmd, len);
                    return;
                }

                mesh::Identity sender(payload);

                if(sender.matches(mesh_.self_id)){
                    reply_error(ERR_ILLEGAL , cmd, len);
                    return;
                }

                uint8_t signed_data[PUB_KEY_SIZE + 4 + MAX_ADVERT_DATA_SIZE] = {};

                memcpy(signed_data, payload, PUB_KEY_SIZE + 4);

                memcpy(signed_data + PUB_KEY_SIZE + 4, payload + header_len, advert_len);

                if(!sender.verify(payload + PUB_KEY_SIZE + 4, signed_data, static_cast<int>(PUB_KEY_SIZE + 4 + advert_len))){

                    ESP_LOGW(TAG, "Rejected unsigned or forged contact advert");
                    reply_error(ERR_ILLEGAL , cmd, len);
                    return;
                }

                const uint32_t timestamp = get_u32(payload + PUB_KEY_SIZE);

                if(!contacts_.observe_advert(sender, timestamp, payload + header_len, advert_len, rtc_.getCurrentTime(), true, 0)){

                    reply_error(contacts_.count() == Contacts::CAPACITY ? ERR_FULL : ERR_ILLEGAL, cmd, len);
                    return;
                }

                reply_ok();
                return;
            }

            case CMD_EXPORT_CONTACT: {
                if(len != 1){
                    reply_error(ERR_UNSUPPORTED, cmd, len);
                    return;
                }

                if(rtc_.getCurrentTime() == 0){
                    reply_error(ERR_BAD_STATE, cmd, len);
                    return;
                }

                size_t name_len = strnlen(node_name_, sizeof(node_name_));

                const bool location = before.advert_loc_policy != 0;

                const size_t offset = location ? 9 : 1;

                if(name_len > MAX_ADVERT_DATA_SIZE - offset){
                    name_len = MAX_ADVERT_DATA_SIZE - offset;

                    while(name_len > 0 && (static_cast<uint8_t>(node_name_[name_len]) & 0xC0) == 0x80){
                        name_len--;
                    }
                }

                if(name_len == 0){
                    reply_error(ERR_ILLEGAL , cmd, len);
                    return;
                }

                uint8_t app_data[MAX_ADVERT_DATA_SIZE] = {};
                app_data[0] = location ? 0x91 : 0x81;

                if(location){
                    put_u32(&app_data[1], static_cast<uint32_t>(before.latitude_micro));
                    put_u32(&app_data[5], static_cast<uint32_t>(before.longitude_micro));
                }

                memcpy(&app_data[offset], node_name_, name_len);

                mesh::Packet *packet = mesh_.createAdvert(mesh_.self_id, app_data, offset + name_len);

                if(packet == nullptr){
                    reply_error(ERR_FULL, cmd, len);
                    return;
                }

                packet->header |= ROUTE_TYPE_FLOOD;

                uint8_t response[hardware::bluetooth::Transport::MAX_FRAME] = { RESP_EXPORT_CONTACT };

                if(packet->getRawLength() + 1 > static_cast<int>(sizeof(response))){
                    mesh_.releasePacket(packet);
                    reply_error(ERR_FULL, cmd, len);
                    return;
                }

                const uint8_t packet_len = packet->writeTo(&response[1]);

                mesh_.releasePacket(packet);

                if(!ble_.send(response, packet_len + 1))
                    reply_error(ERR_BAD_STATE, cmd, len);
                return;
            }

            case CMD_REMOVE_CONTACT:
                if(len < 1 + PUB_KEY_SIZE){
                    reply_error(ERR_ILLEGAL , cmd, len);
                    return;
                }

                if(!contacts_.remove(&cmd[1])){
                    reply_error(ERR_NOT_FOUND , cmd, len);
                    return;
                }

                reply_ok();
                return;

            case CMD_RESET_PATH:
                if(len < 1 + PUB_KEY_SIZE){
                    reply_error(ERR_ILLEGAL , cmd, len);
                    return;
                }

                if(!contacts_.reset_path(&cmd[1])){
                    reply_error(ERR_NOT_FOUND , cmd, len);
                    return;
                }

                reply_ok();
                return;

            case CMD_GET_TIME: {
                uint8_t f[5] = {RESP_CURR_TIME};
                put_u32(&f[1], rtc_.getCurrentTime());
                ble_.send(f, sizeof(f));
                return;
            }
            
            case CMD_SET_TIME: {
                if (len != 5) {
                    reply_error(ERR_ILLEGAL, cmd, len);
                    return;
                }

                const uint32_t requested = get_u32(&cmd[1]);
                const uint32_t current = rtc_.getCurrentTime();

                // Reject obviously invalid Unix timestamps.
                if (requested < 1577836800U ||
                    requested > 4102444799U) {
                    reply_error(ERR_ILLEGAL, cmd, len);
                    return;
                }

                ESP_LOGD(TAG,
                        "SET_TIME requested=%lu current=%lu",
                        static_cast<unsigned long>(requested),
                        static_cast<unsigned long>(current));

                // Ignore small backward differences to preserve
                // monotonic message timestamps.
                if (current != 0 &&
                    requested < current &&
                    current - requested <= 30U) {
                    reply_ok();
                    return;
                }

                // The paired companion can correct an RTC that
                // contains an incorrect time, including backwards.
                if (current != 0 && requested < current) {
                    ESP_LOGW(TAG,
                            "Correcting RTC backwards by %lu seconds",
                            static_cast<unsigned long>(current - requested));
                }

                rtc_.setCurrentTime(requested);

                ESP_LOGD(TAG,
                        "SET_TIME applied=%lu",
                        static_cast<unsigned long>(rtc_.getCurrentTime()));

                reply_ok();
                return;
            }

            case CMD_SET_NAME: {
                if(len < 2 || len > 32){
                    reply_error(ERR_ILLEGAL , cmd, len);
                    return;
                }

                char new_name[32] = {};
                memcpy(new_name, &cmd[1], len - 1);

                if(memchr(new_name, ':', len - 1) != nullptr){
                    reply_error(ERR_ILLEGAL , cmd, len);
                    return;
                }

                char prior[32];
                memcpy(prior, node_name_, 32);
                memcpy(node_name_, new_name, 32);

                if(!save_name()){
                    memcpy(node_name_, prior, 32);
                    reply_error(ERR_IO, cmd, len);
                    return;
                }

                reply_ok();
                return;
            }

            case CMD_GET_CHANNEL:
                if(len != 2){
                    reply_error(ERR_ILLEGAL , cmd, len);
                    return;
                }

                handle_get_channel(cmd[1]);
                return;

            case CMD_SET_CHANNEL: {
                if(len != 50){
                    reply_error(ERR_ILLEGAL , cmd, len);
                    return;
                }

                char name[32] = {};
                memcpy(name, &cmd[2], 31);

                if(!mesh_.set_channel(cmd[1], name, &cmd[34])){
                    reply_error(ERR_ILLEGAL , cmd, len);
                    return;
                }

                reply_ok();
                return;
            }

            case CMD_SEND_CHANNEL_TXT: {
                if(len < 8 || cmd[1] != 0){
                    reply_error(ERR_ILLEGAL , cmd, len);
                    return;
                }

                uint32_t ts = get_u32(&cmd[3]);

                if(ts == 0)
                    ts = rtc_.getCurrentTimeUnique();

                if(!mesh_.send_channel_text(cmd[2], ts, node_name_, &cmd[7], len - 7)){

                    reply_error(ERR_FULL, cmd, len);
                    return;
                }

                reply_ok();
                return;
            }

            case CMD_SEND_TXT: {
                if(len < 14 || cmd[1] != 0){
                    reply_error(ERR_UNSUPPORTED, cmd, len);
                    return;
                }

                const auto *recipient = contacts_.find(&cmd[7], 6);

                if(recipient == nullptr){
                    reply_error(ERR_NOT_FOUND , cmd, len);
                    return;
                }

                uint32_t ts = get_u32(&cmd[3]);

                if(ts == 0)
                    ts = rtc_.getCurrentTimeUnique();

                uint32_t ack = 0;
                uint32_t timeout = 0;

                if(!mesh_.send_private_text(*recipient, ts, &cmd[13], len - 13, ack, timeout)){

                    reply_error(ERR_FULL, cmd, len);
                    return;
                }

                pending_acks_[ack_next_] = { ack, time_ms(clock_) };

                ack_next_ = (ack_next_ + 1) % 8;

                uint8_t f[10] = {RESP_SENT, 1};

                put_u32(&f[2], ack);
                put_u32(&f[6], timeout);

                ble_.send(f, sizeof(f));
                return;
            }

            case CMD_SYNC_MESSAGE:
                if(message_count_ == 0){
                    const uint8_t f[] = {RESP_NO_MORE};
                    ble_.send(f, 1);
                }else{
                    auto &f = messages_[message_head_];

                    if(ble_.send(f.data, f.length)){
                        message_head_ = (message_head_ + 1) % MESSAGE_QUEUE;
                        message_count_--;
                    }
                }
                return;

            case CMD_SEND_ADVERT:
                if(rtc_.getCurrentTime() == 0){
                    reply_error(ERR_BAD_STATE, cmd, len);
                    return;
                }

                if(!mesh_.send_self_advert(node_name_, len > 1 && cmd[1] == 1, before.latitude_micro, before.longitude_micro, before.advert_loc_policy != 0)){

                    reply_error(ERR_FULL, cmd, len);
                    return;
                }

                reply_ok();
                return;

            case CMD_SET_LOCATION: {
                if(len != 9 && len != 13){
                    reply_error(ERR_ILLEGAL , cmd, len);
                    return;
                }

                Settings next = before;

                next.latitude_micro = static_cast<int32_t>(get_u32(&cmd[1]));
                next.longitude_micro = static_cast<int32_t>(get_u32(&cmd[5]));

                if(!update_settings(next, false)){
                    reply_error(ERR_ILLEGAL , cmd, len);
                    return;
                }

                reply_ok();
                return;
            }

            case CMD_SET_RADIO: {
                if(len < 11 || len > 12){
                    reply_error(ERR_ILLEGAL , cmd, len);
                    return;
                }

                if(len == 12 && cmd[11] != 0){
                    reply_error(ERR_UNSUPPORTED, cmd, len);
                    return;
                }

                Settings next = before;

                next.freq_khz = get_u32(&cmd[1]);
                next.bandwidth_hz = get_u32(&cmd[5]);
                next.sf = cmd[9];
                next.cr = cmd[10];

                if(!update_settings(next, true)){
                    reply_error(ERR_ILLEGAL , cmd, len);
                    return;
                }

                reply_ok();
                return;
            }

            case CMD_SET_POWER: {
                if(len != 2){
                    reply_error(ERR_ILLEGAL , cmd, len);
                    return;
                }

                Settings next = before;
                next.tx_power_dbm = static_cast<int8_t>(cmd[1]);

                if(!update_settings(next, true)){
                    reply_error(ERR_ILLEGAL , cmd, len);
                    return;
                }

                reply_ok();
                return;
            }

            case CMD_SET_TUNING: {
                if(len != 9){
                    reply_error(ERR_ILLEGAL , cmd, len);
                    return;
                }

                Settings next = before;
                next.rx_delay_milli = get_u32(&cmd[1]);
                next.airtime_factor_milli = get_u32(&cmd[5]);

                if(!update_settings(next, false)){
                    reply_error(ERR_ILLEGAL , cmd, len);
                    return;
                }

                reply_ok();
                return;
            }

            case CMD_GET_TUNING: {
                uint8_t f[9] = {RESP_TUNING};

                put_u32(&f[1], before.rx_delay_milli);
                put_u32(&f[5], before.airtime_factor_milli);

                ble_.send(f, sizeof(f));
                return;
            }

            case CMD_SET_OTHER: {
                if(len < 2 || len > 5){
                    reply_error(ERR_ILLEGAL , cmd, len);
                    return;
                }

                Settings next = before;

                next.manual_contact_mode = cmd[1];

                if(len >= 3)
                    next.telemetry_modes = cmd[2];

                if(len >= 4)
                    next.advert_loc_policy = cmd[3];

                if(len >= 5)
                    next.multi_acks = cmd[4];

                if(!update_settings(next, false)){
                    reply_error(ERR_ILLEGAL , cmd, len);
                    return;
                }

                reply_ok();
                return;
            }

            case CMD_SET_BLE_PIN: {
                if(len != 5){
                    reply_error(ERR_ILLEGAL , cmd, len);
                    return;
                }

                if(ble_.set_pairing_pin(get_u32(&cmd[1])) != ESP_OK){
                    reply_error(ERR_ILLEGAL , cmd, len);
                    return;
                }

                reply_ok();
                return;
            }
            
            case CMD_GET_ADVERT_PATH: {
                if(len != 34){
                    reply_error(ERR_ILLEGAL, cmd, len);
                    return;
                }

                // Request layout:
                // Byte 0: command 42
                // Byte 1: reserved
                // Bytes 2-33: full 32-byte public key

                uint32_t received_at = 0;
                uint8_t path_len = 0;
                uint8_t path[MAX_PATH_SIZE] = {};

                if(!mesh_.get_advert_path(&cmd[2], received_at, path_len, path)){
                    reply_error(ERR_NOT_FOUND, cmd, len);
                    return;
                }

                // Official MeshCore response:
                // Byte 0: response 22
                // Bytes 1-4: reception timestamp
                // Byte 5: encoded path length
                // Bytes 6+: path bytes

                uint8_t response[6 + MAX_PATH_SIZE] = {};
                response[0] = RESP_ADVERT_PATH;

                put_u32(&response[1], received_at);
                response[5] = path_len;

                const size_t path_bytes = mesh::Packet::writePath(&response[6], path, path_len);
                const size_t response_length = 6 + path_bytes;

                if(!ble_.send(response, response_length)) reply_error(ERR_BAD_STATE, cmd, len);
                return;
            }

            case CMD_SET_AUTOADD: {
                if(len < 2 || len > 3){
                    reply_error(ERR_ILLEGAL , cmd, len);
                    return;
                }

                Settings next = before;
                next.autoadd_config = cmd[1];

                if(len == 3){
                    next.autoadd_max_hops = cmd[2] > 64 ? 64 : cmd[2];
                }

                if(!update_settings(next, false)){
                    reply_error(ERR_ILLEGAL , cmd, len);
                    return;
                }

                reply_ok();
                return;
            }

            case CMD_GET_AUTOADD: {
                uint8_t f[3] = { RESP_AUTOADD, before.autoadd_config, before.autoadd_max_hops };

                ble_.send(f, sizeof(f));
                return;
            }

            case CMD_SET_HASH_MODE: {
                if(len != 3 || cmd[1] != 0){
                    reply_error(ERR_ILLEGAL , cmd, len);
                    return;
                }

                Settings next = before;
                next.path_hash_mode = cmd[2];

                if(!update_settings(next, false)){
                    reply_error(ERR_ILLEGAL , cmd, len);
                    return;
                }

                reply_ok();
                return;
            }

            case CMD_SET_TEMP_SCOPE:
                if(len < 2){
                    reply_error(ERR_ILLEGAL , cmd, len);
                    return;
                }

                if(cmd[1] == 1){
                    mesh_.set_scope_key(nullptr);
                }else if(cmd[1] == 0){
                    if(len == 18){
                        mesh_.set_scope_key(&cmd[2]);
                    }else if(len == 2){
                        mesh_.set_scope_key(before.default_scope_key);
                    }else{
                        reply_error(ERR_ILLEGAL , cmd, len);
                        return;
                    }
                }else{
                    reply_error(ERR_ILLEGAL , cmd, len);
                    return;
                }

                reply_ok();
                return;

            case CMD_SET_DEFAULT_SCOPE: {
                Settings next = before;

                memset(next.default_scope_name, 0, sizeof(next.default_scope_name));

                memset(next.default_scope_key, 0, sizeof(next.default_scope_key));

                if(len == 48){
                    const size_t n = strnlen(reinterpret_cast<const char *>(&cmd[1]), 31);

                    if(n == 0 || n >= 31){
                        reply_error(ERR_ILLEGAL , cmd, len);
                        return;
                    }

                    memcpy(next.default_scope_name, &cmd[1], n);

                    memcpy(next.default_scope_key, &cmd[32], 16);

                }else if(len != 1){
                    reply_error(ERR_ILLEGAL , cmd, len);
                    return;
                }

                if(!update_settings(next, false)){
                    reply_error(ERR_IO, cmd, len);
                    return;
                }

                reply_ok();
                return;
            }

            case CMD_GET_DEFAULT_SCOPE: {
                uint8_t f[48] = {RESP_DEFAULT_SCOPE};

                if(before.default_scope_name[0] != 0){
                    memcpy(&f[1], before.default_scope_name, 31);

                    memcpy(&f[32], before.default_scope_key, 16);

                    ble_.send(f, sizeof(f));
                }else{
                    ble_.send(f, 1);
                }

                return;
            }

            case CMD_GET_REPEAT_FREQ: {
                const uint8_t f[1] = {RESP_REPEAT_FREQ};
                ble_.send(f, sizeof(f));
                return;
            }

            case CMD_SEND_TELEMETRY_REQ: {
                // Standard MeshCore Bluetooth self-telemetry request.
                if(len != 4){
                    reply_error(ERR_ILLEGAL , cmd, len);
                    return;
                }

                uint8_t f[hardware::bluetooth::Transport::MAX_FRAME] = { PUSH_TELEMETRY, 0 };

                memcpy(&f[2], mesh_.self_id.pub_key, 6);

                size_t bytes = telemetry_.encode(TELEM_BASE | TELEM_LOCATION, &f[8], sizeof(f) - 8);

                ESP_LOGD(TAG, "BLE telemetry: payload_bytes=%u gps_included=%d", static_cast<unsigned>(bytes), bytes >= 11 ? 1 : 0);

                if(!ble_.send(f, 8 + bytes)){
                    reply_error(ERR_BAD_STATE, cmd, len);
                }

                return;
            }

            case CMD_GET_BATT: {
                uint8_t f[11] = {RESP_BATT};

                put_u16(&f[1], telemetry_.battery_millivolts());

                uint32_t total = hardware::sd::capacity_kib();

                put_u32(&f[7], total);

                ble_.send(f, sizeof(f));
                return;
            }

            case CMD_GET_STATS: {
                if(len != 2){
                    reply_error(ERR_ILLEGAL , cmd, len);
                    return;
                }

                uint8_t f[32] = {RESP_STATS, cmd[1]};
                size_t n = 2;

                if(cmd[1] == 0){
                    put_u16(&f[n], telemetry_.battery_millivolts());
                    n += 2;

                    put_u32(&f[n], time_ms(clock_) / 1000);
                    n += 4;

                    put_u16(&f[n], mesh_.errorFlags());
                    n += 2;

                    f[n++] = static_cast<uint8_t>(mesh_.pendingOutboundCount());

                }else if(cmd[1] == 1){
                    put_u16(&f[n], 0);
                    n += 2;

                    f[n++] = static_cast<uint8_t>(static_cast<int8_t>(radio_.getLastRSSI()));

                    f[n++] = static_cast<uint8_t>(static_cast<int8_t>(radio_.getLastSNR() * 4));

                    put_u32(&f[n], mesh_.getTotalAirTime() / 1000);
                    n += 4;

                    put_u32(&f[n], mesh_.getReceiveAirTime() / 1000);
                    n += 4;

                }else if(cmd[1] == 2){
                    put_u32(&f[n], mesh_.packetCount());
                    n += 4;

                    put_u32(&f[n], mesh_.getNumSentFlood() + mesh_.getNumSentDirect());
                    n += 4;

                    put_u32(&f[n], mesh_.getNumSentFlood());
                    n += 4;

                    put_u32(&f[n], mesh_.getNumSentDirect());
                    n += 4;

                    put_u32(&f[n], mesh_.getNumRecvFlood());
                    n += 4;

                    put_u32(&f[n], mesh_.getNumRecvDirect());
                    n += 4;

                    put_u32(&f[n], 0);
                    n += 4;

                }else{
                    reply_error(ERR_ILLEGAL , cmd, len);
                    return;
                }

                ble_.send(f, n);
                return;
            }

            case CMD_REBOOT:
                if(len != 7 || memcmp(&cmd[1], "reboot", 6) != 0){
                    reply_error(ERR_ILLEGAL , cmd, len);
                    return;
                }

                reply_ok();

                reboot_at_ms_ = time_ms(clock_) + 2000;

                if(reboot_at_ms_ == 0)
                    reboot_at_ms_ = 1;

                return;
            
            case CMD_GET_CUSTOM_VARS: {
                if(len != 1){
                    reply_error(ERR_ILLEGAL , cmd, len);
                    return;
                }

                const uint8_t response[] = {RESP_CUSTOM_VARS, 'g', 'p', 's', ':', static_cast<uint8_t>(hardware::gps::is_enabled() ? '1' : '0')};
                ble_.send(response, sizeof(response));
                return;
            }

            case CMD_SET_CUSTOM_VAR: {
                if((len != 6 && !(len == 7 && cmd[6] == 0)) || memcmp(&cmd[1], "gps:", 4) != 0 || (cmd[5] != '0' && cmd[5] != '1')){
                    reply_error(ERR_ILLEGAL , cmd, len);
                    return;
                }

                bool enabled = cmd[5] == '1';
                esp_err_t err = hardware::gps::set_enabled(enabled);

                if(err != ESP_OK){
                    ESP_LOGW(TAG, "GPS setting update failed: %s", esp_err_to_name(err));
                    reply_error(ERR_IO, cmd, len);
                    return;
                }

                reply_ok();
                return;
            }

            default:
                ESP_LOGW(TAG, "Unsupported companion command %u (len=%u)", cmd[0], static_cast<unsigned>(len));

                reply_error(ERR_UNSUPPORTED, cmd, len);
                return;
        }
    }

    void Companion::loop(){
        hardware::bluetooth::Transport::Frame frame;

        for(int i = 0; i < 4 && ble_.receive(frame); i++){
            if(ble_.connected())
                handle(frame.data, frame.length);
        }

        if(ble_.connected())
            pump_contacts();
        else
            contact_sync_ = false;
        if(reboot_at_ms_ != 0 && static_cast<int32_t>(time_ms(clock_) - reboot_at_ms_) >= 0){

            ESP_LOGI(TAG, "Reboot requested by paired companion");

            hardware::system::restart();
        }
    }

}
