#include "MeshCorePort.h"
#include "CompanionBluetooth.h"

#include <math.h>
#include "esp_log.h"
#include "HardwareSystem.h"
#include "Storage.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "Board.h"
#include "Power.h"
#include "GPIO.h"
#include "LoRa.h"
#include "GPS.h"
#include "BluetoothTransport.h"
#include "SDStorage.h"
#include "WatchDisplay.h"
#include "WatchStatus.h"

static const char *TAG = "MeshCoreWatch";

#include "esp_heap_caps.h"

static esp_err_t init_hardware(){
    esp_err_t err = hardware::board::init();
    if(err == ESP_OK) err = hardware::power::init();
    if(err == ESP_OK) err = hardware::power::configure();
    if(err == ESP_OK) err = hardware::gpio::init();
    if(err == ESP_OK) err = hardware::gpio::configure();
    if(err != ESP_OK) ESP_LOGE(TAG, "Hardware init failed: %s", esp_err_to_name(err));
    return err;
}

static void synchronize_position(){
    static uint32_t last_save_ms = 0;
    hardware::gps::fix_t fix = {};
    if(!hardware::gps::get_fresh_fix(fix)) return;

    const int32_t gps_latitude = static_cast<int32_t>(lround(fix.latitude * 1000000.0));
    const int32_t gps_longitude = static_cast<int32_t>(lround(fix.longitude * 1000000.0));
    int32_t current_latitude = 0;
    int32_t current_longitude = 0;
    if(!meshcore::get_position(current_latitude, current_longitude)) return;

    int64_t latitude_difference = static_cast<int64_t>(gps_latitude) - current_latitude;
    int64_t longitude_difference = static_cast<int64_t>(gps_longitude) - current_longitude;
    if(latitude_difference < 0) latitude_difference = -latitude_difference;
    if(longitude_difference < 0) longitude_difference = -longitude_difference;

    const bool position_missing = current_latitude == 0 && current_longitude == 0;
    const bool position_changed = latitude_difference >= 100 || longitude_difference >= 100;
    const uint32_t now = static_cast<uint32_t>(hardware::system::millis());

    if((position_missing || position_changed) && (last_save_ms == 0 || now - last_save_ms >= 300000U)){
        esp_err_t err = meshcore::set_position(gps_latitude, gps_longitude);

        if(err == ESP_OK){
            last_save_ms = now;
            ESP_LOGI(TAG, "GPS position synchronized to companion settings");
        }else{
            ESP_LOGW(TAG, "GPS position could not be saved: %s", esp_err_to_name(err));
        }
    }
}

static void log_bluetooth_pairing(){
    auto &bluetooth = hardware::bluetooth::transport();
    const uint32_t pin = bluetooth.take_pairing_pin();
    const bool clear = bluetooth.take_pairing_clear_event();
    if(pin != 0) ESP_LOGI(TAG, "Bluetooth pairing PIN: %06lu", static_cast<unsigned long>(pin));
    if(clear) ESP_LOGI(TAG, "Bluetooth pairing PIN cleared");
}
// GPS and MeshCore are polled by app_main. Copy their state here rather
// than reading non-atomic firmware structures from the LVGL task.
static void publish_watchface_status(bool meshcore_enabled) {
    static uint32_t last_update_ms = 0;
    const uint32_t now = hardware::system::millis();
    if (last_update_ms != 0 && now - last_update_ms < 1000U) return;
    last_update_ms = now;

    ui::watch_display::WatchStatus snapshot{};
    snapshot.gps_enabled = hardware::gps::is_enabled();
    hardware::gps::fix_t fix{};
    snapshot.gps_has_fix = hardware::gps::get_fresh_fix(fix);
    hardware::gps::status_t gps_info{};
    hardware::gps::get_status(gps_info);
    snapshot.gps_acquiring = snapshot.gps_enabled && gps_info.powered;
    if (snapshot.gps_has_fix) snapshot.gps_satellites = gps_info.satellites;

    snapshot.meshcore_active = meshcore_enabled && hardware::sd::ready();
    if (snapshot.meshcore_active)
        snapshot.meshcore_rx_packets = meshcore::received_packet_count();
    snapshot.ble_connected = hardware::bluetooth::transport().connected();
    ui::watch_display::publish_watch_status(snapshot);
}

extern "C" void app_main(){
    ESP_LOGI(TAG, "T-Watch Ultra: MeshCore BLE companion");

    esp_err_t err = hardware::storage::init();
    if(err != ESP_OK){
        ESP_LOGE(TAG, "NVS unavailable: %s", esp_err_to_name(err));
        return;
    }

    if(init_hardware() != ESP_OK) return;
    // Display errors do not prevent the MeshCore/BLE/GPS services from starting.
    err = hardware::watch_display::init();
    if(err != ESP_OK) ESP_LOGW(TAG, "Display foundation unavailable: %s", esp_err_to_name(err));
    // Restore persistent UTC before MeshCore creates its RTC adapter.
    err = hardware::system::initialize_utc_clock();
    if(err != ESP_OK) ESP_LOGW(TAG, "Hardware RTC unavailable: %s", esp_err_to_name(err));
    err = hardware::gps::init();
    if(err != ESP_OK) ESP_LOGW(TAG, "MIA_M10Q unavailable: %s", esp_err_to_name(err));

    err = hardware::sd::init();
    if(err != ESP_OK) ESP_LOGE(TAG, "SD subsystem failed: %s", esp_err_to_name(err));
    bool meshcore_enabled = false;
    if(err == ESP_OK){
        // Single mount: preload identity, settings, contacts, channels and
        // node name, then power off SD BEFORE initializing the LoRa radio.
        err = meshcore::prepare_storage();
        if(err == ESP_OK) err = hardware::lora::init();
        if(err == ESP_OK) err = meshcore::init();
        if(err == ESP_OK) meshcore_enabled = true;
        if(err != ESP_OK) ESP_LOGE(TAG, "MeshCore disabled; SD/radio unavailable: %s", esp_err_to_name(err));
    }

    // Pairing and bonding operate even without the MeshCore SD card.
    auto &bluetooth = hardware::bluetooth::transport();
    err = bluetooth.load_pairing_pin();
    if(err != ESP_OK){ ESP_LOGE(TAG, "Bluetooth settings invalid: %s", esp_err_to_name(err)); return; }
    err = bluetooth.begin("MeshCore-Watch", meshcore::companion::UUIDS);
    if(err != ESP_OK){
        ESP_LOGE(TAG, "Bluetooth initialization failed: %s", esp_err_to_name(err));
        return;
    }

    while(true){
        log_bluetooth_pairing();
        hardware::gps::loop();
        synchronize_position();
        if(meshcore_enabled) meshcore::loop();
        bluetooth.loop();
        publish_watchface_status(meshcore_enabled);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
