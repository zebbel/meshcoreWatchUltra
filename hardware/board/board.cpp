
#include "board.h"
#include "board_pins.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace hardware::board {

    static const char *TAG = "board";

    static i2c_master_bus_handle_t i2c_handle = nullptr;
    static constexpr spi_host_device_t SPI_HOST = SPI2_HOST;
    static bool spi_initialized = false;
    static SemaphoreHandle_t spi_gate = nullptr;

    esp_err_t init(){
        if(i2c_handle != nullptr && spi_initialized){
            return ESP_OK;
        }

        ESP_LOGD(TAG, "Initializing board");

        // Initialize I2C bus
        i2c_master_bus_config_t i2c_config = {};

        i2c_config.i2c_port = I2C_NUM_0;
        i2c_config.sda_io_num = pins::I2C_SDA;
        i2c_config.scl_io_num = pins::I2C_SCL;
        i2c_config.clk_source = I2C_CLK_SRC_DEFAULT;
        i2c_config.glitch_ignore_cnt = 7;
        i2c_config.flags.enable_internal_pullup = true;

        esp_err_t err = i2c_new_master_bus(&i2c_config, &i2c_handle);

        if(err != ESP_OK){
            ESP_LOGE(TAG, "I2C initialization failed: %s", esp_err_to_name(err));
            return err;
        }

        ESP_LOGD(TAG, "I2C bus initialized");


        // Deselect all shared SPI devices before initializing the bus
        gpio_set_level(pins::LORA_CS, 1);
        // GPIO21 belongs to the SDSPI driver while mounted. Leave it
        // released/pulled up until the first SD mount, rather than reserving
        // it in board initialization and triggering GPIO ownership warnings.
        err = gpio_reset_pin(pins::SD_CS);
        if(err != ESP_OK){
            i2c_del_master_bus(i2c_handle);
            i2c_handle = nullptr;
            return err;
        }
        gpio_set_level(pins::SD_CS, 1);
        gpio_set_level(pins::NFC_CS, 1);

        gpio_config_t cs_config = {};
        cs_config.pin_bit_mask = (1ULL << pins::LORA_CS) | (1ULL << pins::NFC_CS);
        cs_config.mode = GPIO_MODE_OUTPUT;
        cs_config.pull_up_en = GPIO_PULLUP_DISABLE;
        cs_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
        cs_config.intr_type = GPIO_INTR_DISABLE;

        err = gpio_config(&cs_config);

        if(err != ESP_OK){
            ESP_LOGE(TAG, "SPI chip-select initialization failed: %s", esp_err_to_name(err));
            i2c_del_master_bus(i2c_handle);
            i2c_handle = nullptr;
            return err;
        }

        ESP_LOGD(TAG, "Shared SPI devices deselected");


        // Initialize shared SPI bus
        spi_bus_config_t spi_config = {};

        spi_config.mosi_io_num = pins::SPI_MOSI;
        spi_config.miso_io_num = pins::SPI_MISO;
        spi_config.sclk_io_num = pins::SPI_SCK;
        spi_config.quadwp_io_num = -1;
        spi_config.quadhd_io_num = -1;
        spi_config.data4_io_num = -1;
        spi_config.data5_io_num = -1;
        spi_config.data6_io_num = -1;
        spi_config.data7_io_num = -1;
        spi_config.max_transfer_sz = 4096;

        err = spi_bus_initialize(SPI_HOST, &spi_config, SPI_DMA_CH_AUTO);

        if(err != ESP_OK){
            ESP_LOGE(TAG, "SPI initialization failed: %s", esp_err_to_name(err));

            i2c_del_master_bus(i2c_handle);
            i2c_handle = nullptr;
            return err;
        }

        spi_gate = xSemaphoreCreateMutex();
        if(spi_gate == nullptr){ spi_bus_free(SPI_HOST); return ESP_ERR_NO_MEM; }

        spi_initialized = true;

        ESP_LOGD(TAG, "SPI bus initialized");
        ESP_LOGI(TAG, "Board initialization complete");

        gpio_install_isr_service(0);

        return ESP_OK;
    }

    i2c_master_bus_handle_t i2c_bus(){
        return i2c_handle;
    }

    bool spi_lock(uint32_t timeout_ticks){ return spi_gate != nullptr && xSemaphoreTake(spi_gate, static_cast<TickType_t>(timeout_ticks)) == pdTRUE; }
    void spi_unlock(){ if(spi_gate != nullptr) xSemaphoreGive(spi_gate); }

    spi_host_device_t spi_host(){
        return SPI_HOST;
    }

}
