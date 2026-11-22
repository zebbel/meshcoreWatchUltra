
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "BluetoothTransport.h"
#include "Node.h"
#include "Settings.h"
#include "Contacts.h"
#include "Telemetry.h"
#include "RadioAdapter.h"
#include "PlatformAdapters.h"

namespace meshcore {

    class Companion {
    public:
        Companion(hardware::bluetooth::Transport &ble, Node &mesh, RadioAdapter &radio, SettingsStore &settings, Contacts &contacts, RTCAdapter &rtc, mesh::MillisecondClock &clock, Telemetry &telemetry);

        void loop();

    private:
        hardware::bluetooth::Transport &ble_;
        Node &mesh_;
        RadioAdapter &radio_;
        SettingsStore &settings_;
        Contacts &contacts_;
        RTCAdapter &rtc_;
        mesh::MillisecondClock &clock_;
        Telemetry &telemetry_;

        uint8_t target_version_ = 3;
        char node_name_[32] = {};
        bool contact_sync_ = false;
        size_t contact_index_ = 0;
        uint32_t contact_since_ = 0;
        uint32_t last_mod_ = 0;
        uint32_t reboot_at_ms_ = 0;

        static constexpr size_t MESSAGE_QUEUE = 32;
        hardware::bluetooth::Transport::Frame messages_[MESSAGE_QUEUE] = {};
        size_t message_head_ = 0;
        size_t message_count_ = 0;

        struct PendingAck {
            uint32_t crc = 0;
            uint32_t start_ms = 0;
        };

        PendingAck pending_acks_[8] = {};
        uint8_t ack_next_ = 0;

        static void group_callback(void *ctx, const mesh::Packet *, uint8_t channel, uint32_t time, const uint8_t *text, size_t len);
        static void peer_callback(void *ctx, const mesh::Packet *, const uint8_t pub[32], uint8_t type, uint32_t time, const uint8_t *text, size_t len);
        static void advert_callback(void *ctx, const mesh::Identity &, uint32_t time, const uint8_t *data, size_t len);
        static void ack_callback(void *ctx, uint32_t ack);

        static void raw_rx_callback(void *ctx, float snr, float rssi, const uint8_t *raw, size_t length);

        void on_raw_rx(float snr, float rssi, const uint8_t *raw, size_t length);


        void on_group_message(const mesh::Packet *, uint8_t channel, uint32_t time, const uint8_t *text, size_t len);
        void on_peer_message(const mesh::Packet *, const uint8_t pub[32], uint8_t type, uint32_t time, const uint8_t *text, size_t len);
        void on_advert(const mesh::Identity &, uint32_t time, const uint8_t *data, size_t len);
        void on_ack(uint32_t code);

        void enqueue(const uint8_t *data, size_t len);
        void pump_contacts();
        void handle(const uint8_t *command, size_t len);

        void reply_error(uint8_t code, const uint8_t *cmd, size_t len);
        void reply_ok();

        bool send_contact(const Contact &contact, uint8_t response_code = 3);
        bool save_name();
        bool update_settings(const Settings &next, bool reconfigure_radio);
        void handle_device_query();
        void handle_app_start();
        void handle_get_channel(uint8_t index);
    };

}
