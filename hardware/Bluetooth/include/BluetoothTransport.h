#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <atomic>
#include <stddef.h>
#include <stdint.h>

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
        // wake app_main when new BLE frames arrive.
        void set_wakeup_task(TaskHandle_t task);
        bool link_active() const { return connection_handle_.load() != 0xFFFF; }
        // short fast-poll burst after RX/TX only.
        bool recent_activity() const;
        bool has_pending_tx() const {
            return tx_queue_ != nullptr && uxQueueMessagesWaiting(tx_queue_) != 0;
        }
        bool connected() const { return authenticated_.load(); }
        bool can_send(size_t length) const;

        // Events are consumed from the app_main task, never drawing on NimBLE's host thread.
        esp_err_t load_pairing_pin();
        esp_err_t set_pairing_pin(uint32_t pin);
        uint32_t pairing_pin() const { return configured_pin_.load(); }
        uint32_t active_pairing_pin() const {
            return active_pairing_pin_.load();
        }

    private:
        friend int bluetooth_gap_callback(struct ble_gap_event *, void *);
        friend int bluetooth_access_callback(uint16_t, uint16_t, struct ble_gatt_access_ctxt *, void *);
        friend void bluetooth_stack_sync();

        static constexpr int QUEUE_CAPACITY = 12;
        QueueHandle_t rx_queue_ = nullptr;
        QueueHandle_t tx_queue_ = nullptr;
        std::atomic<bool> authenticated_{false};
        std::atomic<bool> subscribed_{false};
        std::atomic<uint32_t> configured_pin_{0};
        std::atomic<uint16_t> connection_handle_{0xFFFF};
        std::atomic<TaskHandle_t> wakeup_task_{nullptr};
        std::atomic<uint32_t> last_activity_ms_{0};
        uint16_t tx_handle_ = 0;
        uint8_t own_addr_type_ = 0;
        uint32_t last_notification_ms_ = 0;
        uint32_t notification_gap_ms_ = 10;
        uint32_t last_stats_ms_ = 0;
        uint32_t tx_sent_ = 0;
        uint32_t tx_retries_ = 0;
        uint32_t advertising_started_ms_ = 0;
        char device_name_[32] = {};
        bool started_ = false;
        std::atomic<uint32_t> active_pairing_pin_{0};

        int on_gap_event(struct ble_gap_event *event);
        int on_characteristic(uint16_t connection_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt);
        void sync();
        void advertise(bool fast);
        void wake_main_task();
        void host_loop();
    };

}
