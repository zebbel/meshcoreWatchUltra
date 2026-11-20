
#include "SX1262.h"

#include <string.h>

#include "AXP2101.h"
#include "XL9555.h"
#include "board.h"
#include "board_pins.h"

#include "driver/gpio.h"
#include "esp_attr.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

namespace sx1262 {

    static const char *TAG = "SX1262";

    static constexpr uint8_t CMD_GET_STATUS          = 0xC0;
    static constexpr uint8_t CMD_READ_REGISTER       = 0x1D;
    static constexpr uint8_t CMD_WRITE_REGISTER      = 0x0D;
    static constexpr uint8_t CMD_GET_DEVICE_ERRORS   = 0x17;
    static constexpr uint8_t CMD_CLEAR_DEVICE_ERRORS = 0x07;
    static constexpr uint8_t CMD_SET_STANDBY         = 0x80;
    static constexpr uint8_t CMD_SET_DIO3_TCXO       = 0x97;
    static constexpr uint8_t CMD_SET_DIO2_RF_SWITCH  = 0x9D;
    static constexpr uint8_t CMD_SET_PACKET_TYPE     = 0x8A;
    static constexpr uint8_t CMD_SET_RF_FREQUENCY    = 0x86;
    static constexpr uint8_t CMD_CALIBRATE_IMAGE     = 0x98;
    static constexpr uint8_t CMD_SET_MODULATION      = 0x8B;
    static constexpr uint8_t CMD_SET_PACKET_PARAMS  = 0x8C;
    static constexpr uint8_t CMD_SET_BUFFER_BASE     = 0x8F;
    static constexpr uint8_t CMD_SET_PA_CONFIG      = 0x95;
    static constexpr uint8_t CMD_SET_TX_PARAMS      = 0x8E;
    static constexpr uint8_t CMD_SET_DIO_IRQ_PARAMS = 0x08;
    static constexpr uint8_t CMD_GET_IRQ_STATUS     = 0x12;
    static constexpr uint8_t CMD_CLEAR_IRQ_STATUS   = 0x02;
    static constexpr uint8_t CMD_GET_RX_BUFFER      = 0x13;
    static constexpr uint8_t CMD_GET_PACKET_STATUS  = 0x14;
    static constexpr uint8_t CMD_READ_BUFFER        = 0x1E;
    static constexpr uint8_t CMD_WRITE_BUFFER       = 0x0E;
    static constexpr uint8_t CMD_SET_RX             = 0x82;
    static constexpr uint8_t CMD_SET_TX             = 0x83;

    static constexpr uint16_t REG_VERSION = 0x0320;
    static constexpr uint16_t REG_SYNC_WORD = 0x0740;
    static constexpr uint16_t REG_OCP = 0x08E7;

    static constexpr uint16_t IRQ_TX_DONE = 0x0001;
    static constexpr uint16_t IRQ_RX_DONE = 0x0002;
    static constexpr uint16_t IRQ_HEADER_ERR = 0x0020;
    static constexpr uint16_t IRQ_CRC_ERR = 0x0040;
    static constexpr uint16_t IRQ_TIMEOUT = 0x0200;
    static constexpr uint16_t IRQ_MASK = IRQ_TX_DONE | IRQ_RX_DONE | IRQ_HEADER_ERR | IRQ_CRC_ERR | IRQ_TIMEOUT;

    static constexpr uint32_t BUSY_TIMEOUT_MS = 200;
    static constexpr size_t MAX_TRANSACTION = 260;

#if defined(CONFIG_MCW_SX1262_SPI_8M)
    static constexpr int SPI_CLOCK_HZ = 8000000;
#else
    static constexpr int SPI_CLOCK_HZ = 4000000;
#endif

    static spi_device_handle_t spi_handle = nullptr;
    static SemaphoreHandle_t radio_mutex = nullptr;
    static SemaphoreHandle_t irq_semaphore = nullptr;
    static uint8_t *spi_tx = nullptr;
    static uint8_t *spi_rx = nullptr;
    static bool irq_registered = false;
    static bool initialized = false;
    static bool configured = false;
    static bool receiving = false;
    static config_t current_config;

