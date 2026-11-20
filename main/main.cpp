
#include <stdio.h>

#include "esp_log.h"
#include "esp_heap_caps.h"

#include "board.h"
#include "board_pins.h"

static const char *TAG = "MeshCoreWatch";

static void test_i2c_device(const char *name, uint8_t address){
    esp_err_t err = board::probe_i2c(address);

    if(err == ESP_OK){
        ESP_LOGI(TAG, "%s detected at 0x%02X", name, address);
    }
    else if(err == ESP_ERR_NOT_FOUND){
        ESP_LOGW(TAG, "%s not detected at 0x%02X", name, address);
    }
    else {
        ESP_LOGE(TAG, "%s probe failed: %s", name, esp_err_to_name(err));
    }
}

extern "C" void app_main(void){
    ESP_LOGI(TAG, "LilyGO T-Watch Ultra started");

    // Test PSRAM
    size_t psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);

    ESP_LOGI(TAG, "PSRAM detected: %u bytes", (unsigned int)psram);

    // Initialize board
    esp_err_t err = board::init();

    if(err != ESP_OK){
        ESP_LOGE(TAG, "Board initialization failed");
        return;
    }

    // Probe known I2C devices
    ESP_LOGI(TAG, "Starting I2C hardware test");

    test_i2c_device("AXP2101", board::addresses::AXP2101);

    test_i2c_device("XL9555", board::addresses::XL9555);

    test_i2c_device("RTC", board::addresses::RTC);

    ESP_LOGI(TAG, "Hardware test complete");
}
