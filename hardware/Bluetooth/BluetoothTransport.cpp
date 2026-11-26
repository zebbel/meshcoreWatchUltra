#include "BluetoothTransport.h"

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "nimble/ble.h"
#include "host/ble_sm.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "host/ble_store.h"

#include <stdio.h>
#include <string.h>

#include "Storage.h"

extern "C" void ble_store_config_init(void);

namespace hardware::bluetooth {

    static const char *TAG = "Transport";
    static Transport *instance = nullptr;

    // Protocol-specific UUIDs are supplied by the caller and retained for NimBLE.
    static ble_uuid128_t service_uuid = {};
    static ble_uuid128_t rx_uuid = {};
    static ble_uuid128_t tx_uuid = {};
    // Fast initial discovery and notification pacing (milliseconds).
    static constexpr uint32_t kFastAdvDurationMs = 30000;
    static constexpr uint32_t kNotificationGapMs = 10;
    static constexpr uint32_t kNotifyRetryGapMs = 30;

    // -------------------------------------------------------------------------
    // NimBLE callbacks and singleton
    // -------------------------------------------------------------------------

    // NimBLE dispatches callbacks on its host task; the application owns the transport.
    int bluetooth_gap_callback(struct ble_gap_event *event, void *){
        return instance == nullptr ? 0 : instance->on_gap_event(event);
    }

    int bluetooth_access_callback(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *){
        return instance == nullptr ? BLE_ATT_ERR_UNLIKELY : instance->on_characteristic(conn, attr, ctxt);
    }

    void bluetooth_stack_sync(){
        if(instance != nullptr) instance->sync();
    }

    static void host_task(void *){
        nimble_port_run();
        nimble_port_freertos_deinit();
    }
    // Owned by Hardware; the application starts/polls it, clients borrow it.
    Transport &transport(){
        static Transport singleton;
        return singleton;
    }

    // -------------------------------------------------------------------------
    // Persistent pairing configuration
    // -------------------------------------------------------------------------

    // PIN zero selects a random six-digit passkey; other values are stored in NVS.
    esp_err_t Transport::load_pairing_pin(){
        hardware::storage::Handle h;
        esp_err_t err = hardware::storage::open("bluetooth", hardware::storage::Access::read_write, h);
        if(err != ESP_OK) return err;
        uint32_t value = 0;
        size_t length = sizeof(value);
        err = hardware::storage::read_blob(h, "pin", &value, &length);
        hardware::storage::close(h);
        if(hardware::storage::is_not_found(err)) return set_pairing_pin(0);
        if(err != ESP_OK) return err;
        if(length != sizeof(value) || (value != 0 && (value < 100000 || value > 999999))) return ESP_ERR_INVALID_CRC;
        configured_pin_.store(value);
        return ESP_OK;
    }

    esp_err_t Transport::set_pairing_pin(uint32_t pin){
        if(pin != 0 && (pin < 100000 || pin > 999999)) return ESP_ERR_INVALID_ARG;
        hardware::storage::Handle h;
        esp_err_t err = hardware::storage::open("bluetooth", hardware::storage::Access::read_write, h);
        if(err != ESP_OK) return err;
        err = hardware::storage::write_blob(h, "pin", &pin, sizeof(pin));
        if(err == ESP_OK) err = hardware::storage::commit(h);
        hardware::storage::close(h);
        if(err == ESP_OK) configured_pin_.store(pin);
        return err;
    }

    // -------------------------------------------------------------------------
    // Transport initialization
    // -------------------------------------------------------------------------

