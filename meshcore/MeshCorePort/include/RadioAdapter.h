#pragma once

#include <stdint.h>

#include "LoRa.h"

#include "Dispatcher.h"

namespace meshcore {
    class RadioAdapter final : public mesh::Radio {
    public:
        explicit RadioAdapter(const hardware::lora::config_t &config);
        void begin() override;
        int recvRaw(uint8_t *bytes, int size) override;
        uint32_t getEstAirtimeFor(int length) override;
        float packetScore(float snr, int length) override;
        bool startSendRaw(const uint8_t *bytes, int length) override;
        bool isSendComplete() override;
        void onSendFinished() override;
        bool isInRecvMode() const override;
        bool isReceiving() override;
        float getLastRSSI() const override;
        float getLastSNR() const override;

        // Reprogram hardware then restart RX. Revert to previous settings
        // if either step fails. Call only from main MeshCore task.
        bool applyConfig(const hardware::lora::config_t &config);
        const hardware::lora::config_t &config() const { return config_; }

    private:
        hardware::lora::config_t config_;
        float last_rssi_ = 0;
        float last_snr_ = 0;
        bool tx_pending_ = false;
    };
}
