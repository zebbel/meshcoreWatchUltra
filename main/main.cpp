
#include <stdio.h>

#include "esp_log.h"
#include "esp_heap_caps.h"

#include "board.h"
#include "board_pins.h"
#include "power.h"

static const char *TAG = "MeshCoreWatch";

extern "C" void app_main(void){
    ESP_LOGI(TAG, "LilyGO T-Watch Ultra started");

    // Initialize board
    esp_err_t err = board::init();
    // Initialize power
    err = power::init();
    // Apply power configuration
    err = power::configure();

    if(err != ESP_OK){
        ESP_LOGE(TAG, "Initialization failed");
        return;
    }
}
