#include "WatchCompanion.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_partition.h"
#include "nvs.h"

namespace watch {
    static const char *TAG = "WatchCompanion";

    // Identical command IDs to pinned upstream companion_radio/MyMesh.cpp.
    enum : uint8_t {
        CMD_APP_START=1, CMD_SEND_TXT=2, CMD_SEND_CHANNEL_TXT=3,
        CMD_GET_CONTACTS=4, CMD_GET_TIME=5, CMD_SET_TIME=6,
        CMD_SEND_ADVERT=7, CMD_SET_NAME=8, CMD_ADD_CONTACT=9,
        CMD_SYNC_MESSAGE=10, CMD_SET_RADIO=11, CMD_SET_POWER=12,
        CMD_RESET_PATH=13, CMD_SET_LOCATION=14, CMD_REMOVE_CONTACT=15,
        CMD_REBOOT=19, CMD_GET_BATT=20, CMD_SET_TUNING=21,
        CMD_DEVICE_QUERY=22, CMD_GET_CONTACT_KEY=30, CMD_GET_CHANNEL=31,
        CMD_SET_CHANNEL=32, CMD_SET_BLE_PIN=37, CMD_SET_OTHER=38,
        CMD_GET_TUNING=43, CMD_GET_STATS=56, CMD_SET_AUTOADD=58,
        CMD_GET_AUTOADD=59, CMD_GET_REPEAT_FREQ=60, CMD_SET_HASH_MODE=61,
        CMD_SET_TEMP_SCOPE=54, CMD_SET_DEFAULT_SCOPE=63, CMD_GET_DEFAULT_SCOPE=64
    };
    enum : uint8_t {
        RESP_OK=0, RESP_ERROR=1, RESP_CONTACTS_START=2,
        RESP_CONTACT=3, RESP_CONTACTS_END=4, RESP_SELF=5,
        RESP_SENT=6, RESP_PEER_V3=16, RESP_GROUP_V3=17,
        RESP_PEER_OLD=7, RESP_GROUP_OLD=8, RESP_CURR_TIME=9,
        RESP_NO_MORE=10, RESP_BATT=12, RESP_DEVICE=13,
        RESP_CHANNEL=18, RESP_TUNING=23, RESP_STATS=24,
        RESP_AUTOADD=25, RESP_REPEAT_FREQ=26, RESP_DEFAULT_SCOPE=28,
        PUSH_ADVERT=0x80, PUSH_ACK=0x82, PUSH_MSGS=0x83, PUSH_NEW_ADVERT=0x8A
    };
    enum : uint8_t {
        ERR_UNSUPPORTED=1, ERR_NOT_FOUND=2, ERR_FULL=3,
        ERR_BAD_STATE=4, ERR_IO=5, ERR_ILLEGAL=6
    };

    static void put_u32(uint8_t *p, uint32_t v){ memcpy(p, &v, 4); }
    static uint32_t get_u32(const uint8_t *p){ uint32_t v=0; memcpy(&v, p, 4); return v; }
    static void put_u16(uint8_t *p, uint16_t v){ memcpy(p, &v, 2); }
    static uint32_t time_ms(mesh::MillisecondClock &clock){
        return static_cast<uint32_t>(clock.getMillis());
    }

    WatchCompanion::WatchCompanion(WatchBLE &ble, WatchMesh &mesh, MeshCoreRadio &radio,
                                   WatchSettingsStore &settings, WatchContacts &contacts,
                                   WatchRTC &rtc, mesh::MillisecondClock &clock)
        : ble_(ble), mesh_(mesh), radio_(radio), settings_(settings),
          contacts_(contacts), rtc_(rtc), clock_(clock){
        snprintf(node_name_, sizeof(node_name_), "Watch-%02X%02X%02X%02X",
                 mesh_.self_id.pub_key[0], mesh_.self_id.pub_key[1],
                 mesh_.self_id.pub_key[2], mesh_.self_id.pub_key[3]);
        nvs_handle_t h;
        if(nvs_open("meshcore", NVS_READONLY, &h) == ESP_OK){
            size_t n = sizeof(node_name_);
            if(nvs_get_str(h, "node_name", node_name_, &n) != ESP_OK){
                snprintf(node_name_, sizeof(node_name_), "Watch-%02X%02X%02X%02X",
                         mesh_.self_id.pub_key[0], mesh_.self_id.pub_key[1],
                         mesh_.self_id.pub_key[2], mesh_.self_id.pub_key[3]);
            }
            nvs_close(h);
        }
        mesh_.set_contacts(&contacts_);
        mesh_.set_group_callback(group_callback, this);
        mesh_.set_peer_callback(peer_callback, this);
        mesh_.set_advert_callback(advert_callback, this);
        mesh_.set_ack_callback(ack_callback, this);
    }

