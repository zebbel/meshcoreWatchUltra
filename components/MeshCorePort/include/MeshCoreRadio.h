
#pragma once

#include <stdint.h>
#include "Dispatcher.h"
#include "SX1262.h"

namespace watch {

    // Bridges the upstream mesh::Radio API to the native SX1262 driver.
    // Native hardware initialization and configure() must run before begin().
    class MeshCoreRadio final : public mesh::Radio {
    public:
        explicit MeshCoreRadio(const sx1262::config_t &config);

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

    private:
        sx1262::config_t config_;
        float last_rssi_ = 0;
        float last_snr_ = 0;
        bool tx_pending_ = false;
    };

}
