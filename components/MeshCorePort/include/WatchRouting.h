
#pragma once

#include <stdint.h>
#include "Dispatcher.h"
#include "MeshCoreRadio.h"
#include "helpers/SimpleMeshTables.h"
#include "helpers/StaticPoolPacketManager.h"

namespace watch {

    // Enforces RX-only operation even if the dispatcher ever receives an
    // outbound packet. The underlying SX1262 driver's TX code is not called.
    class ReceiveOnlyRadio final : public mesh::Radio {
    public:
        explicit ReceiveOnlyRadio(MeshCoreRadio &radio) : radio_(radio){}

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
        MeshCoreRadio &radio_;
    };

    // Real upstream MeshCore Dispatcher with a deliberately passive policy.
    // The upstream code parses raw packets, owns scheduling and pool lifecycle;
    // this app hook classifies and deduplicates them, never forwards packets.
    class WatchRouting final : public mesh::Dispatcher {
    public:
        WatchRouting(mesh::Radio &radio,
                     mesh::MillisecondClock &clock,
                     StaticPoolPacketManager &manager,
                     SimpleMeshTables &tables);

        uint32_t uniqueCount() const { return unique_count_; }
        uint32_t duplicateCount() const { return duplicate_count_; }

    protected:
        mesh::DispatcherAction onRecvPacket(mesh::Packet *packet) override;
        int calcRxDelay(float score, uint32_t air_time) const override;

    private:
        SimpleMeshTables &tables_;
        uint32_t unique_count_ = 0;
        uint32_t duplicate_count_ = 0;
    };

}