    static void IRAM_ATTR dio1_isr(void *arg){
        (void)arg;
        if(irq_semaphore == nullptr) return;

        BaseType_t higher_priority_task_woken = pdFALSE;
        xSemaphoreGiveFromISR(irq_semaphore, &higher_priority_task_woken);
        if(higher_priority_task_woken) portYIELD_FROM_ISR();
    }

    static esp_err_t lock_radio(){
        if(radio_mutex == nullptr) return ESP_ERR_INVALID_STATE;
        return xSemaphoreTake(radio_mutex, portMAX_DELAY) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
    }

    static esp_err_t wait_busy(){
        TickType_t started = xTaskGetTickCount();

        while(gpio_get_level(board::pins::LORA_BUSY) != 0){
            if(xTaskGetTickCount() - started >= pdMS_TO_TICKS(BUSY_TIMEOUT_MS)){
                ESP_LOGE(TAG, "SX1262 BUSY timeout");
                return ESP_ERR_TIMEOUT;
            }

            vTaskDelay(1);
        }

        return ESP_OK;
    }

    // Caller holds radio_mutex, and the SPI bus is acquired while CS is low.
    static esp_err_t transfer(const uint8_t *tx, uint8_t *rx, size_t length){
        if(spi_handle == nullptr || spi_tx == nullptr || spi_rx == nullptr) return ESP_ERR_INVALID_STATE;
        if(tx == nullptr || rx == nullptr || length == 0 || length > MAX_TRANSACTION) return ESP_ERR_INVALID_ARG;

        esp_err_t err = wait_busy();
        if(err != ESP_OK) return err;

        memset(spi_tx, 0, length);
        memset(spi_rx, 0, length);
        memcpy(spi_tx, tx, length);

        spi_transaction_t transaction = {};
        transaction.length = length * 8;
        transaction.tx_buffer = spi_tx;
        transaction.rx_buffer = spi_rx;

        err = spi_device_acquire_bus(spi_handle, portMAX_DELAY);
        if(err != ESP_OK) return err;

        gpio_set_level(board::pins::LORA_CS, 0);
        err = spi_device_polling_transmit(spi_handle, &transaction);
        gpio_set_level(board::pins::LORA_CS, 1);
        spi_device_release_bus(spi_handle);

        if(err != ESP_OK) return err;

        memcpy(rx, spi_rx, length);
        return wait_busy();
    }

    static esp_err_t command(const uint8_t *tx, size_t length){
        uint8_t rx[MAX_TRANSACTION] = {};
        return transfer(tx, rx, length);
    }

    static esp_err_t write_register(uint16_t address, const uint8_t *data, size_t length){
        if(length == 0 || length > 255 || data == nullptr) return ESP_ERR_INVALID_ARG;

        uint8_t tx[MAX_TRANSACTION] = {CMD_WRITE_REGISTER, static_cast<uint8_t>(address >> 8), static_cast<uint8_t>(address)};
        memcpy(&tx[3], data, length);
        return command(tx, length + 3);
    }

    static esp_err_t read_version(char (&version)[7]){
        uint8_t tx[10] = {CMD_READ_REGISTER, static_cast<uint8_t>(REG_VERSION >> 8), static_cast<uint8_t>(REG_VERSION), 0};
        uint8_t rx[10] = {};

        esp_err_t err = transfer(tx, rx, sizeof(tx));
        if(err != ESP_OK) return err;

        memcpy(version, &rx[4], 6);
        version[6] = '\0';

        return memcmp(version, "SX126", 5) == 0 ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
    }

    static esp_err_t get_irq(uint16_t &flags){
        uint8_t tx[4] = {CMD_GET_IRQ_STATUS, 0, 0, 0};
        uint8_t rx[4] = {};

        esp_err_t err = transfer(tx, rx, sizeof(tx));
        if(err != ESP_OK) return err;

        flags = (static_cast<uint16_t>(rx[2]) << 8) | rx[3];
        return ESP_OK;
    }

