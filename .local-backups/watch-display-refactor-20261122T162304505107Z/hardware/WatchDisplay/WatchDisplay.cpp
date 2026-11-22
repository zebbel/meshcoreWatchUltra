#include "WatchDisplay.h"
#include "TouchWake.h"

#include "AXP2101.h"
#include "XL9555.h"
#include "board_pins.h"

#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_co5300.h"
#include "esp_lv_adapter.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

namespace hardware::watch_display {
namespace {
    constexpr int kWidth = 410;
    constexpr int kHeight = 502;
    constexpr int kColumnOffset = 22;
    constexpr int kStripeRows = 10; // 410 * 20 * 2 = 16,400 bytes (internal RAM)
    constexpr int kTransferBytes = kWidth * kStripeRows * 2 + 64;
    constexpr auto kSpiHost = SPI3_HOST; // SPI2 is exclusively used by LoRa/SD/NFC
    constexpr int kSpiClockHz = 40 * 1000 * 1000;
    constexpr uint8_t kInitialBrightness = 25;

    const char *const TAG = "WatchDisplay";
    esp_lcd_panel_io_handle_t s_io = nullptr;
    esp_lcd_panel_handle_t s_panel = nullptr;
    bool s_spi_ready = false;
    bool s_adapter_ready = false;
    bool s_ready = false;
    bool s_screen_awake = true;

    // Keep the LilyGO-specific initialization passes unchanged, except
    // for COLMOD (0x3A) and MADCTL (0x36), which Espressif's CO5300 driver
    // already sets from the panel config: 16-bit RGB565 and RGB order.
    static constexpr uint8_t kPage[]      = {0x00};
    static constexpr uint8_t kC4[]        = {0x80};
    static constexpr uint8_t kTeEnable[]  = {0x00};
    static constexpr uint8_t kCtrl[]      = {0x20};
    static constexpr uint8_t kFull[]      = {0xFF};
    static constexpr uint8_t kColumns[]   = {0x00, 0x16, 0x01, 0xAF};
    static constexpr uint8_t kRows[]      = {0x00, 0x00, 0x01, 0xF5};
    static constexpr uint8_t kBrightness[]= {0x00};

#define WATCH_PANEL_INIT_PASS \
        {0xFE, kPage, sizeof(kPage), 0},                  \
        {0xC4, kC4, sizeof(kC4), 0},                      \
        {0x35, kTeEnable, sizeof(kTeEnable), 0},         \
        {0x53, kCtrl, sizeof(kCtrl), 0},                 \
        {0x63, kFull, sizeof(kFull), 0},                 \
        {0x2A, kColumns, sizeof(kColumns), 0},           \
        {0x2B, kRows, sizeof(kRows), 0},                 \
        {0x11, nullptr, 0, 120},                         \
        {0x29, nullptr, 0, 120},                         \
        {0x51, kBrightness, sizeof(kBrightness), 0}

    static const co5300_lcd_init_cmd_t kInit[] = {
        WATCH_PANEL_INIT_PASS,
        WATCH_PANEL_INIT_PASS,
    };
#undef WATCH_PANEL_INIT_PASS

    void rollback_hardware() {
        if (s_panel != nullptr) {
            esp_lcd_panel_del(s_panel);
            s_panel = nullptr;
        }
        if (s_io != nullptr) {
            esp_lcd_panel_io_del(s_io);
            s_io = nullptr;
        }
        if (s_spi_ready) {
            spi_bus_free(kSpiHost);
            s_spi_ready = false;
        }
        // The panel is controlled by both the PMU rail and XL9555 enable.
        XL9555::set_display_enabled(false);
        AXP2101::disable(AXP2101::rail_t::display);
    }

