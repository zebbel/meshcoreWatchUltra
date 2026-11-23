#include "MeshCorePort.h"
#include "CompanionBluetooth.h"

#include <math.h>
#include "esp_log.h"
#include "HardwareSystem.h"
#include "Storage.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "esp_pm.h"  // MCW_ENERGY_STAGE2_V1: dynamic frequency scaling
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "board_pins.h"
#include <atomic>

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

#include "esp_heap_caps.h"

static esp_err_t init_hardware(){
    esp_err_t err = board::init();
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

#if CONFIG_PM_ENABLE && CONFIG_FREERTOS_USE_TICKLESS_IDLE && CONFIG_MCW_AUTO_LIGHT_SLEEP && CONFIG_PM_LIGHT_SLEEP_CALLBACKS
// MCW_AUTO_LIGHT_SLEEP_V1: keep GPS UART, LVGL/panel traffic awake;
// allow tickless idle only when screen and GNSS are both inactive.
static esp_pm_lock_handle_t s_sleep_guard = nullptr;
static bool s_sleep_guard_held = false;
static bool s_auto_light_sleep = false;
static std::atomic<bool> s_restore_wake_pins{false};
static std::atomic<uint32_t> s_light_sleep_exits{0};

// Called on the FreeRTOS idle task path. Never perform I2C, SPI, GPIO
// reconfiguration or log output in this callback.
static esp_err_t on_light_sleep_exit(int64_t, void *) {
    s_restore_wake_pins.store(true, std::memory_order_release);
    s_light_sleep_exits.fetch_add(1, std::memory_order_relaxed);
    return ESP_OK;
}
static esp_pm_sleep_cbs_register_config_t s_sleep_callbacks = {
    .enter_cb = nullptr,
    .exit_cb = on_light_sleep_exit,
    .enter_cb_user_arg = nullptr,
    .exit_cb_user_arg = nullptr,
    .enter_cb_prior = 0,
    .exit_cb_prior = 0,
};

static void disable_light_sleep(const char *reason, esp_err_t err) {
    ESP_LOGE(TAG, "%s (%s); reverting to DFS without light sleep", reason, esp_err_to_name(err));
    esp_pm_config_t cfg = {};
    cfg.max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    cfg.min_freq_mhz = 80;
    cfg.light_sleep_enable = false;
    (void)esp_pm_configure(&cfg);
    s_auto_light_sleep = false;
    (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_EXT0);
    (void)esp_sleep_disable_ext1_wakeup_io(1ULL << board::pins::LORA_DIO1);
}

static void start_light_sleep(bool meshcore_enabled) {
    // No sleep if wake hardware / continuous RX could not be initialized.
    if (!meshcore_enabled || !ui::watch_display::ready() ||
        !hardware::lora::is_in_receive_mode()) {
        ESP_LOGW(TAG, "Auto light sleep not enabled (LoRa RX or touch/display unavailable)");
        return;
    }
    esp_err_t err = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0,
                                      "watch-active", &s_sleep_guard);
    if (err != ESP_OK) { ESP_LOGW(TAG, "PM lock unavailable: %s", esp_err_to_name(err)); return; }
    err = esp_pm_lock_acquire(s_sleep_guard);
    if (err != ESP_OK) { ESP_LOGW(TAG, "PM lock acquire failed: %s", esp_err_to_name(err)); return; }
    s_sleep_guard_held = true;

    // Different IRQ polarities; EXT0 (touch, low) and EXT1 (LoRa, high)
    // leave the digital GPIO interrupt trigger types untouched.
    err = esp_sleep_enable_ext0_wakeup(board::pins::TOUCH_IRQ, 0);
    if (err == ESP_OK)
        err = esp_sleep_enable_ext1_wakeup_io(1ULL << board::pins::LORA_DIO1,
                                             ESP_EXT1_WAKEUP_ANY_HIGH);
    if (err != ESP_OK) { disable_light_sleep("Wake source setup failed", err); return; }
    err = esp_pm_light_sleep_register_cbs(&s_sleep_callbacks);
    if (err != ESP_OK) { disable_light_sleep("Light sleep callback setup failed", err); return; }

    // The main application loop already has an IRQ-assisted 50-ms timeout;
    // ESP-IDF schedules its own wake timer for the earliest RTOS deadline.
    esp_pm_config_t cfg = {};
    cfg.max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    cfg.min_freq_mhz = 80;
    cfg.light_sleep_enable = true;
    err = esp_pm_configure(&cfg);
    if (err != ESP_OK) { disable_light_sleep("esp_pm_configure failed", err); return; }
    s_auto_light_sleep = true;
    ESP_LOGI(TAG, "Auto light sleep configured (CPU power-down disabled): touch EXT0 low, LoRa DIO1 EXT1 high; display/GPS guarded");
}

