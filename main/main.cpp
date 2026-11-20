
#include <stdio.h>

#include "esp_log.h"
#include "esp_heap_caps.h"

#include "board.h"
#include "board_pins.h"
#include "AXP2101.h"
#include "XL9555.h"

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

    // Read current expander state for the first hardware test
    err = XL9555::log_state();

    if(err != ESP_OK){
        ESP_LOGE(TAG, "XL9555 hardware test failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "Hardware initialization complete");
}