    void WatchCompanion::group_callback(void *ctx, const mesh::Packet *p, uint8_t ch,
                                        uint32_t ts, const uint8_t *txt, size_t n){
        static_cast<WatchCompanion *>(ctx)->on_group_message(p, ch, ts, txt, n);
    }

    void WatchCompanion::peer_callback(void *ctx, const mesh::Packet *p, const uint8_t pub[32],
                                       uint8_t type, uint32_t ts, const uint8_t *txt, size_t n){
        static_cast<WatchCompanion *>(ctx)->on_peer_message(p, pub, type, ts, txt, n);
    }

    void WatchCompanion::advert_callback(void *ctx, const mesh::Identity &id,
                                         uint32_t ts, const uint8_t *data, size_t n){
        static_cast<WatchCompanion *>(ctx)->on_advert(id, ts, data, n);
    }

    void WatchCompanion::ack_callback(void *ctx, uint32_t crc){
        static_cast<WatchCompanion *>(ctx)->on_ack(crc);
    }

    void WatchCompanion::enqueue(const uint8_t *data, size_t len){
        if(data == nullptr || len == 0 || len > WatchBLE::MAX_FRAME) return;
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

    void WatchCompanion::on_group_message(const mesh::Packet *pkt, uint8_t ch, uint32_t ts,
                                          const uint8_t *text, size_t n){
        uint8_t f[WatchBLE::MAX_FRAME] = {};
        size_t i = 0;
        if(target_version_ >= 3){
            f[i++] = RESP_GROUP_V3;
            f[i++] = static_cast<uint8_t>(static_cast<int8_t>(pkt->getSNR() * 4));
            f[i++] = 0; f[i++] = 0;
        }else f[i++] = RESP_GROUP_OLD;
        f[i++] = ch;
        f[i++] = pkt->isRouteFlood() ? pkt->path_len : 0xFF;
        f[i++] = 0; // TXT_TYPE_PLAIN
        put_u32(&f[i], ts); i += 4;
        if(n > sizeof(f) - i) n = sizeof(f) - i;
        memcpy(&f[i], text, n); i += n;
        enqueue(f, i);
    }

    void WatchCompanion::on_peer_message(const mesh::Packet *pkt, const uint8_t pub[32],
                                         uint8_t type, uint32_t ts,
                                         const uint8_t *text, size_t n){
        uint8_t f[WatchBLE::MAX_FRAME] = {};
        size_t i = 0;
        if(target_version_ >= 3){
            f[i++] = RESP_PEER_V3;
            f[i++] = static_cast<uint8_t>(static_cast<int8_t>(pkt->getSNR() * 4));
            f[i++] = 0; f[i++] = 0;
        }else f[i++] = RESP_PEER_OLD;
        memcpy(&f[i], pub, 6); i += 6;
        f[i++] = pkt->isRouteFlood() ? pkt->path_len : 0xFF;
        f[i++] = type;
        put_u32(&f[i], ts); i += 4;
        if(n > sizeof(f) - i) n = sizeof(f) - i;
        memcpy(&f[i], text, n); i += n;
        enqueue(f, i);
    }

    void WatchCompanion::on_advert(const mesh::Identity &id, uint32_t ts,
                                   const uint8_t *data, size_t n){
        const auto *previous = contacts_.find(id.pub_key);
        bool is_new = previous == nullptr;
        const auto &cfg = settings_.get();
        // If manual contact mode is enabled, do not auto-add unknown peers.
        bool manual = (cfg.manual_contact_mode & 1) != 0;
        bool autoadd = !manual || (cfg.autoadd_config & 0x1e) != 0;
        uint8_t mask = manual ? cfg.autoadd_config : 0;
        if(!contacts_.observe_advert(id, ts, data, n, rtc_.getCurrentTime(),
                                     autoadd, mask)) return;
        const auto *contact = contacts_.find(id.pub_key);
        if(contact == nullptr) return;
        if(is_new) send_contact(*contact, PUSH_NEW_ADVERT);
        else{
            uint8_t f[1 + PUB_KEY_SIZE] = {PUSH_ADVERT};
            memcpy(&f[1], id.pub_key, PUB_KEY_SIZE);
            ble_.send(f, sizeof(f));
        }
    }

    void WatchCompanion::on_ack(uint32_t crc){
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

    void WatchCompanion::reply_error(uint8_t code){
        const uint8_t f[2] = {RESP_ERROR, code};
        ble_.send(f, 2);
    }
    void WatchCompanion::reply_ok(){
        const uint8_t f[1] = {RESP_OK};
        ble_.send(f, 1);
    }

    bool WatchCompanion::save_name(){
        nvs_handle_t h;
        if(nvs_open("meshcore", NVS_READWRITE, &h) != ESP_OK) return false;
        esp_err_t err = nvs_set_str(h, "node_name", node_name_);
        if(err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
        return err == ESP_OK;
    }

    bool WatchCompanion::update_settings(const WatchSettings &next, bool radio_change){
        if(!next.valid()) return false;
        const WatchSettings before = settings_.get();
        if(radio_change && mesh_.pendingOutboundCount() != 0) return false;
        if(radio_change && !radio_.applyConfig(next.radio())) return false;
        if(settings_.save(next) == ESP_OK){
            mesh_.set_tuning(next.rx_delay_milli, next.airtime_factor_milli);
            mesh_.set_path_hash_mode(next.path_hash_mode);
            mesh_.set_scope_key(next.default_scope_key);
            ble_.set_pairing_pin(next.ble_pin);
            return true;
        }
        if(radio_change && !radio_.applyConfig(before.radio())){
            ESP_LOGE(TAG, "Failed restoring radio after NVS error");
        }
        return false;
    }

    void WatchCompanion::handle_app_start(){
        const auto &s = settings_.get();
        uint8_t f[WatchBLE::MAX_FRAME] = {};
        size_t i = 0;
        f[i++] = RESP_SELF; f[i++] = 1; // ADV_TYPE_CHAT
        f[i++] = static_cast<uint8_t>(s.tx_power_dbm);
        f[i++] = 14; // software-enforced current EU maximum
        memcpy(&f[i], mesh_.self_id.pub_key, PUB_KEY_SIZE); i += PUB_KEY_SIZE;
        put_u32(&f[i], static_cast<uint32_t>(s.latitude_micro)); i += 4;
        put_u32(&f[i], static_cast<uint32_t>(s.longitude_micro)); i += 4;
        f[i++] = s.multi_acks;
        f[i++] = s.advert_loc_policy;
        f[i++] = s.telemetry_modes;
        f[i++] = s.manual_contact_mode;
        put_u32(&f[i], s.freq_khz); i += 4;
        put_u32(&f[i], s.bandwidth_hz); i += 4;
        f[i++] = s.sf; f[i++] = s.cr;
        const size_t n = strnlen(node_name_, sizeof(node_name_));
        memcpy(&f[i], node_name_, n); i += n;
        ble_.send(f, i);
    }

    void WatchCompanion::handle_device_query(){
        uint8_t f[96] = {};
        size_t i = 0;
        f[i++] = RESP_DEVICE;
        f[i++] = 14; // protocol frame version; unsupported commands return ERR_UNSUPPORTED
        f[i++] = WatchContacts::CAPACITY / 2;
        f[i++] = WatchMesh::CHANNEL_COUNT;
        put_u32(&f[i], settings_.get().ble_pin); i += 4;
        snprintf(reinterpret_cast<char *>(&f[i]), 12, "%s", "2026-11-20"); i += 12;
        snprintf(reinterpret_cast<char *>(&f[i]), 40, "%s", "LilyGO T-Watch Ultra"); i += 40;
        snprintf(reinterpret_cast<char *>(&f[i]), 20, "%s", "watch-ble-m6"); i += 20;
        f[i++] = 0; // Non-repeater, by design
        f[i++] = settings_.get().path_hash_mode;
        ble_.send(f, i);
    }

    void WatchCompanion::handle_get_channel(uint8_t index){
        char name[32] = {};
        uint8_t secret[16] = {};
        if(!mesh_.get_channel(index, name, secret)){
            reply_error(ERR_NOT_FOUND); return;
        }
        uint8_t f[50] = {RESP_CHANNEL, index};
        memcpy(&f[2], name, 32);
        memcpy(&f[34], secret, 16);
        ble_.send(f, sizeof(f));
    }

    bool WatchCompanion::send_contact(const WatchContact &c, uint8_t code){
        uint8_t f[WatchContacts::WIRE_LENGTH] = {};
        size_t n = contacts_.to_wire(f, c);
        if(n == 0) return false;
        f[0] = code;
        return ble_.send(f, n);
    }

    void WatchCompanion::pump_contacts(){
        if(!contact_sync_) return;
        for(; contact_index_ < WatchContacts::CAPACITY; contact_index_++){
            const WatchContact *peer = contacts_.at(contact_index_);
            if(peer == nullptr || peer->lastmod <= contact_since_) continue;
            if(!send_contact(*peer)) return; // backpressure until TX queue drains
            if(peer->lastmod > last_mod_) last_mod_ = peer->lastmod;
            contact_index_++;
            return; // pace the contact sync
        }
        uint8_t f[5] = {RESP_CONTACTS_END};
        put_u32(&f[1], last_mod_);
        if(ble_.send(f, sizeof(f))) contact_sync_ = false;
    }

    void WatchCompanion::handle(const uint8_t *cmd, size_t len){
        if(cmd == nullptr || len == 0) return;
        ESP_LOGI(TAG, "BLE command=%u length=%u", cmd[0], static_cast<unsigned>(len));
        const auto &before = settings_.get();
        switch(cmd[0]){
            case CMD_APP_START:
                if(len < 8){ reply_error(ERR_ILLEGAL); return; }
                contact_sync_ = false;
                handle_app_start(); return;
            case CMD_DEVICE_QUERY:
                if(len < 2){ reply_error(ERR_ILLEGAL); return; }
                target_version_ = cmd[1];
                handle_device_query(); return;
            case CMD_GET_CONTACTS: {
                if(contact_sync_){ reply_error(ERR_BAD_STATE); return; }
                contact_since_ = len >= 5 ? get_u32(&cmd[1]) : 0;
                contact_index_ = 0;
                last_mod_ = 0;
                uint8_t f[5] = {RESP_CONTACTS_START};
                put_u32(&f[1], contacts_.count());
                if(!ble_.send(f, sizeof(f))){ reply_error(ERR_FULL); return; }
                contact_sync_ = true;
                return;
            }
            case CMD_GET_CONTACT_KEY:
                if(len != 1 + PUB_KEY_SIZE){ reply_error(ERR_ILLEGAL); return; }
                if(auto *c = contacts_.find(&cmd[1])) send_contact(*c);
                else reply_error(ERR_NOT_FOUND);
                return;
            case CMD_ADD_CONTACT:
                if(len < WatchContacts::WIRE_LENGTH - 12 || len > WatchContacts::WIRE_LENGTH){
                    reply_error(ERR_ILLEGAL); return;
                }
                if(!contacts_.update_from_wire(cmd, len, rtc_.getCurrentTime())){
                    reply_error(ERR_FULL); return;
                }
                reply_ok(); return;
            case CMD_REMOVE_CONTACT:
                if(len < 1 + PUB_KEY_SIZE){ reply_error(ERR_ILLEGAL); return; }
                if(!contacts_.remove(&cmd[1])){ reply_error(ERR_NOT_FOUND); return; }
                reply_ok(); return;
            case CMD_RESET_PATH:
                if(len < 1 + PUB_KEY_SIZE){ reply_error(ERR_ILLEGAL); return; }
                if(!contacts_.reset_path(&cmd[1])){ reply_error(ERR_NOT_FOUND); return; }
                reply_ok(); return;
            case CMD_GET_TIME: {
                uint8_t f[5] = {RESP_CURR_TIME};
                put_u32(&f[1], rtc_.getCurrentTime());
                ble_.send(f, sizeof(f)); return;
            }
            case CMD_SET_TIME:
                if(len != 5 || get_u32(&cmd[1]) < rtc_.getCurrentTime()){
                    reply_error(ERR_ILLEGAL); return;
                }
                rtc_.setCurrentTime(get_u32(&cmd[1])); reply_ok(); return;
            case CMD_SET_NAME: {
                if(len < 2 || len > 32){ reply_error(ERR_ILLEGAL); return; }
                char new_name[32] = {};
                memcpy(new_name, &cmd[1], len - 1);
                if(memchr(new_name, ':', len - 1) != nullptr){ reply_error(ERR_ILLEGAL); return; }
                char prior[32]; memcpy(prior, node_name_, 32);
                memcpy(node_name_, new_name, 32);
                if(!save_name()){
                    memcpy(node_name_, prior, 32);
                    reply_error(ERR_IO); return;
                }
                reply_ok(); return;
            }
            case CMD_GET_CHANNEL:
                if(len != 2){ reply_error(ERR_ILLEGAL); return; }
                handle_get_channel(cmd[1]); return;
            case CMD_SET_CHANNEL: {
                if(len != 50){ reply_error(ERR_ILLEGAL); return; }
                char name[32] = {};
                memcpy(name, &cmd[2], 31);
                if(!mesh_.set_channel(cmd[1], name, &cmd[34])){
                    reply_error(ERR_ILLEGAL); return;
                }
                reply_ok(); return;
            }
            case CMD_SEND_CHANNEL_TXT: {
                if(len < 8 || cmd[1] != 0){ reply_error(ERR_ILLEGAL); return; }
                uint32_t ts = get_u32(&cmd[3]);
                if(ts == 0) ts = rtc_.getCurrentTimeUnique();
                if(!mesh_.send_channel_text(cmd[2], ts, node_name_, &cmd[7], len - 7)){
                    reply_error(ERR_FULL); return;
                }
                reply_ok(); return;
            }
            case CMD_SEND_TXT: {
                if(len < 14 || cmd[1] != 0){ reply_error(ERR_UNSUPPORTED); return; }
                const auto *recipient = contacts_.find(&cmd[7], 6);
                if(recipient == nullptr){ reply_error(ERR_NOT_FOUND); return; }
                uint32_t ts = get_u32(&cmd[3]);
                if(ts == 0) ts = rtc_.getCurrentTimeUnique();
                uint32_t ack = 0, timeout = 0;
                if(!mesh_.send_private_text(*recipient, ts, &cmd[13], len - 13, ack, timeout)){
                    reply_error(ERR_FULL); return;
                }
                pending_acks_[ack_next_] = {ack, time_ms(clock_)};
                ack_next_ = (ack_next_ + 1) % 8;
                uint8_t f[10] = {RESP_SENT, 1};
                put_u32(&f[2], ack); put_u32(&f[6], timeout);
                ble_.send(f, sizeof(f)); return;
            }
            case CMD_SYNC_MESSAGE:
                if(message_count_ == 0){
                    const uint8_t f[] = {RESP_NO_MORE}; ble_.send(f, 1);
                }else{
                    auto &f = messages_[message_head_];
                    if(ble_.send(f.data, f.length)){
                        message_head_ = (message_head_ + 1) % MESSAGE_QUEUE;
                        message_count_--;
                    }
                }
                return;
            case CMD_SEND_ADVERT:
                if(rtc_.getCurrentTime() == 0){ reply_error(ERR_BAD_STATE); return; }
                if(!mesh_.send_self_advert(node_name_, len > 1 && cmd[1] == 1,
                      before.latitude_micro, before.longitude_micro, before.advert_loc_policy != 0)){
                    reply_error(ERR_FULL); return;
                }
                reply_ok(); return;
            case CMD_SET_LOCATION: {
                if(len != 9 && len != 13){ reply_error(ERR_ILLEGAL); return; }
                WatchSettings next = before;
                next.latitude_micro = static_cast<int32_t>(get_u32(&cmd[1]));
                next.longitude_micro = static_cast<int32_t>(get_u32(&cmd[5]));
                if(!update_settings(next, false)){ reply_error(ERR_ILLEGAL); return; }
                reply_ok(); return;
            }
            case CMD_SET_RADIO: {
                if(len < 11 || len > 12){ reply_error(ERR_ILLEGAL); return; }
                if(len == 12 && cmd[11] != 0){
                    reply_error(ERR_UNSUPPORTED); return; // This is a companion, never a repeater.
                }
                WatchSettings next = before;
                next.freq_khz = get_u32(&cmd[1]);
                next.bandwidth_hz = get_u32(&cmd[5]);
                next.sf = cmd[9]; next.cr = cmd[10];
                if(!update_settings(next, true)){ reply_error(ERR_ILLEGAL); return; }
                reply_ok(); return;
            }
            case CMD_SET_POWER: {
                if(len != 2){ reply_error(ERR_ILLEGAL); return; }
                WatchSettings next = before;
                next.tx_power_dbm = static_cast<int8_t>(cmd[1]);
                if(!update_settings(next, true)){ reply_error(ERR_ILLEGAL); return; }
                reply_ok(); return;
            }
            case CMD_SET_TUNING: {
                if(len != 9){ reply_error(ERR_ILLEGAL); return; }
                WatchSettings next = before;
                next.rx_delay_milli = get_u32(&cmd[1]);
                next.airtime_factor_milli = get_u32(&cmd[5]);
                if(!update_settings(next, false)){ reply_error(ERR_ILLEGAL); return; }
                reply_ok(); return;
            }
            case CMD_GET_TUNING: {
                uint8_t f[9] = {RESP_TUNING};
                put_u32(&f[1], before.rx_delay_milli);
                put_u32(&f[5], before.airtime_factor_milli);
                ble_.send(f, sizeof(f)); return;
            }
            case CMD_SET_OTHER: {
                if(len < 2 || len > 5){ reply_error(ERR_ILLEGAL); return; }
                WatchSettings next = before;
                next.manual_contact_mode = cmd[1];
                if(len >= 3) next.telemetry_modes = cmd[2];
                if(len >= 4) next.advert_loc_policy = cmd[3];
                if(len >= 5) next.multi_acks = cmd[4];
                if(!update_settings(next, false)){ reply_error(ERR_ILLEGAL); return; }
                reply_ok(); return;
            }
            case CMD_SET_BLE_PIN: {
                if(len != 5){ reply_error(ERR_ILLEGAL); return; }
                WatchSettings next = before;
                next.ble_pin = get_u32(&cmd[1]);
                if(!update_settings(next, false)){ reply_error(ERR_ILLEGAL); return; }
                // A new code affects future pairing; existing BLE bond is retained.
                reply_ok(); return;
            }
            case CMD_SET_AUTOADD: {
                if(len < 2 || len > 3){ reply_error(ERR_ILLEGAL); return; }
                WatchSettings next = before;
                next.autoadd_config = cmd[1];
                if(len == 3) next.autoadd_max_hops = cmd[2];
                if(!update_settings(next, false)){ reply_error(ERR_ILLEGAL); return; }
                reply_ok(); return;
            }
            case CMD_GET_AUTOADD: {
                uint8_t f[3] = {RESP_AUTOADD, before.autoadd_config, before.autoadd_max_hops};
                ble_.send(f, sizeof(f)); return;
            }
            case CMD_SET_HASH_MODE: {
                if(len != 3 || cmd[1] != 0){ reply_error(ERR_ILLEGAL); return; }
                WatchSettings next = before;
                next.path_hash_mode = cmd[2];
                if(!update_settings(next, false)){ reply_error(ERR_ILLEGAL); return; }
                reply_ok(); return;
            }
            case CMD_SET_TEMP_SCOPE:
                if(len < 2){ reply_error(ERR_ILLEGAL); return; }
                if(cmd[1] == 1){
                    mesh_.set_scope_key(nullptr);
                }else if(cmd[1] == 0){
                    if(len == 18) mesh_.set_scope_key(&cmd[2]);
                    else if(len == 2) mesh_.set_scope_key(before.default_scope_key);
                    else{ reply_error(ERR_ILLEGAL); return; }
                }else{ reply_error(ERR_ILLEGAL); return; }
                reply_ok(); return;
            case CMD_SET_DEFAULT_SCOPE: {
                WatchSettings next = before;
                memset(next.default_scope_name, 0, sizeof(next.default_scope_name));
                memset(next.default_scope_key, 0, sizeof(next.default_scope_key));
                if(len == 48){
                    const size_t n = strnlen(reinterpret_cast<const char *>(&cmd[1]), 31);
                    if(n == 0 || n >= 31){ reply_error(ERR_ILLEGAL); return; }
                    memcpy(next.default_scope_name, &cmd[1], n);
                    memcpy(next.default_scope_key, &cmd[32], 16);
                }else if(len != 1){ reply_error(ERR_ILLEGAL); return; }
                if(!update_settings(next, false)){ reply_error(ERR_IO); return; }
                reply_ok(); return;
            }
            case CMD_GET_DEFAULT_SCOPE: {
                uint8_t f[48] = {RESP_DEFAULT_SCOPE};
                if(before.default_scope_name[0] != 0){
                    memcpy(&f[1], before.default_scope_name, 31);
                    memcpy(&f[32], before.default_scope_key, 16);
                    ble_.send(f, sizeof(f));
                }else ble_.send(f, 1);
                return;
            }
            case CMD_GET_REPEAT_FREQ: {
                const uint8_t f[1] = {RESP_REPEAT_FREQ}; // no repeater frequency ranges
                ble_.send(f, sizeof(f)); return;
            }
            case CMD_GET_BATT: {
                uint8_t f[11] = {RESP_BATT};
                // Battery ADC API is not available yet. 0 indicates unknown.
                // 'storage' partition capacity is real; used bytes are unknown (0).
                const auto *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                            ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "storage");
                uint32_t total = part ? part->size / 1024U : 0;
                put_u32(&f[7], total);
                ble_.send(f, sizeof(f)); return;
            }
            case CMD_GET_STATS: {
                if(len != 2){ reply_error(ERR_ILLEGAL); return; }
                uint8_t f[32] = {RESP_STATS, cmd[1]};
                size_t n = 2;
                if(cmd[1] == 0){ // core
                    put_u16(&f[n], 0); n += 2; // battery sensor unavailable
                    put_u32(&f[n], time_ms(clock_) / 1000); n += 4;
                    put_u16(&f[n], mesh_.errorFlags()); n += 2;
                    f[n++] = static_cast<uint8_t>(mesh_.pendingOutboundCount());
                }else if(cmd[1] == 1){ // radio
                    put_u16(&f[n], 0); n += 2; // noise floor unavailable
                    f[n++] = static_cast<uint8_t>(static_cast<int8_t>(radio_.getLastRSSI()));
                    f[n++] = static_cast<uint8_t>(static_cast<int8_t>(radio_.getLastSNR() * 4));
                    put_u32(&f[n], mesh_.getTotalAirTime() / 1000); n += 4;
                    put_u32(&f[n], mesh_.getReceiveAirTime() / 1000); n += 4;
                }else if(cmd[1] == 2){ // packet counters
                    put_u32(&f[n], mesh_.packetCount()); n += 4;
                    put_u32(&f[n], mesh_.getNumSentFlood() + mesh_.getNumSentDirect()); n += 4;
                    put_u32(&f[n], mesh_.getNumSentFlood()); n += 4;
                    put_u32(&f[n], mesh_.getNumSentDirect()); n += 4;
                    put_u32(&f[n], mesh_.getNumRecvFlood()); n += 4;
                    put_u32(&f[n], mesh_.getNumRecvDirect()); n += 4;
                    put_u32(&f[n], 0); n += 4; // RX error counter not instrumented
                }else{ reply_error(ERR_ILLEGAL); return; }
                ble_.send(f, n); return;
            }
            case CMD_REBOOT:
                if(len != 7 || memcmp(&cmd[1], "reboot", 6) != 0){ reply_error(ERR_ILLEGAL); return; }
                // No false acknowledgement: a deferred safe reboot is not implemented.
                reply_error(ERR_UNSUPPORTED);
                return;
            default:
                ESP_LOGW(TAG, "Unsupported companion command %u (len=%u)",
                         cmd[0], static_cast<unsigned>(len));
                reply_error(ERR_UNSUPPORTED); return;
        }
    }

    void WatchCompanion::loop(){
        WatchBLE::Frame frame;
        for(int i = 0; i < 4 && ble_.receive(frame); i++){
            if(ble_.connected()) handle(frame.data, frame.length);
        }
        if(ble_.connected()) pump_contacts();
        else contact_sync_ = false;
        ble_.loop();
    }
}
