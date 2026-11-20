
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board.h"
#include "AXP2101.h"
#include "XL9555.h"
#include "SX1262.h"
#include "MeshCoreRadio.h"
#include "MeshCorePlatform.h"
#include "WatchRouting.h"

static const char *TAG = "MeshCoreWatch";

extern "C" void app_main(void){
    ESP_LOGI(TAG, "LilyGO T-Watch Ultra started");

    esp_err_t err = board::init();
    if(err != ESP_OK){
        ESP_LOGE(TAG, "Board initialization failed: %s", esp_err_to_name(err));
        return;
    }

    err = AXP2101::init();
    if(err != ESP_OK){
        ESP_LOGE(TAG, "AXP2101 initialization failed: %s", esp_err_to_name(err));
        return;
    }

    err = AXP2101::configure();
    if(err != ESP_OK){
        ESP_LOGE(TAG, "AXP2101 configuration failed: %s", esp_err_to_name(err));
        return;
    }

    err = XL9555::init();
    if(err != ESP_OK){
        ESP_LOGE(TAG, "XL9555 initialization failed: %s", esp_err_to_name(err));
        return;
    }

    err = XL9555::configure();
    if(err != ESP_OK){
        ESP_LOGE(TAG, "XL9555 configuration failed: %s", esp_err_to_name(err));
        return;
    }

    err = sx1262::init();
    if(err != ESP_OK){
        ESP_LOGE(TAG, "SX1262 initialization failed: %s", esp_err_to_name(err));
        return;
    }

    // MeshCore EU/UK Narrow radio settings
    sx1262::config_t config;
    config.frequency_hz = 869618000;
    config.bandwidth_hz = 62500;
    config.spreading_factor = 8;
    config.coding_rate = 8;
    config.sync_word = 0x12;
    config.preamble_symbols = 32;
    config.crc_enabled = true;
    config.tx_power_dbm = 22;

    err = sx1262::configure(config);
    if(err != ESP_OK){
        ESP_LOGE(TAG, "LoRa configuration failed: %s", esp_err_to_name(err));
        return;
    }

    // Static lifetime keeps the packet table out of the app_main task stack.
    static watch::MeshCoreRadio physical_radio(config);
    static watch::ReceiveOnlyRadio radio(physical_radio);
    static watch::WatchClock clock;
    static StaticPoolPacketManager manager(24);
    static SimpleMeshTables seen;
    static watch::WatchRouting routing(radio, clock, manager, seen);

    routing.begin();

    if(!radio.isInRecvMode()){
        ESP_LOGE(TAG, "MeshCore Dispatcher could not start radio RX");
        return;
    }

    ESP_LOGI(TAG, "Upstream MeshCore Dispatcher running (RX-only, no forwarding)");

    uint32_t next_report = static_cast<uint32_t>(clock.getMillis()) + 30000;

    while(true){
        routing.loop();

        uint32_t now = static_cast<uint32_t>(clock.getMillis());

        if(static_cast<int32_t>(now - next_report) >= 0){
            ESP_LOGI(TAG, "Dispatcher: unique=%lu duplicates=%lu flood=%lu direct=%lu pool_free=%d out=%d",
                     static_cast<unsigned long>(routing.uniqueCount()),
                     static_cast<unsigned long>(routing.duplicateCount()),
                     static_cast<unsigned long>(routing.getNumRecvFlood()),
                     static_cast<unsigned long>(routing.getNumRecvDirect()),
                     manager.getFreeCount(),
                     manager.getOutboundTotal());

            next_report = now + 30000;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
