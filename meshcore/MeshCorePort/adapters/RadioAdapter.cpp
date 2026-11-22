#include "RadioAdapter.h"

#include <string.h>
#include "esp_log.h"

namespace meshcore {
    static const char *TAG = "RadioAdapter";

    RadioAdapter::RadioAdapter(const hardware::lora::config_t &config) : config_(config){}

    void RadioAdapter::begin(){
        esp_err_t err = hardware::lora::start_receive();
        if(err != ESP_OK) ESP_LOGE(TAG, "Cannot start reception: %s", esp_err_to_name(err));
    }

    int RadioAdapter::recvRaw(uint8_t *bytes, int size){
        if(bytes == nullptr || size <= 0 || !hardware::lora::is_in_receive_mode()) return 0;
        hardware::lora::packet_t packet;
        bool available = false;
        esp_err_t err = hardware::lora::poll_receive(packet, available);
        if(err == ESP_ERR_INVALID_CRC || err == ESP_ERR_INVALID_RESPONSE || err == ESP_ERR_TIMEOUT) return 0;
        if(err != ESP_OK){
            ESP_LOGW(TAG, "RX poll failed: %s", esp_err_to_name(err));
            return 0;
        }
        if(!available) return 0;
        if(packet.length > size){
            ESP_LOGW(TAG, "Dropped %u-byte packet: RX buffer is too small", packet.length);
            return 0;
        }
        memcpy(bytes, packet.data, packet.length);
        last_rssi_ = packet.rssi_dbm;
        last_snr_ = packet.snr_db;
        return packet.length;
    }

    uint32_t RadioAdapter::getEstAirtimeFor(int length){
        if(length < 0) return 0;
        if(length > 255) length = 255;
        const int sf = config_.spreading_factor;
        const uint32_t bw = config_.bandwidth_hz;
        if(sf < 5 || sf > 12 || bw == 0) return 0;
        const uint64_t symbol_us = ((1ULL << sf) * 1000000ULL + bw - 1) / bw;
        const int de = symbol_us >= 16000 ? 1 : 0;
        const int denominator = 4 * (sf - 2 * de);
        const int numerator = 8 * length - 4 * sf + 28 + (config_.crc_enabled ? 16 : 0);
        const int blocks = numerator > 0 ? (numerator + denominator - 1) / denominator : 0;
        const int payload_symbols = 8 + blocks * config_.coding_rate;
        const int quarter_preamble_symbols = 4 * config_.preamble_symbols + (sf <= 6 ? 25 : 17);
        const uint64_t quarter_symbols = quarter_preamble_symbols + 4ULL * payload_symbols;
        return static_cast<uint32_t>((quarter_symbols * symbol_us + 3999ULL) / 4000ULL);
    }

    float RadioAdapter::packetScore(float snr, int length){
        static constexpr float thresholds[] = {-7.5f, -10.0f, -12.5f, -15.0f, -17.5f, -20.0f};
        const int sf = config_.spreading_factor;
        if(sf < 7 || sf > 12) return 0.0f;
        const float threshold = thresholds[sf - 7];
        if(snr < threshold) return 0.0f;
        const float quality = (snr - threshold) / 10.0f;
        const float penalty = 1.0f - (static_cast<float>(length) / 256.0f);
        const float score = quality * penalty;
        return score < 0.0f ? 0.0f : score > 1.0f ? 1.0f : score;
    }

    bool RadioAdapter::startSendRaw(const uint8_t *bytes, int length){
        if(tx_pending_ || bytes == nullptr || length <= 0 || length > 255) return false;
        bool busy = true;
        esp_err_t err = hardware::lora::channel_busy(busy);
        if(err != ESP_OK || busy){
            ESP_LOGW(TAG, "TX blocked: channel busy or CAD failure (%s)", esp_err_to_name(err));
            return false;
        }
        err = hardware::lora::start_transmit(bytes, static_cast<size_t>(length), 10000);
        if(err != ESP_OK){
            ESP_LOGE(TAG, "TX start failed: %s", esp_err_to_name(err));
            return false;
        }
        tx_pending_ = true;
        return true;
    }

    bool RadioAdapter::isSendComplete(){
        if(!tx_pending_) return false;
        bool finished = false;
        esp_err_t err = hardware::lora::poll_transmit(finished);
        if(finished && err == ESP_OK) return true;
        if(finished && err != ESP_OK) ESP_LOGW(TAG, "TX failed: %s", esp_err_to_name(err));
        return false;
    }

    void RadioAdapter::onSendFinished(){
        if(tx_pending_){
            bool finished = false;
            esp_err_t err = hardware::lora::poll_transmit(finished);
            if(finished && (err == ESP_OK || err == ESP_ERR_TIMEOUT)) err = hardware::lora::finish_transmit();
            else err = hardware::lora::abort_transmit();
            if(err != ESP_OK) ESP_LOGW(TAG, "TX cleanup failed: %s", esp_err_to_name(err));
            tx_pending_ = false;
        }
        esp_err_t err = hardware::lora::start_receive();
        if(err != ESP_OK) ESP_LOGE(TAG, "Unable to resume RX: %s", esp_err_to_name(err));
    }

    bool RadioAdapter::isInRecvMode() const{
        return !tx_pending_ && hardware::lora::is_in_receive_mode();
    }

    bool RadioAdapter::isReceiving(){
        if(tx_pending_) return true;
        bool busy = true;
        esp_err_t err = hardware::lora::channel_busy(busy);
        return err != ESP_OK || busy;
    }

    float RadioAdapter::getLastRSSI() const{ return last_rssi_; }
    float RadioAdapter::getLastSNR() const{ return last_snr_; }

    bool RadioAdapter::applyConfig(const hardware::lora::config_t &next){
        if(tx_pending_) return false;
        hardware::lora::config_t prior = config_;
        esp_err_t err = hardware::lora::configure(next);
        if(err == ESP_OK) err = hardware::lora::start_receive();
        if(err == ESP_OK){
            config_ = next;
            ESP_LOGI(TAG, "Radio reconfigured successfully");
            return true;
        }
        ESP_LOGE(TAG, "Radio config failed: %s; restoring previous profile", esp_err_to_name(err));
        esp_err_t rollback = hardware::lora::configure(prior);
        if(rollback == ESP_OK) rollback = hardware::lora::start_receive();
        if(rollback != ESP_OK) ESP_LOGE(TAG, "Radio rollback failed: %s", esp_err_to_name(rollback));
        return false;
    }
}
