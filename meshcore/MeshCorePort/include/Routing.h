
#pragma once

#include <stdint.h>

#include "RadioAdapter.h"

#include "Dispatcher.h"
#include "helpers/SimpleMeshTables.h"
#include "helpers/StaticPoolPacketManager.h"

namespace meshcore {

    // Receive-only wrapper around the physical SX1262 radio.
    // Prevents the upstream MeshCore engine from transmitting.
    class ReceiveOnlyRadio final : public mesh::Radio {
    public:
        explicit ReceiveOnlyRadio(RadioAdapter &radio)
            : radio_(radio){}

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
        RadioAdapter &radio_;
    };

    // Receive-only dispatcher from the previous milestone.
    // Retained for compatibility and regression testing.
    class Routing final : public mesh::Dispatcher {
    public:
        Routing(mesh::Radio &radio, mesh::MillisecondClock &clock, StaticPoolPacketManager &manager, SimpleMeshTables &tables);

        uint32_t uniqueCount() const {
            return unique_count_;
        }

        uint32_t duplicateCount() const {
            return duplicate_count_;
        }

    protected:
        mesh::DispatcherAction onRecvPacket(mesh::Packet *packet) override;

        int calcRxDelay(float score, uint32_t air_time) const override;

    private:
        SimpleMeshTables &tables_;

        uint32_t unique_count_ = 0;
        uint32_t duplicate_count_ = 0;
    };

}