    // Create queues, register authenticated GATT characteristics and start the host task.
    esp_err_t Transport::begin(const char *name, const Uuids &uuids){
        if(started_ || instance != nullptr) return ESP_ERR_INVALID_STATE;
        if(name == nullptr) return ESP_ERR_INVALID_ARG;

        rx_queue_ = xQueueCreate(QUEUE_CAPACITY, sizeof(Frame));
        tx_queue_ = xQueueCreate(QUEUE_CAPACITY, sizeof(Frame));
        if(rx_queue_ == nullptr || tx_queue_ == nullptr) return ESP_ERR_NO_MEM;

        service_uuid.u.type = BLE_UUID_TYPE_128;
        rx_uuid.u.type = BLE_UUID_TYPE_128;
        tx_uuid.u.type = BLE_UUID_TYPE_128;
        memcpy(service_uuid.value, uuids.service, 16);
        memcpy(rx_uuid.value, uuids.rx, 16);
        memcpy(tx_uuid.value, uuids.tx, 16);

        snprintf(device_name_, sizeof(device_name_), "%s", name);
        instance = this;

        esp_err_t err = nimble_port_init();
        if(err != ESP_OK){
            instance = nullptr;
            return err;
        }

        ble_svc_gap_init();
        ble_svc_gatt_init();
        int rc = ble_svc_gap_device_name_set(device_name_);
        if(rc != 0){
            ESP_LOGE(TAG, "Setting BLE name failed: %d", rc);
            return ESP_FAIL;
        }

        // Use GATT write-with-response and notify; only an authenticated bond
        // may read or write companion protocol frames.
        static ble_gatt_chr_def chars[3] = {};
        chars[0].uuid = &rx_uuid.u;
        chars[0].access_cb = bluetooth_access_callback;
        chars[0].flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_AUTHEN;
        chars[1].uuid = &tx_uuid.u;
        chars[1].access_cb = bluetooth_access_callback;
        chars[1].flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ_AUTHEN;
        chars[1].val_handle = &tx_handle_;
        static ble_gatt_svc_def services[2] = {};
        services[0].type = BLE_GATT_SVC_TYPE_PRIMARY;
        services[0].uuid = &service_uuid.u;
        services[0].characteristics = chars;

        rc = ble_gatts_count_cfg(services);
        if(rc == 0) rc = ble_gatts_add_svcs(services);
        if(rc != 0){
            ESP_LOGE(TAG, "Registering GATT service failed: %d", rc);
            return ESP_FAIL;
        }

        ble_hs_cfg.sync_cb = bluetooth_stack_sync;
        ble_hs_cfg.reset_cb = nullptr;
        ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
        ble_hs_cfg.sm_io_cap = BLE_HS_IO_DISPLAY_ONLY;
        ble_hs_cfg.sm_bonding = 1;
        ble_hs_cfg.sm_mitm = 1;
        ble_hs_cfg.sm_sc = 1;
        ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
        ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
        ble_store_config_init();
        ble_att_set_preferred_mtu(517);
        started_ = true;
        nimble_port_freertos_init(host_task);
        ESP_LOGI(TAG, "Bluetooth GATT service started: %s", device_name_);
        return ESP_OK;
    }

    // -------------------------------------------------------------------------
    // BLE identity and advertising
    // -------------------------------------------------------------------------

    void Transport::sync(){
        // Prefer the ESP32-S3 factory public BLE address.
        // Unlike a generated random static address, it is stable
        // across firmware updates, reboots and power cycles.
        int rc = ble_hs_util_ensure_addr(0);

        if(rc == 0){
            rc = ble_hs_id_infer_auto(0, &own_addr_type_);
        }

        if(rc != 0){
            ESP_LOGE(TAG, "No usable BLE address, rc=%d", rc);
            return;
        }

        if(own_addr_type_ != BLE_OWN_ADDR_PUBLIC){
            ESP_LOGW(TAG, "Public BLE address unavailable; using random identity");
        }

        advertise(true);
    }

