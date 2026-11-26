#include "PowerManagement.h"
#include "PositionSync.h"
#include "TimeService.h"
#include "MeshCorePort.h"
#include "CompanionBluetooth.h"
#include "DisplayPower.h"

#include "esp_log.h"
#include "HardwareSystem.h"
#include "Storage.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "hardware.h"
#include "board.h"
#include "Power.h"
#include "GPIO.h"
#include "LoRa.h"
#include "GPS.h"
#include "BluetoothTransport.h"
#include "SDStorage.h"
#include "WatchDisplay.h"
#include "WatchStatus.h"

static const char *TAG = "MeshCoreWatch";

static void update_bluetooth_pairing(){
    static uint32_t last_pin = 0;

    const uint32_t pin =
        hardware::bluetooth::transport().active_pairing_pin();

    if(pin == last_pin) return;

    ui::watch_display::publish_pairing_pin(pin);
    services::display_power::set_keep_awake(pin != 0);

    last_pin = pin;
}

// GPS and MeshCore are polled by app_main. Copy their state here rather
// than reading non-atomic firmware structures from the LVGL task.
static void publish_watchface_status(bool meshcore_enabled) {
    static uint32_t last_update_ms = 0;
    const uint32_t now = hardware::system::millis();
    if (last_update_ms != 0 && now - last_update_ms < 1000U) return;
    last_update_ms = now;

    watch::WatchStatus snapshot{};
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
    const auto sleep = services::power_management::sleep_stats();
    snapshot.pm_enabled = sleep.pm_enabled;
    snapshot.sleep_percent = sleep.sleep_percent;
    snapshot.sleep_entries = sleep.sleep_entries;
    ui::watch_display::publish_watch_status(snapshot);
}

extern "C" void app_main() {
    ESP_LOGI(TAG, "T-Watch Ultra: MeshCore BLE companion");

    // Basic hardware initialization
    services::power_management::configure_startup_scaling();
    if(hardware::init() != ESP_OK) return;

    // Display
    esp_err_t err = ui::watch_display::init();
    if(err != ESP_OK) ESP_LOGW(TAG, "Watch display unavailable: %s", esp_err_to_name(err));

    // Time service
    err = services::time_service::initialize_utc_clock();
    if(err != ESP_OK) ESP_LOGW(TAG, "Hardware RTC unavailable: %s", esp_err_to_name(err));

    // Synchronize system clock and RTC with valid GPS time.
    hardware::gps::set_utc_callback(services::time_service::sync_gps_utc);

    // Initialize MeshCore if SD storage is available.
    bool meshcore_enabled = false;
    if(hardware::sd::ready()){
        // Load persistent MeshCore data before initializing LoRa.
        err = meshcore::prepare_storage();
        if(err == ESP_OK) err = hardware::lora::init();
        if(err == ESP_OK) err = meshcore::init();
        meshcore_enabled = (err == ESP_OK);
        if(!meshcore_enabled) ESP_LOGE(TAG, "MeshCore unavailable: %s", esp_err_to_name(err));
    } else {
        ESP_LOGW(TAG, "SD unavailable; MeshCore disabled");
    }

    // Bluetooth
    auto &bluetooth = hardware::bluetooth::transport();
    err = bluetooth.load_pairing_pin();
    if(err == ESP_OK) err = bluetooth.begin("MeshCore-Watch", meshcore::companion::UUIDS);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "Bluetooth initialization failed: %s", esp_err_to_name(err));
        return;
    }

    // configure wake sources after LoRa, touch and BLE init.
    services::power_management::start_light_sleep(meshcore_enabled, ui::watch_display::ready(), meshcore_enabled && hardware::lora::is_in_receive_mode());

    // shared LoRa/BLE event wakeups.
    // Idle connected BLE no longer forces a 10-ms CPU polling loop.
    if(meshcore_enabled) hardware::lora::set_wakeup_task(xTaskGetCurrentTaskHandle());
    bluetooth.set_wakeup_task(xTaskGetCurrentTaskHandle());
    constexpr uint32_t kMainLoopWaitMs = 50;
    constexpr uint32_t kActiveBleWaitMs = 10;

    while(true){
        services::power_management::restore_wakeup_pins_if_needed();
        update_bluetooth_pairing();
        hardware::gps::loop();
        services::position_sync::synchronize();
        if(meshcore_enabled) meshcore::loop();
        bluetooth.loop();
        publish_watchface_status(meshcore_enabled);
        hardware::gps::status_t gps{};
        hardware::gps::get_status(gps);
        const bool screen_awake = ui::watch_display::screen_awake();
        const bool ble_active = bluetooth.link_active();
        services::power_management::update_light_sleep_guard(screen_awake, gps.powered, ble_active);
        services::power_management::report_light_sleep_exits(screen_awake, gps.powered, ble_active);
        // Incoming BLE writes and LoRa IRQs wake immediately.
        const uint32_t wait_ms = (bluetooth.has_pending_tx() || bluetooth.recent_activity()) ? kActiveBleWaitMs : kMainLoopWaitMs;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));
    }
}
