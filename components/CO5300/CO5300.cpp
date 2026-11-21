
#include "CO5300.h"

#include "AXP2101.h"
#include "XL9555.h"
#include "board_pins.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

namespace CO5300 {

    static const char *TAG = "CO5300";

    // LilyGO portrait orientation: 410x502 with a 22-pixel column offset
    static constexpr uint16_t WIDTH = board::hardware::DISPLAY_HEIGHT;
    static constexpr uint16_t HEIGHT = board::hardware::DISPLAY_WIDTH;
    static constexpr uint16_t OFFSET_X = 22;
    static constexpr spi_host_device_t SPI_HOST = SPI3_HOST;

#if defined(CONFIG_MCW_DISPLAY_SPI_80)
    static constexpr int SPI_CLOCK_HZ = 80000000;
#elif defined(CONFIG_MCW_DISPLAY_SPI_20)
    static constexpr int SPI_CLOCK_HZ = 20000000;
#else
    static constexpr int SPI_CLOCK_HZ = 40000000;
#endif

    static spi_device_handle_t spi_handle = nullptr;
    static bool spi_bus_ready = false;
    static bool initialized = false;
    static uint8_t *line_buffer = nullptr;

    // CO5300 command with up to four parameter bytes
    struct command_t {
        uint8_t command;
        uint8_t data[4];
        uint8_t length;
        bool delay_after;
    };

    // Initialization sequence from LilyGO T-Watch Ultra reference firmware
    static constexpr command_t init_commands[] = { {0xFE, {0x00}, 1, false}, {0xC4, {0x80}, 1, false}, {0x3A, {0x55}, 1, false}, {0x35, {0x00}, 1, false}, {0x53, {0x20}, 1, false}, {0x63, {0xFF}, 1, false}, {0x2A, {0x00, 0x16, 0x01, 0xAF}, 4, false}, {0x2B, {0x00, 0x00, 0x01, 0xF5}, 4, false}, {0x11, {}, 0, true}, {0x29, {}, 0, true}, {0x51, {0x00}, 1, false} };

    // Send a command using the CO5300 0x02 command and 24-bit address format
    static esp_err_t write_command(uint8_t command, const uint8_t *data = nullptr, size_t length = 0){
        if(spi_handle == nullptr) return ESP_ERR_INVALID_STATE;
        if(length > 4 || (length != 0 && data == nullptr)) return ESP_ERR_INVALID_ARG;

        spi_transaction_t transaction = {};
        transaction.flags = SPI_TRANS_MULTILINE_CMD | SPI_TRANS_MULTILINE_ADDR;
        transaction.cmd = 0x02;
        transaction.addr = static_cast<uint32_t>(command) << 8;

        if(length > 0){
            transaction.flags |= SPI_TRANS_USE_TXDATA;
            transaction.length = length * 8;
            for(size_t i = 0; i < length; i++) transaction.tx_data[i] = data[i];
        }

        gpio_set_level(board::pins::DISPLAY_CS, 0);
        esp_err_t err = spi_device_polling_transmit(spi_handle, &transaction);
        gpio_set_level(board::pins::DISPLAY_CS, 1);

        if(err != ESP_OK){
            ESP_LOGE(TAG, "Command 0x%02X failed: %s", command, esp_err_to_name(err));
        }

        return err;
    }

    // Release SPI resources and switch the display supply off
    static void cleanup(){
        gpio_set_level(board::pins::DISPLAY_CS, 1);

        if(spi_handle != nullptr){
            spi_bus_remove_device(spi_handle);
            spi_handle = nullptr;
        }

        if(spi_bus_ready){
            spi_bus_free(SPI_HOST);
            spi_bus_ready = false;
        }

        if(line_buffer != nullptr){
            heap_caps_free(line_buffer);
            line_buffer = nullptr;
        }

        esp_err_t err = XL9555::set_display_enabled(false);
        if(err != ESP_OK) ESP_LOGW(TAG, "Display enable cleanup failed: %s", esp_err_to_name(err));

        err = AXP2101::disable(AXP2101::rail_t::display);
        if(err != ESP_OK) ESP_LOGW(TAG, "Display supply cleanup failed: %s", esp_err_to_name(err));

        initialized = false;
    }