    void Transport::advertise(bool fast){
        ble_hs_adv_fields adv = {};
        adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
        adv.uuids128 = const_cast<ble_uuid128_t *>(&service_uuid);
        adv.num_uuids128 = 1;
        adv.uuids128_is_complete = 1;
        int rc = ble_gap_adv_set_fields(&adv);

        ble_hs_adv_fields scan = {};
        scan.name = reinterpret_cast<uint8_t *>(device_name_);
        scan.name_len = strlen(device_name_);
        scan.name_is_complete = 1;
        if(rc == 0) rc = ble_gap_adv_rsp_set_fields(&scan);

        ble_gap_adv_params params = {};
        params.conn_mode = BLE_GAP_CONN_MODE_UND;
        params.disc_mode = BLE_GAP_DISC_MODE_GEN;
        // 100-150 ms for 30 seconds; then 250-350 ms to save power.
        // Units are 0.625 ms. A new disconnect restarts the fast window.
        params.itvl_min = fast ? 160 : 400;
        params.itvl_max = fast ? 240 : 560;
        if(rc == 0){
            rc = ble_gap_adv_start(own_addr_type_, nullptr, fast ? kFastAdvDurationMs : BLE_HS_FOREVER, &params, bluetooth_gap_callback, nullptr);
        }
        if(rc != 0) ESP_LOGE(TAG, "Advertising failed: %d", rc);
    }

    // -------------------------------------------------------------------------
    // Connection lifecycle and security events
    // -------------------------------------------------------------------------

    int Transport::on_gap_event(struct ble_gap_event *event){
        switch(event->type){
            case BLE_GAP_EVENT_CONNECT:
                if(event->connect.status == 0){
                    connection_handle_.store(event->connect.conn_handle);
                    authenticated_.store(false);
                    subscribed_.store(false);
                    ESP_LOGI(TAG, "BLE connected; starting authenticated pairing");
                    wake_main_task();
                    int rc = ble_gap_security_initiate(event->connect.conn_handle);
                    if(rc != 0) ESP_LOGW(TAG, "Security initiation: %d", rc);
                }else{
                    advertise(true);
                }
                break;
            case BLE_GAP_EVENT_DISCONNECT:
                connection_handle_.store(0xFFFF);
                authenticated_.store(false);
                subscribed_.store(false);
                active_pairing_pin_.store(0);
                xQueueReset(rx_queue_);
                xQueueReset(tx_queue_);
                wake_main_task();
                advertise(true);
                break;
            case BLE_GAP_EVENT_ADV_COMPLETE:
                // The 30-second fast-advertising window expired.
                if(!link_active()) advertise(false);
                break;
            case BLE_GAP_EVENT_CONN_UPDATE:
                if(event->conn_update.status != 0) ESP_LOGW(TAG, "BLE connection parameter update status=%d", event->conn_update.status);
                break;
            case BLE_GAP_EVENT_SUBSCRIBE:
                if(event->subscribe.attr_handle == tx_handle_){
                    subscribed_.store(event->subscribe.cur_notify != 0);
                    wake_main_task();
                }
                break;
            case BLE_GAP_EVENT_PASSKEY_ACTION:
                handle_passkey(event);
                break;
            case BLE_GAP_EVENT_ENC_CHANGE:
                handle_encryption_change(event);
                break;
            case BLE_GAP_EVENT_REPEAT_PAIRING:
                return handle_repeat_pairing(event);
            default:
                break;
        }
        return 0;
    }

    // -------------------------------------------------------------------------
    // Pairing and authentication handlers
    // -------------------------------------------------------------------------

    void Transport::handle_passkey(struct ble_gap_event *event){
        if(event->passkey.params.action == BLE_SM_IOACT_DISP){
            ble_sm_io io = {};
            io.action = BLE_SM_IOACT_DISP;

            const uint32_t fixed = configured_pin_.load();
            io.passkey = fixed ? fixed : 100000 + (esp_random() % 900000);

            int rc = ble_sm_inject_io(event->passkey.conn_handle, &io);

            if(rc == 0){
                active_pairing_pin_.store(io.passkey);
            }else{
                active_pairing_pin_.store(0);
                ESP_LOGE(TAG, "Passkey injection failed: %d", rc);
            }

            wake_main_task();
        }
    }

