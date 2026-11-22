
#include "Routing.h"

#include "esp_log.h"

namespace meshcore {

    static const char *TAG = "MeshCoreRouting";

    void ReceiveOnlyRadio::begin(){
        radio_.begin();
    }

    int ReceiveOnlyRadio::recvRaw(uint8_t *bytes, int size){
        const int length = radio_.recvRaw(bytes, size);
        if(length <= 0) return 0;

        // Upstream Dispatcher::tryParsePacket() trusts at least the first
        // header and path fields. Validate boundaries before passing it data.
        const bool transport = (bytes[0] & PH_ROUTE_MASK) == ROUTE_TYPE_TRANSPORT_FLOOD || (bytes[0] & PH_ROUTE_MASK) == ROUTE_TYPE_TRANSPORT_DIRECT;
        const int path_index = transport ? 5 : 1;

        if(length <= path_index + 1) return 0;
        const uint8_t path_length = bytes[path_index];
        if(!mesh::Packet::isValidPathLen(path_length)) return 0;

        const int hashes = path_length & 63;
        const int hash_size = (path_length >> 6) + 1;
        const int payload_index = path_index + 1 + hashes * hash_size;

        if(payload_index >= length || length - payload_index > MAX_PACKET_PAYLOAD){
            ESP_LOGW(TAG, "Dropped invalid frame: %d bytes", length);
            return 0;
        }
        return length;
    }

    uint32_t ReceiveOnlyRadio::getEstAirtimeFor(int length){
        return radio_.getEstAirtimeFor(length);
    }

    float ReceiveOnlyRadio::packetScore(float snr, int length){
        return radio_.packetScore(snr, length);
    }

    bool ReceiveOnlyRadio::startSendRaw(const uint8_t *bytes, int length){
        (void)bytes;
        (void)length;
        ESP_LOGW(TAG, "TX blocked: routing monitor is receive-only");
        return false;
    }

    bool ReceiveOnlyRadio::isSendComplete(){
        return false;
    }

    void ReceiveOnlyRadio::onSendFinished(){}

    bool ReceiveOnlyRadio::isInRecvMode() const{
        return radio_.isInRecvMode();
    }

    bool ReceiveOnlyRadio::isReceiving(){
        return radio_.isReceiving();
    }

    float ReceiveOnlyRadio::getLastRSSI() const{
        return radio_.getLastRSSI();
    }

    float ReceiveOnlyRadio::getLastSNR() const{
        return radio_.getLastSNR();
    }

    Routing::Routing(mesh::Radio &radio, mesh::MillisecondClock &clock, StaticPoolPacketManager &manager, SimpleMeshTables &tables)
        : mesh::Dispatcher(radio, clock, manager), tables_(tables){}

    // Read-only monitoring does not need flood retransmission delays.
    int Routing::calcRxDelay(float score, uint32_t air_time) const{
        (void)score;
        (void)air_time;
        return 0;
    }

    mesh::DispatcherAction Routing::onRecvPacket(mesh::Packet *packet){
        const bool duplicate = tables_.wasSeen(packet);
        if(duplicate){
            duplicate_count_++;
        }else{
            tables_.markSeen(packet);
            unique_count_++;
        }

        uint8_t hash[MAX_HASH_SIZE] = {};
        packet->calculatePacketHash(hash);

        ESP_LOGI(TAG, "RX %s: type=%u route=%u payload=%u, RSSI=%.1f, SNR=%.1f, hash=%02X%02X%02X%02X%02X%02X%02X%02X", duplicate ? "duplicate" : "unique", static_cast<unsigned>(packet->getPayloadType()), static_cast<unsigned>(packet->getRouteType()), static_cast<unsigned>(packet->payload_len), _radio->getLastRSSI(), packet->getSNR(), hash[0], hash[1], hash[2], hash[3], hash[4], hash[5], hash[6], hash[7]);

        return ACTION_RELEASE; // Never repeat or transmit a received packet.
    }

}