    esp_err_t panel_init() {
        esp_err_t err = AXP2101::enable(AXP2101::rail_t::display);
        if (err != ESP_OK) return err;
        vTaskDelay(pdMS_TO_TICKS(20));

        err = XL9555::set_display_enabled(true);
        if (err != ESP_OK) {
            rollback_hardware();
            return err;
        }
        vTaskDelay(pdMS_TO_TICKS(20));

        spi_bus_config_t bus_cfg = {};
        bus_cfg.sclk_io_num = board::pins::DISPLAY_SCK;
        bus_cfg.data0_io_num = board::pins::DISPLAY_D0;
        bus_cfg.data1_io_num = board::pins::DISPLAY_D1;
        bus_cfg.data2_io_num = board::pins::DISPLAY_D2;
        bus_cfg.data3_io_num = board::pins::DISPLAY_D3;
        bus_cfg.data4_io_num = -1;
        bus_cfg.data5_io_num = -1;
        bus_cfg.data6_io_num = -1;
        bus_cfg.data7_io_num = -1;
        bus_cfg.max_transfer_sz = kTransferBytes;

        err = spi_bus_initialize(kSpiHost, &bus_cfg, SPI_DMA_CH_AUTO);
        if (err != ESP_OK) {
            rollback_hardware();
            return err;
        }
        s_spi_ready = true;

        esp_lcd_panel_io_spi_config_t io_cfg = {};
        io_cfg.cs_gpio_num = board::pins::DISPLAY_CS;
        io_cfg.dc_gpio_num = GPIO_NUM_NC;
        io_cfg.spi_mode = 0;
        io_cfg.pclk_hz = kSpiClockHz;
        io_cfg.trans_queue_depth = 10;
        io_cfg.lcd_cmd_bits = 32;
        io_cfg.lcd_param_bits = 8;
        io_cfg.flags.quad_mode = true;
        // The LVGL adapter registers completion callbacks for its flushes.
        err = esp_lcd_new_panel_io_spi(
            static_cast<esp_lcd_spi_bus_handle_t>(kSpiHost),
            &io_cfg, &s_io);
        if (err != ESP_OK) {
            rollback_hardware();
            return err;
        }

        co5300_vendor_config_t vendor = {};
        vendor.init_cmds = kInit;
        vendor.init_cmds_size = sizeof(kInit) / sizeof(kInit[0]);
        vendor.flags.use_qspi_interface = true;

        esp_lcd_panel_dev_config_t panel_cfg = {};
        panel_cfg.reset_gpio_num = board::pins::DISPLAY_RST;
        panel_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_cfg.bits_per_pixel = 16;
        panel_cfg.vendor_config = &vendor;

        err = esp_lcd_new_panel_co5300(s_io, &panel_cfg, &s_panel);
        if (err == ESP_OK) err = esp_lcd_panel_reset(s_panel);
        if (err == ESP_OK) err = esp_lcd_panel_init(s_panel);
        if (err == ESP_OK) err = esp_lcd_panel_set_gap(s_panel, kColumnOffset, 0);
        if (err == ESP_OK) err = esp_lcd_panel_disp_on_off(s_panel, true);
        if (err == ESP_OK) err = esp_lcd_panel_co5300_set_brightness(s_panel, kInitialBrightness);
        if (err != ESP_OK) rollback_hardware();
        return err;
    }

    lv_obj_t *make_color_tile(lv_obj_t *parent, int x, int y, uint32_t rgb) {
        lv_obj_t *tile = lv_obj_create(parent);
        lv_obj_set_size(tile, 42, 42);
        lv_obj_set_pos(tile, x, y);
        lv_obj_set_style_radius(tile, 10, 0);
        lv_obj_set_style_bg_color(tile, lv_color_hex(rgb), 0);
        lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(tile, 0, 0);
        lv_obj_set_style_pad_all(tile, 0, 0);
        lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
        return tile;
    }

    void create_test_screen() {
        lv_obj_t *screen = lv_screen_active();
        lv_obj_set_style_bg_color(screen, lv_color_hex(0x070B14), 0);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
        lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

        // Limit content to a central safe area of the watch's rounded face.
        lv_obj_t *panel = lv_obj_create(screen);
        lv_obj_set_size(panel, 320, 320);
        lv_obj_center(panel);
        lv_obj_set_style_bg_color(panel, lv_color_hex(0x101C2A), 0);
        lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(panel, lv_color_hex(0x34506A), 0);
        lv_obj_set_style_border_width(panel, 2, 0);
        lv_obj_set_style_radius(panel, 36, 0);
        lv_obj_set_style_pad_all(panel, 0, 0);
        lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *title = lv_label_create(panel);
        lv_label_set_text(title, "MESHCORE WATCH");
        lv_obj_set_style_text_color(title, lv_color_hex(0xE5F3FF), 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 45);

        lv_obj_t *subtitle = lv_label_create(panel);
        lv_label_set_text(subtitle, "Display foundation");
        lv_obj_set_style_text_color(subtitle, lv_color_hex(0x67DCEC), 0);
        lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, 95);