    void Transport::handle_encryption_change(struct ble_gap_event *event){
        active_pairing_pin_.store(0);
        wake_main_task();

        ble_gap_conn_desc desc = {};
        if(event->enc_change.status == 0 && ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0 && desc.sec_state.encrypted && desc.sec_state.authenticated && desc.sec_state.bonded){
            authenticated_.store(true);
            ESP_LOGI(TAG, "Authenticated BLE bond established");
            wake_main_task();
            // Request 15-30 ms for responsive commands and contact sync.
            // The central (phone) may negotiate a longer interval.
            ble_gap_upd_params params = {};
            params.itvl_min = 12; // 15 ms (1.25 ms units)
            params.itvl_max = 24; // 30 ms
            params.latency = 0;
            params.supervision_timeout = 400; // 4 s (10 ms units)
            const int update_rc = ble_gap_update_params(event->enc_change.conn_handle, &params);
            if(update_rc != 0) {
                ESP_LOGW(TAG, "BLE power-saving interval request rejected: %d", update_rc);
            }
        }else{
            authenticated_.store(false);
            ESP_LOGW(TAG, "BLE authentication not established");
            ble_gap_terminate(event->enc_change.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
    }

    int Transport::handle_repeat_pairing(struct ble_gap_event *event){
        // The peer is requesting pairing although a bond
        // already exists in local storage.
        //
        // Remove only this peer's old security information.
        // Do not clear all BLE bonds or erase NVS.

        ble_gap_conn_desc desc = {};

        int rc = ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);

        if(rc != 0){
            ESP_LOGE(TAG, "Repeat pairing: connection lookup failed, rc=%d", rc);
            return BLE_GAP_REPEAT_PAIRING_IGNORE;
        }

        ESP_LOGW(TAG, "Repeat pairing requested; replacing stale peer bond");

        rc = ble_store_util_delete_peer(&desc.peer_id_addr);

        if(rc != 0){
            ESP_LOGE(TAG, "Failed to remove stale peer bond, rc=%d", rc);
            return BLE_GAP_REPEAT_PAIRING_IGNORE;
        }

        // Keep MITM protection and Secure Connections enabled.
        // A fresh authenticated pairing is still required.
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    // -------------------------------------------------------------------------
    // Incoming GATT frames
    // -------------------------------------------------------------------------

    // One authenticated GATT write corresponds to one queued protocol frame.
    int Transport::on_characteristic(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt){
        (void)attr;
        if(!authenticated_.load() || conn != connection_handle_.load()){
            return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
        }
        if(ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR){
            const int size = OS_MBUF_PKTLEN(ctxt->om);
            if(size < 1 || size > static_cast<int>(MAX_FRAME)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            Frame frame;
            frame.length = size;
            if(ble_hs_mbuf_to_flat(ctxt->om, frame.data, size, nullptr) != 0){
                return BLE_ATT_ERR_UNLIKELY;
            }
            if(xQueueSend(rx_queue_, &frame, 0) != pdTRUE){
                return BLE_ATT_ERR_INSUFFICIENT_RES;
            }
            // Request fast polling only during active traffic, not for the entire BLE link.
            last_activity_ms_.store(static_cast<uint32_t>(esp_timer_get_time() / 1000ULL), std::memory_order_relaxed);
            wake_main_task();
            return 0;
        }
        if(ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR){
            return 0;
        }
        return BLE_ATT_ERR_UNLIKELY;
    }

    // -------------------------------------------------------------------------
    // Application RX/TX queue interface
    // -------------------------------------------------------------------------

    bool Transport::receive(Frame &frame){
        return xQueueReceive(rx_queue_, &frame, 0) == pdTRUE;
    }

    bool Transport::send(const uint8_t *data, size_t length){
        if(data == nullptr || length == 0 || length > MAX_FRAME) return false;
        if(!authenticated_.load() || !subscribed_.load()) return false;
        Frame frame;
        frame.length = length;
        memcpy(frame.data, data, length);
        const bool queued = xQueueSend(tx_queue_, &frame, 0) == pdTRUE;
        if(queued) last_activity_ms_.store(static_cast<uint32_t>(esp_timer_get_time() / 1000ULL), std::memory_order_relaxed);
        return queued;
    }

    bool Transport::send_best_effort(const uint8_t *data, size_t length){
        if(data == nullptr || length == 0 || length > MAX_FRAME) return false;

        if(tx_queue_ == nullptr) return false;

        // Do not enqueue frames that cannot be sent
        // with the currently negotiated BLE MTU.
        // An oversized frame must never block the queue.
        if(!can_send(length)) return false;

        // Preserve at least six queue slots for
        // high-priority protocol responses.
        if(uxQueueSpacesAvailable(tx_queue_) <= 6) return false;

        Frame frame = {};

        frame.length = static_cast<uint16_t>(length);

        memcpy(frame.data, data, length);

        return xQueueSend(tx_queue_, &frame, 0) == pdTRUE;
    }

    bool Transport::can_send(size_t length) const{
        if(!authenticated_.load() || !subscribed_.load() || length > MAX_FRAME) return false;
        ble_gap_conn_desc desc = {};
        uint16_t conn = connection_handle_.load();
        if(conn == 0xFFFF || ble_gap_conn_find(conn, &desc) != 0) return false;
        return desc.sec_state.encrypted && desc.sec_state.authenticated && desc.sec_state.bonded && ble_att_mtu(conn) >= length + 3;
    }

    // -------------------------------------------------------------------------
    // Application task wakeups and activity tracking
    // -------------------------------------------------------------------------

    bool Transport::recent_activity() const {
        if(!link_active()) return false;
        const uint32_t last = last_activity_ms_.load(std::memory_order_relaxed);
        if(last == 0) return false;
        const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
        return static_cast<uint32_t>(now - last) < 500U;
    }

    void Transport::set_wakeup_task(TaskHandle_t task){
        wakeup_task_.store(task, std::memory_order_release);
    }

    void Transport::wake_main_task(){
        // NimBLE callbacks run in task context, not GPIO ISR context.
        const TaskHandle_t task = wakeup_task_.load(std::memory_order_acquire);
        if(task != nullptr) xTaskNotifyGive(task);
    }

    // -------------------------------------------------------------------------
    // Outgoing notifications
    // -------------------------------------------------------------------------

    // Peek until transmission succeeds; retry with backoff without dropping the frame.
    void Transport::loop(){
        if(!authenticated_.load() || !subscribed_.load()) return;
        const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
        // Success pacing targets 10 ms; rejected sends back off to 30 ms.
        if(last_notification_ms_ != 0 && static_cast<uint32_t>(now - last_notification_ms_) < notification_gap_ms_) return;
        Frame frame;
        if(xQueuePeek(tx_queue_, &frame, 0) != pdTRUE) return;
        if(!can_send(frame.length)) return;
        auto mbuf = ble_hs_mbuf_from_flat(frame.data, frame.length);
        if(mbuf == nullptr){
            notification_gap_ms_ = kNotifyRetryGapMs;
            last_notification_ms_ = now;
            ++tx_retries_;
            return;
        }
        // NimBLE consumes the mbuf even when notification fails.
        const int rc = ble_gatts_notify_custom(connection_handle_.load(), tx_handle_, mbuf);
        last_notification_ms_ = now;
        if(rc == 0){
            xQueueReceive(tx_queue_, &frame, 0);
            notification_gap_ms_ = kNotificationGapMs;
            tx_retries_ = 0;
        }else{
            notification_gap_ms_ = kNotifyRetryGapMs;
            ++tx_retries_;
            if(tx_retries_ == 1 || (tx_retries_ % 32) == 0) ESP_LOGW(TAG, "BLE notify backpressure: rc=%d, retries=%lu", rc, static_cast<unsigned long>(tx_retries_));
        }
    }
}
