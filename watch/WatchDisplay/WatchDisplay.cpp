#include "WatchDisplay.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "FlowBridge.h"
#include "CO5300Panel.h"
#include "DisplayPower.h"

#include "esp_lv_adapter.h"
#include "esp_lv_adapter_display.h"
#include "lvgl.h"

namespace watch::display {
namespace {
    // 32 RGB565 rows (~25.6 KiB) in PSRAM.
    // Restrict one LVGL render operation to bounded rows so scaled-font
    // rasterization cannot monopolize CPU0 for a full 410x502 frame.
    // Keep PARTIAL mode for inexpensive small dirty regions.
    constexpr int kDrawRows = 32;
    const char *const TAG = "WatchDisplay";
    bool s_adapter_ready = false;
    bool s_ready = false;
    // no LVGL tick or render task activity while AMOLED is off.
    bool s_adapter_paused = false;
    uint32_t s_flush_events = 0;

    // LVGL task only. The DisplayPower service remains the sole owner of
    // touch IRQ reads/ACKs. No I2C access or LVGL calls from the producer.
    void read_touch(lv_indev_t *indev, lv_indev_data_t *data) {
        const auto sample = services::display_power::read_touch_input();
        data->point.x = sample.x < hardware::co5300::width ? sample.x : hardware::co5300::width - 1;
        data->point.y = sample.y < hardware::co5300::height ? sample.y : hardware::co5300::height - 1;
        data->state = sample.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
        data->continue_reading = sample.more;
        if (sample.reset) {
            lv_indev_reset(indev, nullptr);
            data->state = LV_INDEV_STATE_RELEASED;
            data->continue_reading = false;
        }
    }

    void init_touch_input(lv_display_t *disp) {
        lv_indev_t *s_touch_indev = lv_indev_create();
        if (!s_touch_indev) {
            ESP_LOGW(TAG, "LVGL touch input allocation failed; touch wake still available");
            return;
        }
        lv_indev_set_type(s_touch_indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_display(s_touch_indev, disp);
        lv_indev_set_read_cb(s_touch_indev, read_touch);
        ESP_LOGI(TAG, "LVGL touch pointer registered (IRQ-driven reports)");
    }
    // LVGL supplies packed RGB565 in panel byte order. The hardware driver
    // owns DMA staging and returns after every segment has completed.
    esp_err_t staged_draw_bitmap(lv_display_t *disp, esp_lcd_panel_handle_t,
                                 int x_start, int y_start, int x_end, int y_end,
                                 const void *color_map, void *) {
        const esp_err_t err = hardware::co5300::draw_rgb565(
            x_start, y_start, x_end, y_end, color_map);
        // On failure the adapter acknowledges the flush itself.
        if (err == ESP_OK) lv_display_flush_ready(disp);
        return err;
    }

    // Full-width dirty regions are required for reliable live display updates.
    // Keep vertical bounds and the partial rendering buffer unchanged.
    void full_width_dirty_area(lv_area_t *area, void *) {
        if (!area) return;
        area->x1 = 0;
        area->x2 = hardware::co5300::width - 1;
    }

    // LVGL calls this on its rendering task (NOT in a SPI completion ISR).
    // Large upscaled clock fonts can monopolize CPU0 while drawing. Yield
    // for one scheduler tick after every second PARTIAL flush so IDLE0 can
    // run and service the watchdog. No effect when display power is off.
    void on_flush_submitted(lv_event_t *) {
        if ((++s_flush_events & 1U) == 0U) vTaskDelay(1);
    }

    esp_err_t apply_charging_display(bool charging) {
        esp_err_t err = esp_lv_adapter_lock(1000);
        if (err != ESP_OK) return err;
        refresh_flow(charging);
        lv_obj_invalidate(lv_screen_active());
        lv_refr_now(lv_display_get_default());
        esp_lv_adapter_unlock();
        return ESP_OK;
    }

    // The adapter worker must be paused before CO5300 deep standby/reset.
    // Never pause while holding the LVGL lock; pause needs a worker ACK.
    esp_err_t apply_panel_awake(bool awake) {
        if (!awake) {
            esp_err_t err = esp_lv_adapter_pause(500);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "LVGL pause failed: %s", esp_err_to_name(err));
                return err;
            }
            s_adapter_paused = true;
            // The CO5300 DISPOFF command is an SPI tx_param operation.
            // ESP-IDF drains queued color DMA before issuing that command,
            // replacing the earlier unverified 30ms settling delay.
            err = hardware::co5300::set_screen_enabled(false);
            if (err != ESP_OK) {
                // Do not stop the UI worker if the panel failed to sleep.
                (void)hardware::co5300::set_screen_enabled(true);
                if (esp_lv_adapter_resume() == ESP_OK) {
                    s_adapter_paused = false;
                    if (esp_lv_adapter_lock(500) == ESP_OK) {
                        lv_obj_invalidate(lv_screen_active());
                        esp_lv_adapter_unlock();
                    }
                }
                ESP_LOGW(TAG, "AMOLED sleep failed: %s", esp_err_to_name(err));
            }
            return err;
        }

