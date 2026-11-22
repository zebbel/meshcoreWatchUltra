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
#include "SDStorage.h"
#include "helpers/SimpleMeshTables.h"
#include "helpers/StaticPoolPacketManager.h"

namespace meshcore {

    static const char *TAG = "MeshCorePort";
    static bool initialized = false;
    static bool init_attempted = false;
    static bool prepared = false;
    static meshcore::RadioAdapter *prepared_radio = nullptr;
    static meshcore::Node *node = nullptr;
    static meshcore::Companion *companion = nullptr;
    static meshcore::SettingsStore *settings_store = nullptr;

    // All SD reads and first-boot writes happen before SX126x initialization.
    // Nested SD sessions in IdentityStore/Contacts/Node/Companion reuse this
    // mount, and the outer scope unmounts/powers off exactly once.
    esp_err_t prepare_storage(){
        if(prepared) return ESP_OK;
        if(init_attempted) return ESP_ERR_INVALID_STATE;
        init_attempted = true;

        static meshcore::RandomAdapter rng;
        static mesh::LocalIdentity identity;
        static meshcore::SettingsStore settings;
        static meshcore::Contacts contacts;
        static meshcore::ClockAdapter clock;
        static meshcore::RTCAdapter rtc;
        static StaticPoolPacketManager packet_manager(24);
        static SimpleMeshTables duplicate_table;

        {
            hardware::sd::Session boot_storage;
            if(!boot_storage.ok()) return boot_storage.error();

            hardware::system::enable_boot_random();
            const esp_err_t identity_err = meshcore::load_or_create_identity(identity, rng);
            hardware::system::disable_boot_random();
            if(identity_err != ESP_OK){
                ESP_LOGE(TAG, "Persistent identity unavailable: %s", esp_err_to_name(identity_err));
                return identity_err;
            }

            esp_err_t err = settings.load();
            if(err != ESP_OK){
                ESP_LOGE(TAG, "Stored settings invalid: %s", esp_err_to_name(err));
                return err;
            }
            err = contacts.load();
            if(err != ESP_OK){
                ESP_LOGE(TAG, "Contacts unavailable: %s", esp_err_to_name(err));
                return err;
            }

            // Constructors and channel/name preload do not issue radio SPI I/O.
            static meshcore::RadioAdapter radio(settings.get().radio());
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

            static meshcore::Telemetry telemetry;
            hardware::bluetooth::Transport &ble = hardware::bluetooth::transport();
            static meshcore::Companion companion_instance(ble, mesh_node, radio, settings, contacts, rtc, clock, telemetry);
            if(!companion_instance.storage_ready()) return ESP_ERR_INVALID_CRC;

            node = &mesh_node;
            companion = &companion_instance;
            settings_store = &settings;
            prepared_radio = &radio;
        }
        // Outer Session destructor has unmounted the card and disabled ALDO1.
        prepared = true;
        ESP_LOGI(TAG, "MeshCore persistence loaded using one SD power session");
        return ESP_OK;
    }

    esp_err_t init(){
        if(initialized) return ESP_OK;
        if(!prepared || node == nullptr || prepared_radio == nullptr || settings_store == nullptr)
            return ESP_ERR_INVALID_STATE;

        const hardware::lora::config_t config = settings_store->get().radio();
        esp_err_t err = hardware::lora::configure(config);
        if(err != ESP_OK){
            ESP_LOGE(TAG, "Radio configure failed: %s", esp_err_to_name(err));
            return err;
        }

        node->begin();
        if(!prepared_radio->isInRecvMode()){
            ESP_LOGE(TAG, "MeshCore radio RX did not start");
            return ESP_FAIL;
        }
        initialized = true;
        ESP_LOGI(TAG, "MeshCore node, companion protocol and telemetry initialized; Bluetooth managed by Main");
        return ESP_OK;
    }

    void loop(){
        if(!initialized) return;
        if(!hardware::sd::ready()){
            static bool reported = false;
            if(!reported){ ESP_LOGE(TAG, "SD removed/unavailable; MeshCore processing suspended until reboot"); reported = true; }
            return;
        }
        node->loop();
        companion->loop();
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
