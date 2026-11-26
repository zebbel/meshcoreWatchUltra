#include "Storage.h"
#include "hardware.h"
#include "board.h"
#include "Power.h"
#include "GPIO.h"
#include "WatchDisplay.h"
#include "TimeService.h"
#include "MIA_M10Q.h"
#include "Storage.h"
#include "GPS.h"
#include "SDStorage.h"
#include "MeshCorePort.h"
#include "LoRa.h"
#include "Companion.h"

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

        // init display
        err = ui::watch_display::init();
        if(err != ESP_OK) ESP_LOGW(TAG, "Watch display unavailable: %s", esp_err_to_name(err));

        // Restore persistent UTC before MeshCore creates its RTC adapter.
        err = services::time_service::initialize_utc_clock();
        if(err != ESP_OK) ESP_LOGW(TAG, "Hardware RTC unavailable: %s", esp_err_to_name(err));
        hardware::gps::set_utc_callback(services::time_service::sync_gps_utc);

        // init GPS
        err = hardware::gps::init();
        if(err != ESP_OK) ESP_LOGW(TAG, "MIA_M10Q unavailable: %s", esp_err_to_name(err));

        // init SD card
        err = hardware::sd::init();
        if(err != ESP_OK) ESP_LOGE(TAG, "SD subsystem failed: %s", esp_err_to_name(err));
        if(err == ESP_OK){
            // Single mount: preload identity, settings, contacts, channels and
            // node name, then power off SD BEFORE initializing the LoRa radio.
            err = meshcore::prepare_storage();
            if(err == ESP_OK) err = hardware::lora::init();
            if(err == ESP_OK) err = meshcore::init();
            if(err == ESP_OK) meshcore_enabled = true;
            if(err != ESP_OK) ESP_LOGE(TAG, "MeshCore disabled; SD/radio unavailable: %s", esp_err_to_name(err));
        }

        // init bluetooth
        
        err = bluetooth_transport.load_pairing_pin();
        if(err != ESP_OK){ ESP_LOGE(TAG, "Bluetooth settings invalid: %s", esp_err_to_name(err)); return err; }
        err = bluetooth_transport.begin("MeshCore-Watch", meshcore::companion::UUIDS);
        if(err != ESP_OK){
            ESP_LOGE(TAG, "Bluetooth initialization failed: %s", esp_err_to_name(err));
            return err;
        }

        return err;
    }
}