        // the CO5300 wakes with its output hidden.
        // The DisplayPower NO_LIGHT_SLEEP lock protects all operations here.
        esp_err_t err = hardware::co5300::set_screen_enabled(true, true);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "AMOLED hidden wake failed: %s", esp_err_to_name(err));
            return err;
        }

        // Lock BEFORE unpausing the worker. This prevents an early worker
        // flush from competing with the forced redraw below.
        err = esp_lv_adapter_lock(1000);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "LVGL wake lock failed: %s", esp_err_to_name(err));
            (void)hardware::co5300::reveal_after_full_refresh();
            return err;
        }
        if (s_adapter_paused) {
            err = esp_lv_adapter_resume();
            if (err != ESP_OK) {
                esp_lv_adapter_unlock();
                (void)hardware::co5300::reveal_after_full_refresh();
                ESP_LOGE(TAG, "LVGL resume failed: %s", esp_err_to_name(err));
                return err;
            }
            s_adapter_paused = false;
        }
        lv_display_t *disp = lv_display_get_default();
        if (disp == nullptr) {
            esp_lv_adapter_unlock();
            (void)hardware::co5300::reveal_after_full_refresh();
            ESP_LOGE(TAG, "AMOLED wake: LVGL display missing");
            return ESP_ERR_INVALID_STATE;
        }

        // Refresh native state before the hidden full-frame redraw.
        refresh_flow(services::display_power::is_charging());
        lv_obj_invalidate(lv_screen_active());
        // LVGL renders ~16 bounded regions, yielding periodically to avoid
        // starving IDLE0. The mutex prevents any concurrent LVGL redraw.
        lv_refr_now(disp);

        // IDF 6.1.1 spi_panel_io_tx_param drains queued color DMA before
        // sending DISPON. The full frame is in panel GRAM before showing it.
        err = hardware::co5300::reveal_after_full_refresh();
        esp_lv_adapter_unlock();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "AMOLED DMA-fenced reveal failed: %s", esp_err_to_name(err));
        }
        return err;
    }
} // namespace

esp_err_t init() {
    if (s_ready) return ESP_OK;
    if (s_adapter_ready) return ESP_ERR_INVALID_STATE;

    constexpr size_t kDrawBufferBytes =
        static_cast<size_t>(hardware::co5300::width) * kDrawRows * 2;
    esp_err_t err = hardware::co5300::init();
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
        hardware::co5300::shutdown();
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
    display_cfg.profile.buffer_height = kDrawRows;
    display_cfg.profile.use_psram = true; // Verified on the device.
    display_cfg.profile.enable_ppa_accel = false;
    // Single 32-row PSRAM draw buffer: LVGL waits for a completed flush
    // before reusing it; avoid a concurrent second-buffer DMA race.
    display_cfg.profile.require_double_buffer = false;
    display_cfg.profile.mono_layout = ESP_LV_ADAPTER_MONO_LAYOUT_NONE;
    display_cfg.tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE;

    // Disable adapter IDF LCD ISR-to-LVGL completion forwarding for
    // this display. The custom synchronous blitter calls flush_ready once
    // after all SPI transfers finish. Otherwise every 4 KiB segment could
    // falsely acknowledge a full LVGL region before its last pixels arrive.
    err = esp_lv_adapter_set_default_display_idf_callback_registration_enabled(false);
    lv_display_t *disp = (err == ESP_OK) ? esp_lv_adapter_register_display(&display_cfg) : nullptr;
    if (disp == nullptr) {
        if (err == ESP_OK) err = ESP_FAIL;
    } else {
        // One callback; yield every two flushes to preserve the effective
        // cadence of the previous duplicate callback registration.
        lv_display_add_event_cb(disp, on_flush_submitted, LV_EVENT_FLUSH_FINISH, nullptr);
        // The adapter's SPI completion forwarding is disabled above.
        esp_lv_adapter_draw_bitmap_callbacks_t cbs = {};
        cbs.custom_draw_bitmap = staged_draw_bitmap;
        err = esp_lv_adapter_set_draw_bitmap_callbacks(disp, &cbs, nullptr);
        // Register before the LVGL worker starts. The adapter invokes
        // this callback on LV_EVENT_INVALIDATE_AREA, not from an ISR.
        if (err == ESP_OK) {
            err = esp_lv_adapter_set_area_rounder_cb(disp, full_width_dirty_area, nullptr);
        }
        if (err == ESP_OK) err = esp_lv_adapter_start();
    }
    if (err != ESP_OK) {
        esp_lv_adapter_deinit();
        s_adapter_ready = false;
        hardware::co5300::shutdown();
        ESP_LOGW(TAG, "LVGL adapter start failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_lv_adapter_lock(2000);
    if (err == ESP_OK) {
        err = init_flow();
        if (err == ESP_OK) init_touch_input(disp);
        esp_lv_adapter_unlock();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "EEZ UI initialization failed: %s", esp_err_to_name(err));
            return err;
        }
    } else {
        ESP_LOGE(TAG, "LVGL display lock failed: %s", esp_err_to_name(err));
        // Preserve existing behavior: don't destroy a running LVGL task.
        return err;
    }

    s_ready = true;
    // Failure of CST9217/service must not leave the watch with a black screen.
    const esp_err_t touch_err = services::display_power::start(apply_panel_awake, 5000, apply_charging_display);
    if (touch_err != ESP_OK) {
        ESP_LOGW(TAG, "Touch wake service unavailable: %s; keeping panel on",
                 esp_err_to_name(touch_err));
    }
    ESP_LOGI(TAG, "LVGL ready: %u-row PSRAM buffer (%zu bytes), full-width updates",
             static_cast<unsigned>(kDrawRows), kDrawBufferBytes);
    return ESP_OK;
}

esp_err_t set_screen_awake(bool awake) {
    return services::display_power::set_screen_awake(awake);
}

bool screen_awake() { return services::display_power::is_screen_awake(); }
bool ready() { return s_ready; }
} // namespace watch::display
