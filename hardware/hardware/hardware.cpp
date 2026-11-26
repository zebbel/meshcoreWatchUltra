#include "Storage.h"
#include "hardware.h"
#include "board.h"
#include "Power.h"
#include "GPIO.h"

namespace hardware{
    static const char *TAG = "Hardware";

    esp_err_t init(){
        esp_err_t err = hardware::storage::init();
        if(err != ESP_OK){
            ESP_LOGE(TAG, "NVS unavailable: %s", esp_err_to_name(err));
            return err;
        }

        err = hardware::board::init();
        if(err == ESP_OK) err = hardware::power::init();
        if(err == ESP_OK) err = hardware::gpio::init();
        if(err != ESP_OK){
            ESP_LOGE(TAG, "Hardware init failed: %s", esp_err_to_name(err));
            return err;
        }
        return err;
    }
}