    static esp_err_t clear_irq(uint16_t flags){
        uint8_t cmd[3] = {CMD_CLEAR_IRQ_STATUS, static_cast<uint8_t>(flags >> 8), static_cast<uint8_t>(flags)};
        return command(cmd, sizeof(cmd));
    }

    static void drain_irq(){
        while(xSemaphoreTake(irq_semaphore, 0) == pdTRUE){}
    }

    static esp_err_t set_packet_length(uint8_t length){
        uint8_t cmd[7] = {
            CMD_SET_PACKET_PARAMS,
            static_cast<uint8_t>(current_config.preamble_symbols >> 8),
            static_cast<uint8_t>(current_config.preamble_symbols),
            0x00, // Explicit header
            length,
            static_cast<uint8_t>(current_config.crc_enabled ? 0x01 : 0x00),
            0x00  // Standard IQ
        };
        return command(cmd, sizeof(cmd));
    }

    static esp_err_t start_rx_locked(){
        uint8_t standby[2] = {CMD_SET_STANDBY, 0x01};
        esp_err_t err = command(standby, sizeof(standby));
        if(err != ESP_OK) return err;

        err = set_packet_length(255);
        if(err != ESP_OK) return err;

        err = clear_irq(0xFFFF);
        if(err != ESP_OK) return err;
        drain_irq();

        uint8_t cmd[4] = {CMD_SET_RX, 0xFF, 0xFF, 0xFF}; // Continuous RX
        err = command(cmd, sizeof(cmd));
        if(err == ESP_OK) receiving = true;
        return err;
    }

    // After an IRQ, reading the latched IRQ flags also handles a missed DIO1 edge.
    static esp_err_t wait_irq(uint16_t mask, uint32_t timeout_ms, uint16_t &flags){
        TickType_t start = xTaskGetTickCount();
        TickType_t max_ticks = pdMS_TO_TICKS(timeout_ms);

        while(true){
            esp_err_t err = get_irq(flags);
            if(err != ESP_OK) return err;
            if((flags & mask) != 0) return ESP_OK;

            TickType_t elapsed = xTaskGetTickCount() - start;
            if(elapsed >= max_ticks) return ESP_ERR_TIMEOUT;

            TickType_t remaining = max_ticks - elapsed;
            TickType_t delay_ticks = pdMS_TO_TICKS(50);
            if(delay_ticks == 0) delay_ticks = 1;
            if(delay_ticks > remaining) delay_ticks = remaining;
            xSemaphoreTake(irq_semaphore, delay_ticks);
        }
    }

    // Called while holding radio_mutex.
    static esp_err_t read_status_locked(status_t &status){
        status = status_t{};

        uint8_t tx_status[2] = {CMD_GET_STATUS, 0};
        uint8_t rx_status[2] = {};
        esp_err_t err = transfer(tx_status, rx_status, sizeof(tx_status));
        if(err != ESP_OK) return err;

        status.raw = rx_status[1];
        status.mode = status.raw & 0x70;
        status.command_status = status.raw & 0x0E;

        if(status.mode != 0x20 && status.mode != 0x30 && status.mode != 0x40 && status.mode != 0x50 && status.mode != 0x60) return ESP_ERR_INVALID_RESPONSE;

        uint8_t tx_errors[4] = {CMD_GET_DEVICE_ERRORS, 0, 0, 0};
        uint8_t rx_errors[4] = {};
        err = transfer(tx_errors, rx_errors, sizeof(tx_errors));
        if(err != ESP_OK) return err;

        status.device_errors = (static_cast<uint16_t>(rx_errors[2]) << 8) | rx_errors[3];
        status.busy = gpio_get_level(board::pins::LORA_BUSY) != 0;
        status.dio1 = gpio_get_level(board::pins::LORA_DIO1) != 0;
        return ESP_OK;
    }

