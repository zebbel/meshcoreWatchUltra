#pragma once

#include <atomic>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

struct ble_gap_event;
struct ble_gatt_access_ctxt;

namespace hardware::bluetooth {

    // Exact MeshCore BLE NUS frame: one GATT write or notification = one frame.
    // Does not use USB, Wi-Fi, or length-prefix framing.
    struct Uuids { uint8_t service[16] = {}; uint8_t rx[16] = {}; uint8_t tx[16] = {}; };
    // One process-wide BLE transport. Main owns lifecycle; MeshCore only uses frames.
    class Transport;
    Transport &transport();


    class Transport {
    public:
        static constexpr size_t MAX_FRAME = 258;

        struct Frame {
            uint16_t length = 0;
            uint8_t data[MAX_FRAME] = {};
        };

        Transport();
        esp_err_t begin(const char *name, const Uuids &uuids);
        bool receive(Frame &frame);
        bool send(const uint8_t *data, size_t length);
        bool send_best_effort(const uint8_t *data, size_t length);
        void loop();
        bool connected() const { return authenticated_.load(); }
        bool can_send(size_t length) const;

        // Events are consumed from the app_main task, never drawing on NimBLE's host thread.
        void set_pairing_pin(uint32_t pin){ configured_pin_.store(pin); }
        uint32_t take_pairing_pin();
        bool take_pairing_clear_event();

    private:
        friend int bluetooth_gap_callback(struct ble_gap_event *, void *);
        friend int bluetooth_access_callback(uint16_t, uint16_t, struct ble_gatt_access_ctxt *, void *);
        friend void bluetooth_stack_sync();

        static constexpr int QUEUE_CAPACITY = 12;
        QueueHandle_t rx_queue_ = nullptr;
        QueueHandle_t tx_queue_ = nullptr;
        std::atomic<bool> authenticated_{false};
        std::atomic<bool> subscribed_{false};
        std::atomic<uint32_t> pending_pin_{0};
        std::atomic<uint32_t> configured_pin_{0};
        std::atomic<bool> clear_pin_{false};
        std::atomic<uint16_t> connection_handle_{0xFFFF};
        uint16_t tx_handle_ = 0;
        uint8_t own_addr_type_ = 0;
        uint32_t last_notification_ms_ = 0;
        char device_name_[32] = {};
        bool started_ = false;

        int on_gap_event(struct ble_gap_event *event);
        int on_characteristic(uint16_t connection_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt);
        void sync();
        void advertise();
        void host_loop();
    };

}
