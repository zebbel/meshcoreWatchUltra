#include "PowerManagement.h"
#include "TimeService.h"
#include "MeshCorePort.h"
#include "CompanionBluetooth.h"
#include "DisplayPower.h"

#include "esp_log.h"
#include "HardwareSystem.h"
#include "Storage.h"

#include "hardware.h"
#include "services.h"
#include "watch.h"
#include "board.h"
#include "Power.h"
#include "GPIO.h"
#include "LoRa.h"
#include "BluetoothTransport.h"
#include "SDStorage.h"
#include "WatchDisplay.h"

static const char *TAG = "MeshCoreWatch";

extern "C" void app_main() {
    ESP_LOGI(TAG, "T-Watch Ultra: MeshCore BLE companion");

    // Basic hardware initialization
    services::power_management::configure_startup_scaling();
    if(hardware::init() != ESP_OK) return;

    // Display
    esp_err_t err = watch::display::init();
    if(err != ESP_OK) ESP_LOGW(TAG, "Watch display unavailable: %s", esp_err_to_name(err));

    // Time service
    err = services::time_service::initialize_utc_clock();
    if(err != ESP_OK) ESP_LOGW(TAG, "Hardware RTC unavailable: %s", esp_err_to_name(err));

    // Initialize MeshCore.
    err = meshcore::init();
    const bool meshcore_enabled = (err == ESP_OK);
    if(!meshcore_enabled) ESP_LOGW(TAG, "MeshCore unavailable: %s", esp_err_to_name(err));

    // Start MeshCore Bluetooth companion.
    auto &meshcore_bluetooth = hardware::bluetooth::transport();
    err = meshcore_bluetooth.begin("MeshCore-Watch", meshcore::companion::UUIDS);
    if(err != ESP_OK){
        ESP_LOGE(TAG, "Bluetooth initialization failed: %s", esp_err_to_name(err));
        return;
    }

    // configure wake sources after LoRa, touch and BLE init.
    services::power_management::start_light_sleep(meshcore_enabled, watch::display::ready(), meshcore_enabled && hardware::lora::is_in_receive_mode());

    // Configure event-driven task wakeups.
    services::power_management::configure_task_wakeups(meshcore_enabled);

    while(true){
        services::restore_wakup_pins();
        hardware::loop();
        services::loop();
        meshcore::loop();
        watch::loop();
        services::sleep_guard();
    }
}
