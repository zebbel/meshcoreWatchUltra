
#include "hardware.h"
#include "Storage.h"
#include "board.h"
#include "Power.h"
#include "GPIO.h"
#include "GPS.h"
#include "SDStorage.h"
#include "esp_log.h"

namespace hardware {

    static constexpr const char *TAG = "Hardware";

    esp_err_t init() {
        esp_err_t err = storage::init();

        if(err == ESP_OK) err = board::init();
        if(err == ESP_OK) err = power::init();
        if(err == ESP_OK) err = gpio::init();
        if(err != ESP_OK){
            ESP_LOGE(TAG, "Hardware init failed: %s", esp_err_to_name(err));
            return err;
        }

        // GPS initialization
        err = gps::init();
        if(err != ESP_OK) ESP_LOGW(TAG, "GPS unavailable: %s", esp_err_to_name(err));

        // Initialize SD storage subsystem.
        err = sd::init();
        if(err != ESP_OK) ESP_LOGE(TAG, "SD subsystem failed: %s", esp_err_to_name(err));

        return ESP_OK;
    }
}
