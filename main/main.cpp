
#include "esp_log.h"
#include "nvs_flash.h"
#include "bootloader_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board.h"
#include "AXP2101.h"
#include "MIA_M10Q.h"
#include "XL9555.h"
#include "CO5300.h"
#include "SX1262.h"

#include "MeshCorePlatform.h"
#include "MeshCoreRadio.h"
#include "WatchMesh.h"
#include "WatchIdentityStore.h"
#include "WatchBLE.h"
#include "WatchDisplay.h"
#include "WatchCompanion.h"
#include "WatchContacts.h"
#include "WatchSettings.h"
#include "WatchTelemetry.h"
#include "helpers/SimpleMeshTables.h"
#include "helpers/StaticPoolPacketManager.h"

static const char *TAG = "MeshCoreWatch";

static bool init_hardware(){
    esp_err_t err = board::init();

    if(err == ESP_OK) err = AXP2101::init();
    if(err == ESP_OK) err = AXP2101::configure();
    if(err == ESP_OK) err = XL9555::init();
    if(err == ESP_OK) err = XL9555::configure();
    if(err == ESP_OK) err = sx1262::init();

    if(err != ESP_OK){
        ESP_LOGE(TAG, "Hardware init failed: %s", esp_err_to_name(err));
        return false;
    }

    return true;
}

extern "C" void app_main(void){
    ESP_LOGI(TAG, "T-Watch Ultra: MeshCore BLE companion");

    // Keep all existing NVS contents intact.
    esp_err_t err = nvs_flash_init();

    if(err != ESP_OK){
        ESP_LOGE(TAG, "NVS unavailable: %s", esp_err_to_name(err));
        return;
    }

    err = nvs_flash_init_partition("contacts");

    if(err != ESP_OK){
        ESP_LOGE(TAG, "Contact NVS initialization failed: %s", esp_err_to_name(err));
        return;
    }

    static watch::WatchRNG rng;
    static mesh::LocalIdentity identity;

    bootloader_random_enable();
    err = watch::load_or_create_identity(identity, rng);
    bootloader_random_disable();

    if(err != ESP_OK){
        ESP_LOGE(TAG, "Persistent identity unavailable: %s", esp_err_to_name(err));
        return;
    }

    static watch::WatchSettingsStore settings;

    err = settings.load();

    if(err != ESP_OK){
        ESP_LOGE(TAG, "Stored settings invalid: %s", esp_err_to_name(err));
        return;
    }

    static watch::WatchContacts contacts;

    err = contacts.load();

    if(err != ESP_OK){
        ESP_LOGE(TAG, "Contacts unavailable: %s", esp_err_to_name(err));
        return;
    }

    if(!init_hardware())
        return;

    const sx1262::config_t config = settings.get().radio();

    err = sx1262::configure(config);

    if(err != ESP_OK){
        ESP_LOGE(TAG, "Radio configure failed: %s", esp_err_to_name(err));
        return;
    }

    err = watch::pairing_display_init();

    if(err != ESP_OK){
        ESP_LOGE(TAG, "Cannot display secure BLE pairing code: %s", esp_err_to_name(err));
        return;
    }

    static watch::WatchClock clock;
    static watch::WatchRTC rtc;
    static watch::MeshCoreRadio radio(config);
    static StaticPoolPacketManager packet_manager(24);
    static SimpleMeshTables duplicate_table;

    static watch::WatchMesh mesh(radio, clock, rng, rtc, packet_manager, duplicate_table);

    mesh.self_id = identity;
    mesh.set_contacts(&contacts);
    mesh.set_tuning(settings.get().rx_delay_milli, settings.get().airtime_factor_milli);
    mesh.set_path_hash_mode(settings.get().path_hash_mode);
    mesh.set_scope_key(settings.get().default_scope_key);

    err = mesh.load_channels();

    if(err != ESP_OK){
        ESP_LOGE(TAG, "Failed loading channels: %s", esp_err_to_name(err));
        return;
    }

    mesh.begin();

    if(!radio.isInRecvMode()){
        ESP_LOGE(TAG, "MeshCore radio RX did not start");
        return;
    }

    // Independent GNSS hardware component.
    err = MIA_M10Q::init();

    if(err != ESP_OK){
        ESP_LOGW(TAG, "MIA_M10Q unavailable: %s", esp_err_to_name(err));
    }

    static watch::WatchTelemetry telemetry;
    static watch::WatchBLE ble;

    static watch::WatchCompanion companion(ble, mesh, radio, settings, contacts, rtc, clock, telemetry);

    ble.set_pairing_pin(settings.get().ble_pin);

    err = ble.begin("MeshCore-Watch");

    if(err != ESP_OK){
        ESP_LOGE(TAG, "BLE start failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "MeshCore node running; BLE companion and telemetry initialized");

    uint32_t next_log = static_cast<uint32_t>(clock.getMillis()) + 30000;

    while(true){
        uint32_t pin = ble.take_pairing_pin();

        if(ble.take_clear_pairing_display()){
            watch::pairing_display_clear();
        }else if(pin != 0){
            watch::pairing_display_show(pin);
        }

        // GPS component handles its own power and UART.
        MIA_M10Q::loop();

        mesh.loop();
        companion.loop();

        uint32_t now = static_cast<uint32_t>(clock.getMillis());

        if(static_cast<int32_t>(now - next_log) >= 0){
            ESP_LOGI(TAG, "BLE paired=%d RX=%lu adverts=%lu pool=%d queued=%d", ble.connected(), static_cast<unsigned long>(mesh.packetCount()), static_cast<unsigned long>(mesh.validAdvertCount()), packet_manager.getFreeCount(), packet_manager.getOutboundTotal());

            next_log = now + 30000;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