    static esp_err_t configure_oscillator(){
        ESP_LOGI(TAG, "Configuring SX1262 oscillator");

        uint8_t standby_rc[] = {CMD_SET_STANDBY, 0x00};
        esp_err_t err = command(standby_rc, sizeof(standby_rc));
        if(err != ESP_OK) return err;

        uint8_t clear_errors[] = {CMD_CLEAR_DEVICE_ERRORS, 0x00, 0x00};
        err = command(clear_errors, sizeof(clear_errors));
        if(err != ESP_OK) return err;

        // DIO3 TCXO 3.0V and 5ms startup timeout (320 ticks of 15.625us)
        uint8_t tcxo_config[] = {CMD_SET_DIO3_TCXO, 0x06, 0x00, 0x01, 0x40};
        err = command(tcxo_config, sizeof(tcxo_config));
        if(err != ESP_OK) return err;

        uint8_t rf_switch[] = {CMD_SET_DIO2_RF_SWITCH, 0x01};
        err = command(rf_switch, sizeof(rf_switch));
        if(err != ESP_OK) return err;

        uint8_t standby_xosc[] = {CMD_SET_STANDBY, 0x01};
        err = command(standby_xosc, sizeof(standby_xosc));
        if(err != ESP_OK) return err;

        ESP_LOGI(TAG, "SX1262 oscillator configuration complete");
        return ESP_OK;
    }

    // The SPI2 bus is owned by board::init(); only unregister our device.
    static void cleanup(){
        if(irq_registered){
            gpio_isr_handler_remove(board::pins::LORA_DIO1);
            irq_registered = false;
        }

        if(spi_handle != nullptr){
            gpio_set_level(board::pins::LORA_CS, 1);
            gpio_set_level(board::pins::LORA_RST, 0);
            spi_bus_remove_device(spi_handle);
            spi_handle = nullptr;
        }

        if(spi_tx != nullptr){
            heap_caps_free(spi_tx);
            spi_tx = nullptr;
        }
        if(spi_rx != nullptr){
            heap_caps_free(spi_rx);
            spi_rx = nullptr;
        }
        if(irq_semaphore != nullptr){
            vSemaphoreDelete(irq_semaphore);
            irq_semaphore = nullptr;
        }

        if(radio_mutex != nullptr){
            vSemaphoreDelete(radio_mutex);
            radio_mutex = nullptr;
        }

        esp_err_t err = AXP2101::disable(AXP2101::rail_t::radio);
        if(err != ESP_OK) ESP_LOGW(TAG, "Unable to disable radio power: %s", esp_err_to_name(err));

        initialized = false;
        configured = false;
        receiving = false;
    }

