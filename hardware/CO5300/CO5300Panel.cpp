#include "CO5300Panel.h"

#include "AXP2101.h"
#include "XL9555.h"
#include "board_pins.h"

#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_co5300.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace hardware::co5300 {
namespace {
    constexpr auto kSpiHost = SPI3_HOST; // SPI2 belongs to LoRa/SD/NFC.
    constexpr int kSpiClockHz = 40 * 1000 * 1000;
    constexpr uint8_t kInitialBrightness = 25;

    const char *const TAG = "CO5300Panel";
    esp_lcd_panel_io_handle_t s_io = nullptr;
    esp_lcd_panel_handle_t s_panel = nullptr;
    bool s_spi_ready = false;
    bool s_ready = false;
    bool s_screen_enabled = true;
    // Deep standby replays the panel init table, including brightness=0.
    // Keep the last requested brightness for an exact wake restoration.
    uint8_t s_brightness = kInitialBrightness;

    // The working T-Watch Ultra sequence is intentionally sent twice.
    // COLMOD 0x3A / MADCTL 0x36 are provided by the Espressif panel config;
    // omitting them here avoids the duplicate-command warnings.
    static constexpr uint8_t kPage[]       = {0x00};
    static constexpr uint8_t kC4[]         = {0x80};
    static constexpr uint8_t kTeEnable[]   = {0x00};
    static constexpr uint8_t kCtrl[]       = {0x20};
    static constexpr uint8_t kFull[]       = {0xFF};
    static constexpr uint8_t kColumns[]    = {0x00, 0x16, 0x01, 0xAF};
    static constexpr uint8_t kRows[]       = {0x00, 0x00, 0x01, 0xF5};
    static constexpr uint8_t kBrightness[] = {0x00};

#define WATCH_PANEL_INIT_PASS \
        {0xFE, kPage, sizeof(kPage), 0},                   \
        {0xC4, kC4, sizeof(kC4), 0},                       \
        {0x35, kTeEnable, sizeof(kTeEnable), 0},           \
        {0x53, kCtrl, sizeof(kCtrl), 0},                   \
        {0x63, kFull, sizeof(kFull), 0},                   \
        {0x2A, kColumns, sizeof(kColumns), 0},             \
        {0x2B, kRows, sizeof(kRows), 0},                   \
        {0x11, nullptr, 0, 120},                          \
        {0x29, nullptr, 0, 120},                          \
        {0x51, kBrightness, sizeof(kBrightness), 0}

    static const co5300_lcd_init_cmd_t kInit[] = {
        WATCH_PANEL_INIT_PASS,
        WATCH_PANEL_INIT_PASS,
    };
#undef WATCH_PANEL_INIT_PASS

    void rollback_hardware() {
        s_ready = false;
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
        XL9555::set_display_enabled(false);
        AXP2101::disable(AXP2101::rail_t::display);
    }
} // namespace

esp_err_t init(size_t max_transfer_bytes) {
    if (s_ready) return ESP_OK;
    if (max_transfer_bytes == 0) return ESP_ERR_INVALID_ARG;
    ESP_LOGI(TAG, "Initializing CO5300 hardware: 410x502, SPI3 QSPI 40MHz");
    esp_err_t err = AXP2101::enable(AXP2101::rail_t::display);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(20));

    err = XL9555::set_display_enabled(true);
    if (err != ESP_OK) { rollback_hardware(); return err; }
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
    bus_cfg.max_transfer_sz = max_transfer_bytes;

    err = spi_bus_initialize(kSpiHost, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) { rollback_hardware(); return err; }
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
    err = esp_lcd_new_panel_io_spi(static_cast<esp_lcd_spi_bus_handle_t>(kSpiHost),
                                   &io_cfg, &s_io);
    if (err != ESP_OK) { rollback_hardware(); return err; }

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
    if (err == ESP_OK) err = esp_lcd_panel_set_gap(s_panel, x_gap, 0);
    if (err == ESP_OK) err = esp_lcd_panel_disp_on_off(s_panel, true);
    if (err == ESP_OK) err = esp_lcd_panel_co5300_set_brightness(s_panel, kInitialBrightness);
    if (err != ESP_OK) { rollback_hardware(); return err; }

    s_screen_enabled = true;
    s_brightness = kInitialBrightness;
    s_ready = true;
    return ESP_OK;
}

esp_err_t set_screen_enabled(bool enabled) {
    if (!s_ready || s_panel == nullptr) return ESP_ERR_INVALID_STATE;
    if (s_screen_enabled == enabled) return ESP_OK;

    if (!enabled) {
        esp_err_t err = esp_lcd_panel_disp_on_off(s_panel, false);
        if (err != ESP_OK) return err;
#if defined(CONFIG_MCW_AMOLED_DEEP_STANDBY)
        // CO5300 driver v2.2.0 sends SLPIN and deep-standby, with the
        // 120 ms settle delay. Keep ALDO2 and the SPI panel handle alive.
        err = esp_lcd_panel_disp_sleep(s_panel, true);
        if (err != ESP_OK) {
            // Best effort: leave the watch usable if standby failed.
            (void)esp_lcd_panel_disp_sleep(s_panel, false);
            (void)esp_lcd_panel_co5300_set_brightness(s_panel, s_brightness);
            (void)esp_lcd_panel_disp_on_off(s_panel, true);
            ESP_LOGW(TAG, "AMOLED deep standby failed: %s", esp_err_to_name(err));
            return err;
        }
        ESP_LOGI(TAG, "AMOLED entered CO5300 deep standby");
#endif
        s_screen_enabled = false;
        return ESP_OK;
    }

#if defined(CONFIG_MCW_AMOLED_DEEP_STANDBY)
    // The driver resets and reinitializes the panel after deep standby.
    esp_err_t err = esp_lcd_panel_disp_sleep(s_panel, false);
    if (err != ESP_OK) return err;
    err = esp_lcd_panel_co5300_set_brightness(s_panel, s_brightness);
    if (err != ESP_OK) return err;
#else
    esp_err_t err = ESP_OK;
#endif
    err = esp_lcd_panel_disp_on_off(s_panel, true);
    if (err == ESP_OK) {
        s_screen_enabled = true;
        ESP_LOGI(TAG, "AMOLED panel awake");
    }
    return err;
}

esp_err_t set_brightness(uint8_t percentage) {
    if (!s_ready || s_panel == nullptr) return ESP_ERR_INVALID_STATE;
    if (percentage > 100) return ESP_ERR_INVALID_ARG;
    const esp_err_t err = esp_lcd_panel_co5300_set_brightness(s_panel, percentage);
    if (err == ESP_OK) s_brightness = percentage;
    return err;
}

bool ready() { return s_ready; }
esp_lcd_panel_handle_t panel() { return s_panel; }
esp_lcd_panel_io_handle_t panel_io() { return s_io; }

// Only for rollback before LVGL starts. Never free a panel with active LVGL flushes.
void shutdown_after_failed_ui_start() {
    rollback_hardware();
}
} // namespace hardware::co5300
