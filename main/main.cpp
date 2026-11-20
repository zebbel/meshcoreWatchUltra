
#include <stdio.h>

#include "esp_log.h"
#include "esp_heap_caps.h"

#include "board.h"
#include "board_pins.h"
#include "AXP2101.h"
#include "XL9555.h"
#include "CO5300.h"
#include "SX1262.h"

static const char *TAG = "MeshCoreWatch";

extern "C" void app_main(void){
    ESP_LOGI(TAG, "LilyGO T-Watch Ultra started");

    // Initialize board
    esp_err_t err = board::init();

    if(err != ESP_OK){
        ESP_LOGE(TAG, "Board initialization failed: %s", esp_err_to_name(err));
        return;
    }

    // Initialize power
    err = AXP2101::init();

    if(err != ESP_OK){
        ESP_LOGE(TAG, "AXP2101 initialization failed: %s", esp_err_to_name(err));
        return;
    }

    // Apply power configuration
    err = AXP2101::configure();

    if(err != ESP_OK){
        ESP_LOGE(TAG, "AXP2101 configuration failed: %s", esp_err_to_name(err));
        return;
    }

    // Initialize XL9555 GPIO expander without changing pin states
    err = XL9555::init();

    if(err != ESP_OK){
        ESP_LOGE(TAG, "XL9555 initialization failed: %s", esp_err_to_name(err));
        return;
    }

    // Configure XL9555 GPIO expander
    err = XL9555::configure();

    if(err != ESP_OK){
        ESP_LOGE(TAG, "XL9555 configuration failed: %s", esp_err_to_name(err));
        return;
    }

    // Initialize SX1262
    err = sx1262::init();

    if(err != ESP_OK){
        ESP_LOGE(TAG, "SX1262 initialization failed: %s", esp_err_to_name(err));
        return;
    }

    // MeshCore EU/UK Narrow radio preset
    sx1262::config_t radio_config;

    radio_config.frequency_hz = 869618000;
    radio_config.bandwidth_hz = 62500;
    radio_config.spreading_factor = 8;
    radio_config.coding_rate = 8;
    radio_config.sync_word = 0x12;
    radio_config.preamble_symbols = 32;
    radio_config.crc_enabled = true;
    radio_config.tx_power_dbm = 22;

    err = sx1262::configure(radio_config);

    if(err != ESP_OK){
        ESP_LOGE(TAG, "LoRa configuration failed: %s", esp_err_to_name(err));
        return;
    }

    // Start receiving
    err = sx1262::start_receive();

    if(err != ESP_OK){
        ESP_LOGE(TAG, "LoRa receive startup failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "SX1262 ready for LoRa reception");

    while(true){
        sx1262::packet_t packet;
        err = sx1262::receive(packet, 1000);

        if(err == ESP_OK){
            ESP_LOGI(TAG, "LoRa RX: %u bytes, RSSI %.1f dBm, SNR %.1f dB", packet.length, packet.rssi_dbm, packet.snr_db);
            ESP_LOG_BUFFER_HEX(TAG, packet.data, packet.length);
        }else if(err != ESP_ERR_TIMEOUT){
            ESP_LOGW(TAG, "LoRa receive error: %s", esp_err_to_name(err));
        }
    }



    ESP_LOGI(TAG, "Hardware initialization complete");
}
