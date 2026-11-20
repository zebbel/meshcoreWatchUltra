
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board.h"
#include "AXP2101.h"
#include "XL9555.h"
#include "SX1262.h"
#include "MeshCoreRadio.h"
#include "Packet.h"

static const char *TAG = "MeshCoreWatch";

static bool parse_packet(const uint8_t *raw, int length, mesh::Packet &packet){
    if(raw == nullptr || length < 3 || length > MAX_TRANS_UNIT) return false;

    const uint8_t route = raw[0] & PH_ROUTE_MASK;
    const bool transport = (route == ROUTE_TYPE_TRANSPORT_FLOOD || route == ROUTE_TYPE_TRANSPORT_DIRECT);
    const size_t path_index = transport ? 5 : 1;

    if(static_cast<size_t>(length) <= path_index + 1) return false;
    if(((raw[0] >> PH_VER_SHIFT) & PH_VER_MASK) > PAYLOAD_VER_1) return false;

    const uint8_t encoded_path = raw[path_index];
    if(!mesh::Packet::isValidPathLen(encoded_path)) return false;

    const size_t path_bytes = static_cast<size_t>(encoded_path & 63) * ((encoded_path >> 6) + 1);
    const size_t payload_index = path_index + 1 + path_bytes;

    if(payload_index >= static_cast<size_t>(length)) return false;
    if(static_cast<size_t>(length) - payload_index > MAX_PACKET_PAYLOAD) return false;

    return packet.readFrom(raw, static_cast<uint8_t>(length));
}

extern "C" void app_main(void){
    ESP_LOGI(TAG, "LilyGO T-Watch Ultra started");

    esp_err_t err = board::init();

    if(err != ESP_OK){
        ESP_LOGE(TAG, "Board initialization failed: %s", esp_err_to_name(err));
        return;
    }

    err = AXP2101::init();

    if(err != ESP_OK){
        ESP_LOGE(TAG, "Power initialization failed: %s", esp_err_to_name(err));
        return;
    }

    err = AXP2101::configure();

    if(err != ESP_OK){
        ESP_LOGE(TAG, "Power configuration failed: %s", esp_err_to_name(err));
        return;
    }

    err = XL9555::init();

    if(err != ESP_OK){
        ESP_LOGE(TAG, "GPIO expander initialization failed: %s", esp_err_to_name(err));
        return;
    }

    err = XL9555::configure();

    if(err != ESP_OK){
        ESP_LOGE(TAG, "GPIO expander configuration failed: %s", esp_err_to_name(err));
        return;
    }

    err = sx1262::init();

    if(err != ESP_OK){
        ESP_LOGE(TAG, "SX1262 initialization failed: %s", esp_err_to_name(err));
        return;
    }

    // MeshCore EU/UK Narrow radio settings
    sx1262::config_t config;
    config.frequency_hz = 869618000;
    config.bandwidth_hz = 62500;
    config.spreading_factor = 8;
    config.coding_rate = 8;
    config.sync_word = 0x12;
    config.preamble_symbols = 32;
    config.crc_enabled = true;
    config.tx_power_dbm = 22;

    err = sx1262::configure(config);

    if(err != ESP_OK){
        ESP_LOGE(TAG, "Radio configuration failed: %s", esp_err_to_name(err));
        return;
    }

    // The adapter bridges our SX1262 driver to upstream MeshCore.
    watch::MeshCoreRadio radio(config);
    radio.begin();

    if(!radio.isInRecvMode()){
        ESP_LOGE(TAG, "MeshCore radio adapter failed to start RX");
        return;
    }

    ESP_LOGI(TAG, "Upstream MeshCore packet parser ready (RX only)");

    while(true){
        uint8_t raw[MAX_TRANS_UNIT] = {};
        const int length = radio.recvRaw(raw, sizeof(raw));

        if(length > 0){
            mesh::Packet packet;

            if(parse_packet(raw, length, packet)){
                uint8_t packet_hash[MAX_HASH_SIZE] = {};
                packet.calculatePacketHash(packet_hash);

                ESP_LOGI(TAG, "MeshCore RX: type=%u route=%u payload=%u bytes, RSSI=%.1f dBm, SNR=%.1f dB",
                         packet.getPayloadType(),
                         packet.getRouteType(),
                         static_cast<unsigned>(packet.payload_len),
                         radio.getLastRSSI(),
                         radio.getLastSNR());

                ESP_LOG_BUFFER_HEX(TAG, packet_hash, sizeof(packet_hash));
            }else{
                ESP_LOGW(TAG, "Invalid MeshCore wire packet: %d bytes", length);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
