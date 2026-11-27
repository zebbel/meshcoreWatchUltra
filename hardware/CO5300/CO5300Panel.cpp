#include "CO5300Panel.h"

#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstring>

#include "AXP2101.h"
#include "XL9555.h"
#include "board_pins.h"

#include "esp_lcd_co5300.h"

namespace hardware::co5300 {
    namespace {
        constexpr auto kSpiHost = SPI3_HOST; // SPI2 belongs to LoRa/SD/NFC.
        constexpr int spi_clock_hz = 40 * 1000 * 1000;
        constexpr uint8_t initial_brightness = 0; // blank until EEZ sets brightness
        constexpr size_t dma_stage_bytes = 4096;
        uint8_t *dma_stage = nullptr;

        const char *const TAG = "CO5300Panel";
        esp_lcd_panel_io_handle_t s_io = nullptr;
        esp_lcd_panel_handle_t s_panel = nullptr;
        bool spi_ready = false;
        bool s_ready = false;
        bool screen_enabled = true;
        bool output_hidden_for_redraw = false;
        // Deep standby replays the panel init table, including brightness=0.
        // Keep the last requested brightness for an exact wake restoration.
        uint8_t s_brightness = initial_brightness;

        // The working T-Watch Ultra sequence is intentionally sent twice.
        // COLMOD 0x3A / MADCTL 0x36 are provided by the Espressif panel config;
        // omitting them here avoids the duplicate-command warnings.
        static constexpr uint8_t kPage[]       = {0x00};
        static constexpr uint8_t kC4[]         = {0x80};
        static constexpr uint8_t te_enable[]   = {0x00};
        static constexpr uint8_t kCtrl[]       = {0x20};
        static constexpr uint8_t kFull[]       = {0xFF};
        static constexpr uint8_t columns[]    = {0x00, 0x16, 0x01, 0xAF};
        static constexpr uint8_t kRows[]       = {0x00, 0x00, 0x01, 0xF5};
        static constexpr uint8_t kBrightness[] = {0x00};

#define WATCH_PANEL_INIT_PASS \
        {0xFE, kPage, sizeof(kPage), 0},                   \
        {0xC4, kC4, sizeof(kC4), 0},                       \
        {0x35, te_enable, sizeof(te_enable), 0},           \
        {0x53, kCtrl, sizeof(kCtrl), 0},                   \
        {0x63, kFull, sizeof(kFull), 0},                   \
        {0x2A, columns, sizeof(columns), 0},             \
        {0x2B, kRows, sizeof(kRows), 0},                   \
        {0x11, nullptr, 0, 120},                          \
        {0x29, nullptr, 0, 0},                          \
        {0x51, kBrightness, sizeof(kBrightness), 0}

        static const co5300_lcd_init_cmd_t kInit[] = {WATCH_PANEL_INIT_PASS};
#undef WATCH_PANEL_INIT_PASS

        // ------------------------------------------------------------------------
        // Panel resource cleanup
        // ------------------------------------------------------------------------

        // Release resources in reverse acquisition order.
        void rollback_hardware(){
            s_ready = false;
            if(s_panel != nullptr){
                esp_lcd_panel_del(s_panel);
                s_panel = nullptr;
            }
            if(s_io != nullptr){
                esp_lcd_panel_io_del(s_io);
                s_io = nullptr;
            }
            if(spi_ready){
                spi_bus_free(kSpiHost);
                spi_ready = false;
            }
            heap_caps_free(dma_stage);
            dma_stage = nullptr;
            hardware::xl9555::set_display_enabled(false);
            hardware::axp2101::disable(hardware::axp2101::rail_t::display);
        }
    } // namespace

    // ------------------------------------------------------------------------
    // Display initialization
    // ------------------------------------------------------------------------

