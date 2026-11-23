#include "WatchDisplay.h"
#include "DisplayTest.h"
#include "ClockPage.h"

#include "CO5300Panel.h"
#include "DisplayPower.h"

#include "esp_lv_adapter.h"
#include "esp_log.h"
#include "lvgl.h"

namespace ui::watch_display {
namespace {
    constexpr int kStripeRows = 10; // Working configuration: 8.2 KiB PSRAM stripe.
    const char *const TAG = "WatchDisplay";
    bool s_adapter_ready = false;
    bool s_ready = false;
    // MCW_ENERGY_STAGE2_V1: no LVGL tick or render task activity while AMOLED is off.
    bool s_adapter_paused = false;

    // Touch wake is handled by DisplayPower, not by LVGL's auto-sleep state machine.
    // Never call pause() while holding the LVGL lock; it needs the worker to quiesce.
    esp_err_t apply_panel_awake(bool awake) {
        if (awake && s_adapter_paused) {
            const esp_err_t resume_err = esp_lv_adapter_resume();
            if (resume_err != ESP_OK) return resume_err;
            s_adapter_paused = false;
        }

        esp_err_t err = esp_lv_adapter_lock(500);
        if (err != ESP_OK) return err;
        err = hardware::co5300::set_screen_enabled(awake);
        if (err == ESP_OK && awake) lv_obj_invalidate(lv_screen_active());
        esp_lv_adapter_unlock();

        if (err == ESP_OK && !awake) {
            err = esp_lv_adapter_pause(500);
            if (err == ESP_OK) {
                s_adapter_paused = true;
            } else {
                // Failure must not leave the panel blank while the policy says awake.
                if (esp_lv_adapter_lock(500) == ESP_OK) {
                    (void)hardware::co5300::set_screen_enabled(true);
                    lv_obj_invalidate(lv_screen_active());
                    esp_lv_adapter_unlock();
                }
            }
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "AMOLED/LVGL sleep transition failed: %s", esp_err_to_name(err));
        }
        return err;
    }
} // namespace

esp_err_t init() {
    if (s_ready) return ESP_OK;
    if (s_adapter_ready) return ESP_ERR_INVALID_STATE;

    ESP_LOGI(TAG, "Initializing LVGL 9 display UI");
    esp_err_t err = hardware::co5300::init(
        hardware::co5300::width * kStripeRows * 2 + 64);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CO5300 panel initialization failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_lv_adapter_config_t adapter_cfg = {};
    adapter_cfg.task_stack_size = ESP_LV_ADAPTER_DEFAULT_STACK_SIZE;
    adapter_cfg.task_priority = ESP_LV_ADAPTER_DEFAULT_TASK_PRIORITY;
    adapter_cfg.task_core_id = ESP_LV_ADAPTER_DEFAULT_TASK_CORE_ID;
    adapter_cfg.tick_period_ms = ESP_LV_ADAPTER_DEFAULT_TICK_PERIOD_MS;
    adapter_cfg.task_min_delay_ms = ESP_LV_ADAPTER_DEFAULT_TASK_MIN_DELAY_MS;
    adapter_cfg.task_max_delay_ms = ESP_LV_ADAPTER_DEFAULT_TASK_MAX_DELAY_MS;
    adapter_cfg.auto_sleep.enable = false;
    adapter_cfg.auto_sleep.mode = ESP_LV_ADAPTER_AUTO_SLEEP_MODE_DISABLED;
    err = esp_lv_adapter_init(&adapter_cfg);
    if (err != ESP_OK) {
        hardware::co5300::shutdown_after_failed_ui_start();
        return err;
    }
    s_adapter_ready = true;

    esp_lv_adapter_display_config_t display_cfg = {};
    display_cfg.panel = hardware::co5300::panel();
    display_cfg.panel_io = hardware::co5300::panel_io();
    display_cfg.profile.interface = ESP_LV_ADAPTER_PANEL_IF_OTHER;
    display_cfg.profile.rotation = ESP_LV_ADAPTER_ROTATE_0;
    display_cfg.profile.hor_res = hardware::co5300::width;
    display_cfg.profile.ver_res = hardware::co5300::height;
    display_cfg.profile.buffer_height = kStripeRows;
    display_cfg.profile.use_psram = true; // Verified on the device.
    display_cfg.profile.enable_ppa_accel = false;
    display_cfg.profile.require_double_buffer = false;
    display_cfg.profile.mono_layout = ESP_LV_ADAPTER_MONO_LAYOUT_NONE;
    display_cfg.tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE;

    lv_display_t *disp = esp_lv_adapter_register_display(&display_cfg);
    if (disp == nullptr) {
        err = ESP_FAIL;
    } else {
        err = esp_lv_adapter_start();
    }
    if (err != ESP_OK) {
        esp_lv_adapter_deinit();
        s_adapter_ready = false;
        hardware::co5300::shutdown_after_failed_ui_start();
        ESP_LOGW(TAG, "LVGL adapter start failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_lv_adapter_lock(2000);
    if (err == ESP_OK) {
        render_clock_page();
        esp_lv_adapter_unlock();
    } else {
        ESP_LOGE(TAG, "LVGL display lock failed: %s", esp_err_to_name(err));
        // Preserve existing behavior: don't destroy a running LVGL task.
        return err;
    }

    s_ready = true;
    // Failure of CST9217/service must not leave the watch with a black screen.
    const esp_err_t touch_err = services::display_power::start(apply_panel_awake, 5000);
    if (touch_err != ESP_OK) {
        ESP_LOGW(TAG, "Touch wake service unavailable: %s; keeping panel on",
                 esp_err_to_name(touch_err));
    }
    ESP_LOGI(TAG, "LVGL 9 display active: 410x502, 22px X gap, SPI3 40MHz");
    return ESP_OK;
}

esp_err_t set_brightness(uint8_t percentage) {
    return hardware::co5300::set_brightness(percentage);
}

esp_err_t set_screen_awake(bool awake) {
    return services::display_power::set_screen_awake(awake);
}

bool screen_awake() { return services::display_power::is_screen_awake(); }
bool ready() { return s_ready; }
} // namespace ui::watch_display
