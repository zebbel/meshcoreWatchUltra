#include "MeshCorePort.h"

#include "esp_log.h"
#include "HardwareSystem.h"

#include "LoRa.h"
#include "PlatformAdapters.h"
#include "RadioAdapter.h"
#include "Node.h"
#include "IdentityStore.h"
#include "BluetoothTransport.h"
#include "Companion.h"
#include "Contacts.h"
#include "Settings.h"
#include "Telemetry.h"
#include "helpers/SimpleMeshTables.h"
#include "helpers/StaticPoolPacketManager.h"

namespace meshcore {

    static const char *TAG = "MeshCorePort";
    static bool initialized = false;
    static bool init_attempted = false;
    static meshcore::Node *node = nullptr;
    static meshcore::Companion *companion = nullptr;
    static meshcore::SettingsStore *settings_store = nullptr;

    esp_err_t init(){
        if(initialized) return ESP_OK;
        if(init_attempted) return ESP_ERR_INVALID_STATE;
        init_attempted = true;

        static meshcore::RandomAdapter rng;
        static mesh::LocalIdentity identity;

        hardware::system::enable_boot_random();
        esp_err_t err = meshcore::load_or_create_identity(identity, rng);
        hardware::system::disable_boot_random();

        if(err != ESP_OK){
            ESP_LOGE(TAG, "Persistent identity unavailable: %s", esp_err_to_name(err));
            return err;
        }

        static meshcore::SettingsStore settings;
        err = settings.load();

        if(err != ESP_OK){
            ESP_LOGE(TAG, "Stored settings invalid: %s", esp_err_to_name(err));
            return err;
        }

        static meshcore::Contacts contacts;
        err = contacts.load();

        if(err != ESP_OK){
            ESP_LOGE(TAG, "Contacts unavailable: %s", esp_err_to_name(err));
            return err;
        }

        const hardware::lora::config_t config = settings.get().radio();
        err = hardware::lora::configure(config);

        if(err != ESP_OK){
            ESP_LOGE(TAG, "Radio configure failed: %s", esp_err_to_name(err));
            return err;
        }

        static meshcore::ClockAdapter clock;
        static meshcore::RTCAdapter rtc;
        static meshcore::RadioAdapter radio(config);
        static StaticPoolPacketManager packet_manager(24);
        static SimpleMeshTables duplicate_table;
        static meshcore::Node mesh_node(radio, clock, rng, rtc, packet_manager, duplicate_table);

        mesh_node.self_id = identity;
        mesh_node.set_contacts(&contacts);
        mesh_node.set_tuning(settings.get().rx_delay_milli, settings.get().airtime_factor_milli);
        mesh_node.set_path_hash_mode(settings.get().path_hash_mode);
        mesh_node.set_scope_key(settings.get().default_scope_key);

        err = mesh_node.load_channels();

        if(err != ESP_OK){
            ESP_LOGE(TAG, "Failed loading channels: %s", esp_err_to_name(err));
            return err;
        }

        mesh_node.begin();

        if(!radio.isInRecvMode()){
            ESP_LOGE(TAG, "MeshCore radio RX did not start");
            return ESP_FAIL;
        }

        static meshcore::Telemetry telemetry;
        hardware::bluetooth::Transport &ble = hardware::bluetooth::transport();
        static meshcore::Companion companion_instance(ble, mesh_node, radio, settings, contacts, rtc, clock, telemetry);
        node = &mesh_node;
        companion = &companion_instance;
        settings_store = &settings;
        initialized = true;

        ESP_LOGI(TAG, "MeshCore node, companion protocol and telemetry initialized; Bluetooth managed by Main");
        return ESP_OK;
    }

    void loop(){
        if(!initialized) return;
        node->loop();
        companion->loop();
    }

    // Compatibility only: persisted Settings (VERSION=1) still stores ble_pin.
    // Main passes this value into Bluetooth before BLE advertising starts.
    uint32_t legacy_ble_pin(){
        return settings_store == nullptr ? 0 : settings_store->get().ble_pin;
    }

    bool get_position(int32_t &latitude_micro, int32_t &longitude_micro){
        if(!initialized) return false;
        latitude_micro = settings_store->get().latitude_micro;
        longitude_micro = settings_store->get().longitude_micro;
        return true;
    }

    esp_err_t set_position(int32_t latitude_micro, int32_t longitude_micro){
        if(!initialized) return ESP_ERR_INVALID_STATE;
        meshcore::Settings next = settings_store->get();
        next.latitude_micro = latitude_micro;
        next.longitude_micro = longitude_micro;
        if(!next.valid()) return ESP_ERR_INVALID_ARG;
        return settings_store->save(next);
    }

}