    // Power the panel, configure QSPI and allocate internal DMA staging.
    esp_err_t init(){
        if(s_ready) return ESP_OK;
        esp_err_t err = hardware::axp2101::enable(hardware::axp2101::rail_t::display);
        if(err != ESP_OK) return err;
        vTaskDelay(pdMS_TO_TICKS(20));

        err = hardware::xl9555::set_display_enabled(true);
        if(err != ESP_OK){ rollback_hardware(); return err; }
        vTaskDelay(pdMS_TO_TICKS(20));

        spi_bus_config_t bus_cfg = {};
        bus_cfg.sclk_io_num = hardware::board::pins::DISPLAY_SCK;
        bus_cfg.data0_io_num = hardware::board::pins::DISPLAY_D0;
        bus_cfg.data1_io_num = hardware::board::pins::DISPLAY_D1;
        bus_cfg.data2_io_num = hardware::board::pins::DISPLAY_D2;
        bus_cfg.data3_io_num = hardware::board::pins::DISPLAY_D3;
        bus_cfg.data4_io_num = -1;
        bus_cfg.data5_io_num = -1;
        bus_cfg.data6_io_num = -1;
        bus_cfg.data7_io_num = -1;
        bus_cfg.max_transfer_sz = dma_stage_bytes;

        err = spi_bus_initialize(kSpiHost, &bus_cfg, SPI_DMA_CH_AUTO);
        if(err != ESP_OK){ rollback_hardware(); return err; }
        spi_ready = true;

        esp_lcd_panel_io_spi_config_t io_cfg = {};
        io_cfg.cs_gpio_num = hardware::board::pins::DISPLAY_CS;
        io_cfg.dc_gpio_num = GPIO_NUM_NC;
        io_cfg.spi_mode = 0;
        io_cfg.pclk_hz = spi_clock_hz;
        // Single queued transfer; use internal DMA RAM, never direct PSRAM DMA.
        io_cfg.trans_queue_depth = 1;
        io_cfg.flags.psram_dma_direct = false;
        io_cfg.lcd_cmd_bits = 32;
        io_cfg.lcd_param_bits = 8;
        io_cfg.flags.quad_mode = true;
        err = esp_lcd_new_panel_io_spi(static_cast<esp_lcd_spi_bus_handle_t>(kSpiHost), &io_cfg, &s_io);
        if(err != ESP_OK){ rollback_hardware(); return err; }

        co5300_vendor_config_t vendor = {};
        vendor.init_cmds = kInit;
        vendor.init_cmds_size = sizeof(kInit) / sizeof(kInit[0]);
        vendor.flags.use_qspi_interface = true;

        esp_lcd_panel_dev_config_t panel_cfg = {};
        panel_cfg.reset_gpio_num = hardware::board::pins::DISPLAY_RST;
        panel_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_cfg.bits_per_pixel = 16;
        panel_cfg.vendor_config = &vendor;

        err = esp_lcd_new_panel_co5300(s_io, &panel_cfg, &s_panel);
        if(err == ESP_OK) err = esp_lcd_panel_reset(s_panel);
        if(err == ESP_OK) err = esp_lcd_panel_init(s_panel);
        if(err == ESP_OK) err = esp_lcd_panel_set_gap(s_panel, x_gap, 0);
        if(err == ESP_OK) err = esp_lcd_panel_disp_on_off(s_panel, true);
        if(err == ESP_OK) err = esp_lcd_panel_co5300_set_brightness(s_panel, initial_brightness);
        if(err != ESP_OK){ rollback_hardware(); return err; }

        screen_enabled = true;
        output_hidden_for_redraw = false;
        s_brightness = initial_brightness;
        dma_stage = static_cast<uint8_t *>(heap_caps_aligned_alloc( 64, dma_stage_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
        if(!dma_stage){
            ESP_LOGE(TAG, "Unable to reserve internal display DMA buffer");
            rollback_hardware();
            return ESP_ERR_NO_MEM;
        }
        s_ready = true;
        ESP_LOGI(TAG, "Panel ready: %ux%u portrait, QSPI %d MHz, %zu-byte DMA stage", static_cast<unsigned>(width), static_cast<unsigned>(height), spi_clock_hz / 1000000, dma_stage_bytes);
        return ESP_OK;
    }

    // ------------------------------------------------------------------------
    // Display power and wake restoration
    // ------------------------------------------------------------------------

    esp_err_t set_screen_enabled(bool enabled, bool defer_display_on){
        if(!s_ready || s_panel == nullptr) return ESP_ERR_INVALID_STATE;
        if(screen_enabled == enabled){
            // A retry of an interrupted wake should not leave the panel blank.
            if(enabled && !defer_display_on && output_hidden_for_redraw) return reveal_after_full_refresh();
            return ESP_OK;
        }

        if(!enabled){
            esp_err_t err = esp_lcd_panel_disp_on_off(s_panel, false);
            if(err != ESP_OK) return err;
#if defined(CONFIG_MCW_AMOLED_DEEP_STANDBY)
            // CO5300 driver v2.2.0 sends SLPIN and deep-standby, with the
            // 120 ms settle delay. Keep ALDO2 and the SPI panel handle alive.
            err = esp_lcd_panel_disp_sleep(s_panel, true);
            if(err != ESP_OK){
                // Best effort: leave the watch usable if standby failed.
                (void)esp_lcd_panel_disp_sleep(s_panel, false);
                (void)esp_lcd_panel_co5300_set_brightness(s_panel, s_brightness);
                (void)esp_lcd_panel_disp_on_off(s_panel, true);
                ESP_LOGW(TAG, "AMOLED deep standby failed: %s", esp_err_to_name(err));
                return err;
            }
#endif
            screen_enabled = false;
            output_hidden_for_redraw = false;
            return ESP_OK;
        }

#if defined(CONFIG_MCW_AMOLED_DEEP_STANDBY)
        // The driver resets and reinitializes the panel after deep standby.
        esp_err_t err = esp_lcd_panel_disp_sleep(s_panel, false);
        if(err != ESP_OK) return err;
#else
        esp_err_t err = ESP_OK;
#endif
        if(defer_display_on){
            // CO5300's deep-standby init table issues DISPON with brightness=0.
            // Immediately force output off *before* restoring brightness. No
            // picture can appear until after the complete LVGL frame DMA drain.
            err = esp_lcd_panel_disp_on_off(s_panel, false);
            if(err == ESP_OK){
                screen_enabled = true;
                output_hidden_for_redraw = true;
            }
            return err;
        }
        err = esp_lcd_panel_co5300_set_brightness(s_panel, s_brightness);
        if(err != ESP_OK) return err;
        err = esp_lcd_panel_disp_on_off(s_panel, true);
        if(err == ESP_OK){
            screen_enabled = true;
            output_hidden_for_redraw = false;
        }
        return err;
    }

    esp_err_t reveal_after_full_refresh(){
        if(!s_ready || s_panel == nullptr || !screen_enabled || !output_hidden_for_redraw) return ESP_ERR_INVALID_STATE;

        // Important: ESP-IDF 6.1.1 panel_io_spi_tx_param drains every queued
        // tx_color DMA before issuing this display-on command. Thus the panel
        // stays blank until ALL pixel data submitted by lv_refr_now has arrived.
        // The caller holds the LVGL mutex to prevent another frame interleaving.
        esp_err_t err = esp_lcd_panel_disp_on_off(s_panel, true);
        if(err != ESP_OK) return err;
        err = esp_lcd_panel_co5300_set_brightness(s_panel, s_brightness);
        if(err == ESP_OK){
            output_hidden_for_redraw = false;
        }
        return err;
    }

    // ------------------------------------------------------------------------
    // Synchronous RGB565 transfers
    // ------------------------------------------------------------------------

    esp_err_t draw_rgb565(int x_start, int y_start, int x_end, int y_end, const void *color_map){
        // Source is packed RGB565 in panel byte order; caller serializes drawing.
        if(x_start < 0 || y_start < 0 || x_end > width || y_end > height) return ESP_ERR_INVALID_ARG;
        if(!dma_stage || !s_panel || !color_map || x_end <= x_start || y_end <= y_start){
            return ESP_ERR_INVALID_ARG;
        }
        const size_t row_bytes = static_cast<size_t>(x_end - x_start) * 2U;
        if(row_bytes > dma_stage_bytes) return ESP_ERR_INVALID_SIZE;
        const int max_rows = static_cast<int>(dma_stage_bytes / row_bytes);
        const uint8_t *src = static_cast<const uint8_t *>(color_map);
        for(int y = y_start; y < y_end;){
            const int remaining = y_end - y;
            const int rows = remaining < max_rows ? remaining : max_rows;
            const size_t n = static_cast<size_t>(rows) * row_bytes;
            std::memcpy(dma_stage, src + static_cast<size_t>(y - y_start) * row_bytes, n);
            esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, x_start, y, x_end, y + rows, dma_stage);
            if(err != ESP_OK){
                (void)esp_lcd_panel_io_tx_param(s_io, -1, nullptr, 0);
                return err;
            }
            // ESP-IDF 6.1.1 SPI panel IO: command=-1, zero parameters
            // acquires the bus and drains all queued TX transactions.
            // This is a real DMA fence, not a timing delay.
            err = esp_lcd_panel_io_tx_param(s_io, -1, nullptr, 0);
            if(err != ESP_OK) return err;
            y += rows;
        }
        return ESP_OK;
    }

    // ------------------------------------------------------------------------
    // Brightness and panel access
    // ------------------------------------------------------------------------

    esp_err_t set_brightness(uint8_t percentage){
        if(!s_ready || s_panel == nullptr) return ESP_ERR_INVALID_STATE;
        if(percentage > 100) return ESP_ERR_INVALID_ARG;
        const esp_err_t err = esp_lcd_panel_co5300_set_brightness(s_panel, percentage);
        if(err == ESP_OK) s_brightness = percentage;
        return err;
    }

    uint8_t brightness(){ return s_brightness; }

    bool ready(){ return s_ready; }
    esp_lcd_panel_handle_t panel(){ return s_panel; }
    esp_lcd_panel_io_handle_t panel_io(){ return s_io; }

    // ------------------------------------------------------------------------
    // Display shutdown
    // ------------------------------------------------------------------------

    // Caller must stop all rendering before shutdown.
    void shutdown(){
        rollback_hardware();
    }
} // namespace hardware::co5300
