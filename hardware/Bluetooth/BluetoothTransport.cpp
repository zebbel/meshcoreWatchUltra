#include "BluetoothTransport.h"

#include <stdio.h>
#include <string.h>
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

extern "C" void ble_store_config_init(void);

namespace hardware::bluetooth {

    static const char *TAG = "Transport";
    static Transport *instance = nullptr;

    // UUIDs are passed in by the protocol owner.
    static ble_uuid128_t service_uuid = {};
    static ble_uuid128_t rx_uuid = {};
    static ble_uuid128_t tx_uuid = {};

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


    Transport::Transport(){}

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
        int bond_count = -1;

        int store_rc = ble_store_util_count(BLE_STORE_OBJ_TYPE_PEER_SEC, &bond_count);

        ESP_LOGI(TAG, "BLE bond storage: rc=%d stored_bonds=%d", store_rc, bond_count);
        ble_att_set_preferred_mtu(517);
        started_ = true;
        nimble_port_freertos_init(host_task);
        ESP_LOGI(TAG, "Bluetooth GATT service started: %s", device_name_);
        return ESP_OK;
    }

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

        uint8_t address[6] = {};

        rc = ble_hs_id_copy_addr(own_addr_type_, address, nullptr);

        if(rc == 0){
            ESP_LOGI(TAG, "BLE identity type=%u address=%02X:%02X:%02X:%02X:%02X:%02X", static_cast<unsigned>(own_addr_type_), address[5], address[4], address[3], address[2], address[1], address[0]);
        }

        if(own_addr_type_ != BLE_OWN_ADDR_PUBLIC){
            ESP_LOGW(TAG, "Public BLE address unavailable; using random identity");
        }

        advertise();
    }

    void Transport::advertise(){
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
        if(rc == 0){
            rc = ble_gap_adv_start(own_addr_type_, nullptr, BLE_HS_FOREVER, &params, bluetooth_gap_callback, nullptr);
        }
        if(rc != 0) ESP_LOGE(TAG, "Advertising failed: %d", rc);
    }

    int Transport::on_gap_event(struct ble_gap_event *event){
        switch(event->type){
            case BLE_GAP_EVENT_CONNECT:
                if(event->connect.status == 0){
                    connection_handle_.store(event->connect.conn_handle);
                    authenticated_.store(false);
                    subscribed_.store(false);
                    ESP_LOGI(TAG, "BLE connected; starting authenticated pairing");
                    int rc = ble_gap_security_initiate(event->connect.conn_handle);
                    if(rc != 0) ESP_LOGW(TAG, "Security initiation: %d", rc);
                }else{
                    advertise();
                }
                break;
            case BLE_GAP_EVENT_DISCONNECT:
                connection_handle_.store(0xFFFF);
                authenticated_.store(false);
                subscribed_.store(false);
                clear_pin_.store(true);
                xQueueReset(rx_queue_);
                xQueueReset(tx_queue_);
                advertise();
                break;
            case BLE_GAP_EVENT_ADV_COMPLETE:
                advertise();
                break;
            case BLE_GAP_EVENT_SUBSCRIBE:
                if(event->subscribe.attr_handle == tx_handle_){
                    subscribed_.store(event->subscribe.cur_notify != 0);
                }
                break;
            case BLE_GAP_EVENT_PASSKEY_ACTION:
                if(event->passkey.params.action == BLE_SM_IOACT_DISP){
                    ble_sm_io io = {};
                    io.action = BLE_SM_IOACT_DISP;
                    const uint32_t fixed = configured_pin_.load();
                    io.passkey = fixed ? fixed : 100000 + (esp_random() % 900000);
                    pending_pin_.store(io.passkey);
                    int rc = ble_sm_inject_io(event->passkey.conn_handle, &io);
                    if(rc != 0) ESP_LOGE(TAG, "Passkey injection failed: %d", rc);
                }
                break;
            case BLE_GAP_EVENT_ENC_CHANGE: {
                ble_gap_conn_desc security_desc = {};

                int find_rc = ble_gap_conn_find(event->enc_change.conn_handle, &security_desc);

                ESP_LOGI(TAG, "BLE security: status=%d lookup=%d encrypted=%d authenticated=%d bonded=%d", event->enc_change.status, find_rc, find_rc == 0 ? security_desc.sec_state.encrypted : 0, find_rc == 0 ? security_desc.sec_state.authenticated : 0, find_rc == 0 ? security_desc.sec_state.bonded : 0);
                ble_gap_conn_desc desc = {};
                if(event->enc_change.status == 0 && ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0 && desc.sec_state.encrypted && desc.sec_state.authenticated && desc.sec_state.bonded){
                    authenticated_.store(true);
                    clear_pin_.store(true);
                    ESP_LOGI(TAG, "Authenticated BLE bond established");
                }else{
                    authenticated_.store(false);
                    ESP_LOGW(TAG, "BLE authentication not established");
                    ble_gap_terminate(event->enc_change.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
                }
                break;
            }       
            case BLE_GAP_EVENT_REPEAT_PAIRING: {
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
            default:
                break;
        }
        return 0;
    }

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
            return 0;
        }
        if(ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR){
            return 0;
        }
        return BLE_ATT_ERR_UNLIKELY;
    }

    bool Transport::receive(Frame &frame){
        return xQueueReceive(rx_queue_, &frame, 0) == pdTRUE;
    }

    bool Transport::send(const uint8_t *data, size_t length){
        if(data == nullptr || length == 0 || length > MAX_FRAME) return false;
        if(!authenticated_.load() || !subscribed_.load()) return false;
        Frame frame;
        frame.length = length;
        memcpy(frame.data, data, length);
        return xQueueSend(tx_queue_, &frame, 0) == pdTRUE;
    }

    bool Transport::send_best_effort(const uint8_t *data, size_t length){
        if(data == nullptr || length == 0 || length > MAX_FRAME)
            return false;

        if(tx_queue_ == nullptr)
            return false;

        // Do not enqueue frames that cannot be sent
        // with the currently negotiated BLE MTU.
        // An oversized frame must never block the queue.
        if(!can_send(length))
            return false;

        // Preserve at least six queue slots for
        // high-priority protocol responses.
        if(uxQueueSpacesAvailable(tx_queue_) <= 6)
            return false;

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

    void Transport::loop(){
        if(!authenticated_.load() || !subscribed_.load()) return;
        uint32_t now = esp_timer_get_time() / 1000ULL;
        if(now - last_notification_ms_ < 60) return;
        Frame frame;
        if(xQueuePeek(tx_queue_, &frame, 0) != pdTRUE) return;
        if(!can_send(frame.length)) return;
        auto mbuf = ble_hs_mbuf_from_flat(frame.data, frame.length);
        if(mbuf == nullptr) return;
        int rc = ble_gatts_notify_custom(connection_handle_.load(), tx_handle_, mbuf);
        if(rc == 0){
            xQueueReceive(tx_queue_, &frame, 0);
            last_notification_ms_ = now;
        }else{
            ESP_LOGW(TAG, "BLE notify retry: %d", rc);
        }
    }

    uint32_t Transport::take_pairing_pin(){ return pending_pin_.exchange(0); }
    bool Transport::take_pairing_clear_event(){ return clear_pin_.exchange(false); }

}