    esp_err_t init(){
        if(initialized) return ESP_OK;
        ESP_LOGI(TAG, "Initializing SX1262");

        gpio_set_level(board::pins::LORA_RST, 0);
        gpio_config_t reset_config = {};
        reset_config.pin_bit_mask = 1ULL << board::pins::LORA_RST;
        reset_config.mode = GPIO_MODE_OUTPUT;
        esp_err_t err = gpio_config(&reset_config);
        if(err != ESP_OK) return err;

        gpio_config_t input_config = {};
        input_config.pin_bit_mask = (1ULL << board::pins::LORA_BUSY) | (1ULL << board::pins::LORA_DIO1);
        input_config.mode = GPIO_MODE_INPUT;
        input_config.intr_type = GPIO_INTR_DISABLE;
        err = gpio_config(&input_config);
        if(err != ESP_OK) return err;

        err = AXP2101::enable(AXP2101::rail_t::radio);
        if(err != ESP_OK) return err;
        vTaskDelay(pdMS_TO_TICKS(20));

        err = XL9555::set_lora_antenna(false);
        if(err != ESP_OK){
            cleanup();
            return err;
        }

        // GPIO36 remains controlled by board::init(), avoiding an ESP-IDF GPIO conflict.
        spi_device_interface_config_t device_config = {};
        device_config.clock_speed_hz = SPI_CLOCK_HZ;
        device_config.mode = 0;
        device_config.spics_io_num = -1;
        device_config.queue_size = 4;

        err = spi_bus_add_device(board::spi_host(), &device_config, &spi_handle);
        if(err != ESP_OK){
            ESP_LOGE(TAG, "SPI device registration failed: %s", esp_err_to_name(err));
            cleanup();
            return err;
        }

        spi_tx = static_cast<uint8_t *>(heap_caps_malloc(MAX_TRANSACTION, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
        spi_rx = static_cast<uint8_t *>(heap_caps_malloc(MAX_TRANSACTION, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
        radio_mutex = xSemaphoreCreateMutex();
        irq_semaphore = xSemaphoreCreateBinary();
        if(spi_tx == nullptr || spi_rx == nullptr || radio_mutex == nullptr || irq_semaphore == nullptr){
            ESP_LOGE(TAG, "Unable to allocate SX1262 buffers or synchronization objects");
            cleanup();
            return ESP_ERR_NO_MEM;
        }

        gpio_set_level(board::pins::LORA_RST, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(board::pins::LORA_RST, 1);
        vTaskDelay(pdMS_TO_TICKS(20));

        err = wait_busy();
        if(err != ESP_OK){
            cleanup();
            return err;
        }

        char version[7] = {};
        err = read_version(version);
        if(err != ESP_OK){
            ESP_LOGE(TAG, "SX1262 version read failed: %s", esp_err_to_name(err));
            cleanup();
            return err;
        }

        err = configure_oscillator();
        if(err != ESP_OK){
            cleanup();
            return err;
        }

        status_t status;
        err = read_status_locked(status);
        if(err != ESP_OK || status.mode != 0x30 || status.device_errors != 0 || status.command_status == 0x06 || status.command_status == 0x08 || status.command_status == 0x0A){
            ESP_LOGE(TAG, "SX1262 oscillator check failed: status 0x%02X, errors 0x%04X", status.raw, status.device_errors);
            cleanup();
            return err != ESP_OK ? err : ESP_ERR_INVALID_RESPONSE;
        }

        // Install DIO1 rising-edge notification. Do not alter any other pin ISR.
        err = gpio_install_isr_service(0);
        if(err != ESP_OK && err != ESP_ERR_INVALID_STATE){
            ESP_LOGE(TAG, "GPIO ISR service unavailable: %s", esp_err_to_name(err));
            cleanup();
            return err;
        }

        err = gpio_set_intr_type(board::pins::LORA_DIO1, GPIO_INTR_POSEDGE);
        if(err == ESP_OK) err = gpio_isr_handler_add(board::pins::LORA_DIO1, dio1_isr, nullptr);
        if(err != ESP_OK){
            ESP_LOGE(TAG, "DIO1 interrupt setup failed: %s", esp_err_to_name(err));
            cleanup();
            return err;
        }
        irq_registered = true;

        initialized = true;
        ESP_LOGI(TAG, "SX126x detected: %s, status=0x%02X, SPI=%d MHz", version, status.raw, SPI_CLOCK_HZ / 1000000);
        ESP_LOGI(TAG, "SX1262 oscillator test passed, device errors=0x%04X", status.device_errors);
        ESP_LOGI(TAG, "SX1262 hardware initialization complete (no RF transmission)");
        return ESP_OK;
    }

    esp_err_t read_status(status_t &status){
        if(!initialized) return ESP_ERR_INVALID_STATE;
        esp_err_t err = lock_radio();
        if(err != ESP_OK) return err;
        err = read_status_locked(status);
        xSemaphoreGive(radio_mutex);
        return err;
    }

    static esp_err_t calibrate_image(uint32_t frequency_hz){
        uint8_t values[2] = {};
        if(frequency_hz >= 902000000 && frequency_hz <= 928000000){
            values[0] = 0xE1; values[1] = 0xE9;
        }else if(frequency_hz >= 863000000 && frequency_hz <= 870000000){
            values[0] = 0xD7; values[1] = 0xDB;
        }else if(frequency_hz >= 779000000 && frequency_hz <= 787000000){
            values[0] = 0xC1; values[1] = 0xC5;
        }else if(frequency_hz >= 470000000 && frequency_hz <= 510000000){
            values[0] = 0x75; values[1] = 0x81;
        }else if(frequency_hz >= 430000000 && frequency_hz <= 440000000){
            values[0] = 0x6B; values[1] = 0x6F;
        }else{
            ESP_LOGE(TAG, "Frequency outside supported image-calibration ranges: %lu Hz", static_cast<unsigned long>(frequency_hz));
            return ESP_ERR_NOT_SUPPORTED;
        }

        uint8_t cmd[3] = {CMD_CALIBRATE_IMAGE, values[0], values[1]};
        return command(cmd, sizeof(cmd));
    }

    esp_err_t configure(const config_t &config){
        if(!initialized) return ESP_ERR_INVALID_STATE;
        if(config.frequency_hz == 0) return ESP_ERR_INVALID_ARG;
        if(config.spreading_factor < 5 || config.spreading_factor > 12 || config.coding_rate < 5 || config.coding_rate > 8) return ESP_ERR_INVALID_ARG;
        if(config.preamble_symbols < 6 || config.tx_power_dbm < -9 || config.tx_power_dbm > 22) return ESP_ERR_INVALID_ARG;

        uint8_t bw = 0;

        if(config.bandwidth_hz == 62500) bw = 0x03;
        else if(config.bandwidth_hz == 125000) bw = 0x04;
        else if(config.bandwidth_hz == 250000) bw = 0x05;
        else if(config.bandwidth_hz == 500000) bw = 0x06;
        else return ESP_ERR_INVALID_ARG;

        esp_err_t err = lock_radio();
        if(err != ESP_OK) return err;
        if(receiving){
            uint8_t standby[2] = {CMD_SET_STANDBY, 0x01};
            err = command(standby, sizeof(standby));
            if(err != ESP_OK){
                xSemaphoreGive(radio_mutex);
                return err;
            }
            receiving = false;
        }

        configured = false;
        uint8_t standby[2] = {CMD_SET_STANDBY, 0x01};
        err = command(standby, sizeof(standby));
        if(err != ESP_OK) goto end;

        // Calibration is specific to the operating frequency band.
        err = calibrate_image(config.frequency_hz);
        if(err != ESP_OK) goto end;

        {
            uint8_t packet_type[2] = {CMD_SET_PACKET_TYPE, 0x01}; // LoRa
            err = command(packet_type, sizeof(packet_type));
            if(err != ESP_OK) goto end;
        }

        {
            uint32_t raw = static_cast<uint32_t>((static_cast<uint64_t>(config.frequency_hz) << 25) / 32000000ULL);
            uint8_t frequency[5] = {CMD_SET_RF_FREQUENCY, static_cast<uint8_t>(raw >> 24), static_cast<uint8_t>(raw >> 16), static_cast<uint8_t>(raw >> 8), static_cast<uint8_t>(raw)};
            err = command(frequency, sizeof(frequency));
            if(err != ESP_OK) goto end;
        }

        {
            uint64_t symbol_us = (static_cast<uint64_t>(1) << config.spreading_factor) * 1000000ULL / config.bandwidth_hz;
            uint8_t modulation[5] = {CMD_SET_MODULATION, config.spreading_factor, bw, static_cast<uint8_t>(config.coding_rate - 4), static_cast<uint8_t>(symbol_us >= 16000 ? 1 : 0)};
            err = command(modulation, sizeof(modulation));
            if(err != ESP_OK) goto end;
        }

        {
            uint8_t sync[2] = {static_cast<uint8_t>((config.sync_word & 0xF0) | 0x04), static_cast<uint8_t>((config.sync_word << 4) | 0x04)};
            err = write_register(REG_SYNC_WORD, sync, sizeof(sync));
            if(err != ESP_OK) goto end;
        }

        {
            uint8_t ocp = 0x38; // 140mA over-current protection
            err = write_register(REG_OCP, &ocp, 1);
            if(err != ESP_OK) goto end;
        }

        {
            uint8_t pa[5] = {CMD_SET_PA_CONFIG, 0x04, 0x07, 0x00, 0x01}; // SX1262 high-power PA
            err = command(pa, sizeof(pa));
            if(err != ESP_OK) goto end;
        }

        {
            uint8_t tx_params[3] = {CMD_SET_TX_PARAMS, static_cast<uint8_t>(config.tx_power_dbm), 0x04}; // 200us ramp
            err = command(tx_params, sizeof(tx_params));
            if(err != ESP_OK) goto end;
        }

        {
            uint8_t buffer_base[3] = {CMD_SET_BUFFER_BASE, 0, 0};
            err = command(buffer_base, sizeof(buffer_base));
            if(err != ESP_OK) goto end;
        }

        current_config = config;
        err = set_packet_length(255);
        if(err != ESP_OK) goto end;

        {
            uint8_t irq[9] = {CMD_SET_DIO_IRQ_PARAMS, static_cast<uint8_t>(IRQ_MASK >> 8), static_cast<uint8_t>(IRQ_MASK), static_cast<uint8_t>(IRQ_MASK >> 8), static_cast<uint8_t>(IRQ_MASK), 0, 0, 0, 0};
            err = command(irq, sizeof(irq));
            if(err != ESP_OK) goto end;
        }

        err = clear_irq(0xFFFF);
        if(err != ESP_OK) goto end;
        drain_irq();
        configured = true;
        ESP_LOGI(TAG, "LoRa configured: %lu Hz, BW=%lu, SF=%u, CR=4/%u, TX=%d dBm, sync=0x%02X, CRC=%s", static_cast<unsigned long>(config.frequency_hz), static_cast<unsigned long>(config.bandwidth_hz), config.spreading_factor, config.coding_rate, config.tx_power_dbm, config.sync_word, config.crc_enabled ? "On" : "Off");

    end:
        if(err != ESP_OK) ESP_LOGE(TAG, "LoRa configuration failed: %s", esp_err_to_name(err));
        xSemaphoreGive(radio_mutex);
        return err;
    }

    esp_err_t start_receive(){
        if(!initialized || !configured) return ESP_ERR_INVALID_STATE;
        esp_err_t err = lock_radio();
        if(err != ESP_OK) return err;
        err = start_rx_locked();
        if(err == ESP_OK) ESP_LOGI(TAG, "LoRa continuous RX started");
        xSemaphoreGive(radio_mutex);
        return err;
    }

    esp_err_t receive(packet_t &packet, uint32_t timeout_ms){
        if(!initialized || !configured) return ESP_ERR_INVALID_STATE;
        esp_err_t err = lock_radio();
        if(err != ESP_OK) return err;
        if(!receiving){
            xSemaphoreGive(radio_mutex);
            return ESP_ERR_INVALID_STATE;
        }

        packet = packet_t{};
        uint16_t irq = 0;
        err = wait_irq(IRQ_RX_DONE | IRQ_CRC_ERR | IRQ_HEADER_ERR | IRQ_TIMEOUT, timeout_ms, irq);
        if(err != ESP_OK){
            xSemaphoreGive(radio_mutex);
            return err;
        }

        if((irq & (IRQ_CRC_ERR | IRQ_HEADER_ERR | IRQ_TIMEOUT)) != 0){
            ESP_LOGD(TAG, "Discarding LoRa frame, IRQ=0x%04X", irq);

            err = (irq & IRQ_TIMEOUT) ? ESP_ERR_TIMEOUT : (irq & IRQ_CRC_ERR) ? ESP_ERR_INVALID_CRC : ESP_ERR_INVALID_RESPONSE;

        }else if((irq & IRQ_RX_DONE) != 0){
            uint8_t tx_buffer_status[4] = {CMD_GET_RX_BUFFER, 0, 0, 0};
            uint8_t rx_buffer_status[4] = {};
            err = transfer(tx_buffer_status, rx_buffer_status, sizeof(tx_buffer_status));

            if(err == ESP_OK){
                uint8_t payload_length = rx_buffer_status[2];
                uint8_t payload_start = rx_buffer_status[3];
                uint8_t tx_packet[MAX_TRANSACTION] = {CMD_READ_BUFFER, payload_start, 0};
                uint8_t rx_packet[MAX_TRANSACTION] = {};

                err = transfer(tx_packet, rx_packet, static_cast<size_t>(payload_length) + 3);
                if(err == ESP_OK){
                    memcpy(packet.data, &rx_packet[3], payload_length);
                    packet.length = payload_length;

                    uint8_t tx_status[5] = {CMD_GET_PACKET_STATUS, 0, 0, 0, 0};
                    uint8_t rx_status[5] = {};
                    err = transfer(tx_status, rx_status, sizeof(tx_status));
                    if(err == ESP_OK){
                        packet.rssi_dbm = -static_cast<float>(rx_status[2]) / 2.0f;
                        packet.snr_db = static_cast<float>(static_cast<int8_t>(rx_status[3])) / 4.0f;
                    }
                }
            }
        }

        // Restart RX after consuming the packet or dropping an invalid one.
        esp_err_t restart_err = start_rx_locked();
        if(err == ESP_OK) err = restart_err;
        if(err != ESP_OK) ESP_LOGW(TAG, "LoRa receive failed: %s", esp_err_to_name(err));
        xSemaphoreGive(radio_mutex);
        return err;
    }

    esp_err_t transmit(const uint8_t *data, size_t length, uint32_t timeout_ms){
        if(!initialized || !configured) return ESP_ERR_INVALID_STATE;
        if(data == nullptr || length == 0 || length > 255 || timeout_ms == 0 || timeout_ms > 60000) return ESP_ERR_INVALID_ARG;

        esp_err_t err = lock_radio();
        if(err != ESP_OK) return err;

        bool resume_rx = receiving;
        receiving = false;
        uint8_t standby[2] = {CMD_SET_STANDBY, 0x01};
        err = command(standby, sizeof(standby));
        if(err != ESP_OK) goto restore;

        err = clear_irq(0xFFFF);
        if(err != ESP_OK) goto restore;
        drain_irq();

        err = set_packet_length(static_cast<uint8_t>(length));
        if(err != ESP_OK) goto restore;

        {
            uint8_t buffer[MAX_TRANSACTION] = {CMD_WRITE_BUFFER, 0x00};
            memcpy(&buffer[2], data, length);
            err = command(buffer, length + 2);
            if(err != ESP_OK) goto restore;
        }

        {
            uint32_t timeout_ticks = static_cast<uint32_t>(timeout_ms) * 64;
            uint8_t tx[4] = {CMD_SET_TX, static_cast<uint8_t>(timeout_ticks >> 16), static_cast<uint8_t>(timeout_ticks >> 8), static_cast<uint8_t>(timeout_ticks)};
            err = command(tx, sizeof(tx));
            if(err != ESP_OK) goto restore;
        }

        {
            uint16_t irq = 0;
            err = wait_irq(IRQ_TX_DONE | IRQ_TIMEOUT, timeout_ms + 100, irq);
            if(err == ESP_OK){
                if((irq & IRQ_TIMEOUT) != 0) err = ESP_ERR_TIMEOUT;
                else if((irq & IRQ_TX_DONE) == 0) err = ESP_ERR_INVALID_RESPONSE;
                else err = clear_irq(0xFFFF);
            }
        }

    restore:
        if(resume_rx){
            esp_err_t resume_err = start_rx_locked();
            if(err == ESP_OK) err = resume_err;
        }else{
            uint8_t standby_xosc[2] = {CMD_SET_STANDBY, 0x01};
            esp_err_t standby_err = command(standby_xosc, sizeof(standby_xosc));
            if(err == ESP_OK) err = standby_err;
        }

        if(err == ESP_OK) ESP_LOGI(TAG, "LoRa TX complete: %u bytes", static_cast<unsigned>(length));
        else ESP_LOGE(TAG, "LoRa TX failed: %s", esp_err_to_name(err));
        xSemaphoreGive(radio_mutex);
        return err;
    }

    esp_err_t shutdown(){
        if(!initialized) return ESP_OK;
        esp_err_t err = lock_radio();
        if(err != ESP_OK) return err;
        receiving = false;
        configured = false;
        uint8_t standby[2] = {CMD_SET_STANDBY, 0x00};
        err = command(standby, sizeof(standby));
        xSemaphoreGive(radio_mutex);
        cleanup();
        ESP_LOGI(TAG, "SX1262 powered off");
        return err;
    }

}