    // Select a visible rectangle in portrait coordinates
    static esp_err_t set_window(uint16_t x, uint16_t y, uint16_t width, uint16_t height){
        uint16_t x1 = x + OFFSET_X;
        uint16_t x2 = x1 + width - 1;
        uint16_t y2 = y + height - 1;
        uint8_t columns[] = {static_cast<uint8_t>(x1 >> 8), static_cast<uint8_t>(x1), static_cast<uint8_t>(x2 >> 8), static_cast<uint8_t>(x2)};
        uint8_t rows[] = {static_cast<uint8_t>(y >> 8), static_cast<uint8_t>(y), static_cast<uint8_t>(y2 >> 8), static_cast<uint8_t>(y2)};

        esp_err_t err = write_command(0x2A, columns, sizeof(columns));
        if(err != ESP_OK) return err;

        err = write_command(0x2B, rows, sizeof(rows));
        if(err != ESP_OK) return err;

        return write_command(0x2C);
    }

    esp_err_t init(){
        if(initialized) return ESP_OK;

        ESP_LOGI(TAG, "Initializing CO5300 AMOLED");

        // Configure chip select and panel reset before enabling display power
        gpio_set_level(board::pins::DISPLAY_CS, 1);
        gpio_set_level(board::pins::DISPLAY_RST, 1);

        gpio_config_t gpio_config_data = {};
        gpio_config_data.pin_bit_mask = (1ULL << board::pins::DISPLAY_CS) | (1ULL << board::pins::DISPLAY_RST);
        gpio_config_data.mode = GPIO_MODE_OUTPUT;

        esp_err_t err = gpio_config(&gpio_config_data);
        if(err != ESP_OK) return err;

        // Supply the panel from ALDO2, then enable the board-level display signal
        err = AXP2101::enable(AXP2101::rail_t::display);
        if(err != ESP_OK) return err;
        vTaskDelay(pdMS_TO_TICKS(20));

        err = XL9555::set_display_enabled(true);
        if(err != ESP_OK){
            cleanup();
            return err;
        }
        vTaskDelay(pdMS_TO_TICKS(20));

        // Reset the display controller
        gpio_set_level(board::pins::DISPLAY_RST, 1);
        vTaskDelay(pdMS_TO_TICKS(200));
        gpio_set_level(board::pins::DISPLAY_RST, 0);
        vTaskDelay(pdMS_TO_TICKS(300));
        gpio_set_level(board::pins::DISPLAY_RST, 1);
        vTaskDelay(pdMS_TO_TICKS(200));

        // Dedicated display QSPI bus (shared radio/SD/NFC SPI remains on SPI2)
        spi_bus_config_t bus_config = {};
        bus_config.data0_io_num = board::pins::DISPLAY_D0;
        bus_config.data1_io_num = board::pins::DISPLAY_D1;
        bus_config.data2_io_num = board::pins::DISPLAY_D2;
        bus_config.data3_io_num = board::pins::DISPLAY_D3;
        bus_config.sclk_io_num = board::pins::DISPLAY_SCK;
        bus_config.data4_io_num = -1;
        bus_config.data5_io_num = -1;
        bus_config.data6_io_num = -1;
        bus_config.data7_io_num = -1;
        bus_config.max_transfer_sz = WIDTH * 2 + 8;
        bus_config.flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_GPIO_PINS;

        err = spi_bus_initialize(SPI_HOST, &bus_config, SPI_DMA_CH_AUTO);
        if(err != ESP_OK){
            ESP_LOGE(TAG, "Display QSPI bus initialization failed: %s", esp_err_to_name(err));
            cleanup();
            return err;
        }
        spi_bus_ready = true;

        spi_device_interface_config_t device_config = {};
        device_config.command_bits = 8;
        device_config.address_bits = 24;
        device_config.mode = 0;
        device_config.clock_speed_hz = SPI_CLOCK_HZ;
        device_config.spics_io_num = -1;
        device_config.flags = SPI_DEVICE_HALFDUPLEX;
        device_config.queue_size = 7;

        err = spi_bus_add_device(SPI_HOST, &device_config, &spi_handle);
        if(err != ESP_OK){
            ESP_LOGE(TAG, "Display QSPI device initialization failed: %s", esp_err_to_name(err));
            cleanup();
            return err;
        }

        line_buffer = static_cast<uint8_t *>(heap_caps_malloc(WIDTH * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
        if(line_buffer == nullptr){
            ESP_LOGE(TAG, "Unable to allocate display DMA buffer");
            cleanup();
            return ESP_ERR_NO_MEM;
        }

        // LilyGO sends the CO5300 initialization table twice
        for(int pass = 0; pass < 2; pass++){
            for(const auto &entry : init_commands){
                err = write_command(entry.command, entry.data, entry.length);
                if(err != ESP_OK){
                    cleanup();
                    return err;
                }
                if(entry.delay_after) vTaskDelay(pdMS_TO_TICKS(120));
            }
        }

        // Normal watch portrait orientation
        uint8_t madctl = 0x00;
        err = write_command(0x36, &madctl, 1);
        if(err != ESP_OK){
            cleanup();
            return err;
        }

        initialized = true;
        err = set_brightness(CONFIG_MCW_DISPLAY_BRIGHTNESS);
        if(err != ESP_OK){
            cleanup();
            return err;
        }

        ESP_LOGI(TAG, "CO5300 initialized: %ux%u, QSPI %d MHz", WIDTH, HEIGHT, SPI_CLOCK_HZ / 1000000);
        return ESP_OK;
    }

    esp_err_t set_brightness(uint8_t brightness){
        if(!initialized) return ESP_ERR_INVALID_STATE;

        esp_err_t err = write_command(0x51, &brightness, 1);
        if(err == ESP_OK) ESP_LOGI(TAG, "Display brightness: %u", brightness);
        return err;
    }

    esp_err_t fill_rect(uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint16_t color){
        if(!initialized) return ESP_ERR_INVALID_STATE;
        if(width == 0 || height == 0 || static_cast<uint32_t>(x) + width > WIDTH || static_cast<uint32_t>(y) + height > HEIGHT) return ESP_ERR_INVALID_ARG;

        // Fill the DMA buffer with RGB565 in display byte order
        for(uint16_t i = 0; i < width; i++){
            line_buffer[i * 2] = color >> 8;
            line_buffer[i * 2 + 1] = color & 0xFF;
        }

        esp_err_t err = spi_device_acquire_bus(spi_handle, portMAX_DELAY);
        if(err != ESP_OK) return err;

        err = set_window(x, y, width, height);

        if(err == ESP_OK){
            gpio_set_level(board::pins::DISPLAY_CS, 0);

            for(uint16_t row = 0; row < height; row++){
                spi_transaction_ext_t transaction = {};
                transaction.base.flags = SPI_TRANS_MODE_QIO;

                if(row == 0){
                    transaction.base.cmd = 0x32;
                    transaction.base.addr = 0x002C00;
                }else{
                    transaction.base.flags |= SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_VARIABLE_DUMMY;
                    transaction.command_bits = 0;
                    transaction.address_bits = 0;
                    transaction.dummy_bits = 0;
                }

                transaction.base.tx_buffer = line_buffer;
                transaction.base.length = width * 16;
                err = spi_device_polling_transmit(spi_handle, &transaction.base);
                if(err != ESP_OK) break;
            }

            gpio_set_level(board::pins::DISPLAY_CS, 1);
        }

        spi_device_release_bus(spi_handle);

        if(err != ESP_OK) ESP_LOGE(TAG, "Display drawing failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_err_t draw_test_pattern(){
        ESP_LOGI(TAG, "Drawing AMOLED test pattern");

        esp_err_t err = fill_rect(0, 0, WIDTH, 167, 0xF800);
        if(err != ESP_OK) return err;

        err = fill_rect(0, 167, WIDTH, 167, 0x07E0);
        if(err != ESP_OK) return err;

        err = fill_rect(0, 334, WIDTH, HEIGHT - 334, 0x001F);
        if(err != ESP_OK) return err;

        ESP_LOGI(TAG, "AMOLED test pattern complete");
        return ESP_OK;
    }

    esp_err_t shutdown(){
        if(!initialized) return ESP_OK;

        esp_err_t err = write_command(0x28);
        if(err == ESP_OK){
            vTaskDelay(pdMS_TO_TICKS(20));
            err = write_command(0x10);
            if(err == ESP_OK) vTaskDelay(pdMS_TO_TICKS(120));
        }

        cleanup();
        ESP_LOGI(TAG, "CO5300 shutdown complete");
        return err;
    }

}