// RTC pads used for EXT0/EXT1 must return to digital GPIO mode after sleep.
// LoRa IRQ is level-latched until SX1262 clears it, so the 50-ms application
// timeout remains an independent fallback if the edge occurs while asleep.
static void restore_wakeup_pins_if_needed() {
    if (!s_auto_light_sleep || !s_restore_wake_pins.exchange(false, std::memory_order_acq_rel)) return;
    esp_err_t err = rtc_gpio_hold_dis(board::pins::TOUCH_IRQ);
    if (err == ESP_OK) err = rtc_gpio_hold_dis(board::pins::LORA_DIO1);
    if (err == ESP_OK) err = rtc_gpio_deinit(board::pins::TOUCH_IRQ);
    if (err == ESP_OK) err = rtc_gpio_deinit(board::pins::LORA_DIO1);
    if (err == ESP_OK) err = gpio_set_intr_type(board::pins::TOUCH_IRQ, GPIO_INTR_NEGEDGE);
    if (err == ESP_OK) err = gpio_set_intr_type(board::pins::LORA_DIO1, GPIO_INTR_POSEDGE);
    if (err == ESP_OK) err = gpio_intr_enable(board::pins::TOUCH_IRQ);
    if (err == ESP_OK) err = gpio_intr_enable(board::pins::LORA_DIO1);
    if (err != ESP_OK) disable_light_sleep("GPIO restoration failed", err);
}

static void update_light_sleep_guard() {
    if (!s_auto_light_sleep || s_sleep_guard == nullptr) return;
    hardware::gps::status_t gps{};
    hardware::gps::get_status(gps);
    const bool must_stay_awake = ui::watch_display::screen_awake() || gps.powered;
    if (must_stay_awake == s_sleep_guard_held) return;
    const esp_err_t err = must_stay_awake
        ? esp_pm_lock_acquire(s_sleep_guard)
        : esp_pm_lock_release(s_sleep_guard);
    if (err == ESP_OK) s_sleep_guard_held = must_stay_awake;
    else disable_light_sleep("PM lock transition failed", err);
}

static void report_light_sleep_exits() {
    if (!s_auto_light_sleep) return;
    static uint32_t last_report = 0;
    const uint32_t now = hardware::system::millis();
    if (last_report == 0) { last_report = now; return; }
    if (static_cast<uint32_t>(now - last_report) < 60000U) return;
    last_report = now;
    const uint32_t exits = s_light_sleep_exits.exchange(0, std::memory_order_relaxed);
    ESP_LOGI(TAG, "Auto light sleep exits / 60 s: %lu%s",
             static_cast<unsigned long>(exits),
             exits == 0 ? " (check BLE/USB and other PM locks)" : "");
}
#else
static void start_light_sleep(bool) {
    ESP_LOGW(TAG, "Auto light sleep disabled in firmware config; DFS remains active");
}
static void restore_wakeup_pins_if_needed() {}
static void update_light_sleep_guard() {}
static void report_light_sleep_exits() {}
#endif

extern "C" void app_main(){
    ESP_LOGI(TAG, "T-Watch Ultra: MeshCore BLE companion");

    esp_err_t err = hardware::storage::init();
    if(err != ESP_OK){
        ESP_LOGE(TAG, "NVS unavailable: %s", esp_err_to_name(err));
        return;
    }

#if CONFIG_PM_ENABLE
    // MCW_ENERGY_STAGE2_V1: retain full speed under load, allow 80 MHz idle.
    // Deliberately do NOT enable automatic Light Sleep: LoRa DIO1 and BLE
    // would require additional low-power wake/clock validation.
    esp_pm_config_t pm = {};
    pm.max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    pm.min_freq_mhz = 80;
    pm.light_sleep_enable = false;
    const esp_err_t pm_err = esp_pm_configure(&pm);
    if (pm_err == ESP_OK)
        ESP_LOGI(TAG, "Dynamic CPU frequency scaling: %d -> 80 MHz (no light sleep)", pm.max_freq_mhz);
    else
        ESP_LOGW(TAG, "Dynamic CPU scaling unavailable: %s", esp_err_to_name(pm_err));
#endif

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

    // MCW_AUTO_LIGHT_SLEEP_V1: configure wake sources after LoRa, touch and BLE init.
    start_light_sleep(meshcore_enabled);

    // MCW_CPU_LORA_IDLE_V1: DIO1 IRQ wakes app_main for RX/TX events.
    // MeshCore timers, GPS UART and BLE still run at least every 50 ms.
    // Registration is optional; the periodic timeout remains the recovery path.
    if(meshcore_enabled) hardware::lora::set_wakeup_task(xTaskGetCurrentTaskHandle());
    constexpr uint32_t kMainLoopWaitMs = 50;
    ESP_LOGI(TAG, "IRQ-assisted MeshCore loop: <=50 ms idle wait, LoRa DIO1 wake");

    while(true){
        restore_wakeup_pins_if_needed();
        log_bluetooth_pairing();
        hardware::gps::loop();
        synchronize_position();
        if(meshcore_enabled) meshcore::loop();
        bluetooth.loop();
        publish_watchface_status(meshcore_enabled);
        update_light_sleep_guard();
        report_light_sleep_exits();
        // Incoming LoRa RX/TX IRQ wakes immediately; otherwise sleep briefly.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kMainLoopWaitMs));
    }
}