        make_color_tile(panel, 52, 146, 0xF44336);  // RGB red
        make_color_tile(panel, 110, 146, 0x4CAF50); // RGB green
        make_color_tile(panel, 168, 146, 0x2196F3); // RGB blue
        make_color_tile(panel, 226, 146, 0xFFFFFF); // white

        lv_obj_t *details = lv_label_create(panel);
        lv_label_set_text(details, "CO5300  /  QSPI  /  RGB565");
        lv_obj_set_style_text_color(details, lv_color_hex(0xAABACB), 0);
        lv_obj_align(details, LV_ALIGN_BOTTOM_MID, 0, -70);

        lv_obj_t *footer = lv_label_create(panel);
        lv_label_set_text(footer, "LVGL 9  |  410 x 502");
        lv_obj_set_style_text_color(footer, lv_color_hex(0x75869C), 0);
        lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, -39);
    }
} // namespace

esp_err_t init() {
    if (s_ready) return ESP_OK;
    if (s_adapter_ready) return ESP_ERR_INVALID_STATE;

    ESP_LOGI(TAG, "Initializing T-Watch Ultra CO5300 + LVGL display foundation");
    esp_err_t err = panel_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CO5300 panel initialization failed: %s", esp_err_to_name(err));
        return err;
    }

    // Explicit initialization also builds cleanly under ESP-IDF C++17.
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
        rollback_hardware();
        return err;
    }
    s_adapter_ready = true;

    // Start with 16.4 KB internal partial RGB565 render buffer. Avoid
    // allocating full-screen framebuffers until refresh is verified.
    esp_lv_adapter_display_config_t display_cfg = {};
    display_cfg.panel = s_panel;
    display_cfg.panel_io = s_io;
    display_cfg.profile.interface = ESP_LV_ADAPTER_PANEL_IF_OTHER;
    display_cfg.profile.rotation = ESP_LV_ADAPTER_ROTATE_0;
    display_cfg.profile.hor_res = kWidth;
    display_cfg.profile.ver_res = kHeight;
    display_cfg.profile.buffer_height = kStripeRows;
    display_cfg.profile.use_psram = true;
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
        rollback_hardware();
        ESP_LOGW(TAG, "LVGL adapter start failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_lv_adapter_lock(2000);
    if (err == ESP_OK) {
        create_test_screen();
        esp_lv_adapter_unlock();
    } else {
        ESP_LOGE(TAG, "LVGL display lock failed: %s", esp_err_to_name(err));
        // Do not destroy an active LVGL panel while its task is running.
        return err;
    }

    s_ready = true;
    // Start touch-to-wake only after LVGL is ready. A failed touch setup
    // leaves the panel on rather than creating an unwakeable black screen.
    esp_err_t touch_err = touch_wake::start();
    if (touch_err != ESP_OK) {
        ESP_LOGW(TAG, "CST9217 wake service unavailable: %s; keeping panel on",
                 esp_err_to_name(touch_err));
    }
    ESP_LOGI(TAG, "LVGL 9 display active: 410x502, 22px X gap, SPI3 40MHz");
    return ESP_OK;
}

esp_err_t set_brightness(uint8_t percentage) {
    if (s_panel == nullptr) return ESP_ERR_INVALID_STATE;
    if (percentage > 100) return ESP_ERR_INVALID_ARG;
    return esp_lcd_panel_co5300_set_brightness(s_panel, percentage);
}

esp_err_t set_screen_awake(bool awake) {
    if (!s_ready || s_panel == nullptr || !s_adapter_ready)
        return ESP_ERR_INVALID_STATE;
    if (s_screen_awake == awake) return ESP_OK;

    // LVGL's worker may flush concurrently; serialize display operations.
    esp_err_t err = esp_lv_adapter_lock(500);
    if (err != ESP_OK) return err;
    err = esp_lcd_panel_disp_on_off(s_panel, awake);
    if (err == ESP_OK) {
        s_screen_awake = awake;
        if (awake) lv_obj_invalidate(lv_screen_active());
    }
    esp_lv_adapter_unlock();
    if (err != ESP_OK) ESP_LOGW(TAG, "AMOLED display power command failed: %s",
                               esp_err_to_name(err));
    return err;
}

bool screen_awake() { return s_screen_awake; }
bool ready() { return s_ready; }

} // namespace hardware::watch_display
