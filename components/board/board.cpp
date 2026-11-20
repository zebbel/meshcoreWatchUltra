
#include "board.h"
#include "board_pins.h"

#include "esp_log.h"

namespace board {

    static const char *TAG = "board";

    static i2c_master_bus_handle_t i2c_handle = nullptr;
    static constexpr spi_host_device_t SPI_HOST = SPI2_HOST;
    static bool spi_initialized = false;

    esp_err_t init(){
        if(i2c_handle != nullptr && spi_initialized){
            return ESP_OK;
        }

        ESP_LOGI(TAG, "Initializing board");

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
            ESP_LOGE(TAG, "I2C initialization failed: %s",
                    esp_err_to_name(err));
            return err;
        }

        ESP_LOGI(TAG, "I2C bus initialized");

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

        spi_initialized = true;

        ESP_LOGI(TAG, "SPI bus initialized");
        ESP_LOGI(TAG, "Board initialization complete");

        return ESP_OK;
    }

    i2c_master_bus_handle_t i2c_bus(){
        return i2c_handle;
    }

    spi_host_device_t spi_host(){
        return SPI_HOST;
    }

    esp_err_t probe_i2c(uint8_t address){
        if(i2c_handle == nullptr){
            return ESP_ERR_INVALID_STATE;
        }

        return i2c_master_probe(i2c_handle, address, 100);
    }

}
