#include "LoRa.h"

namespace hardware::lora {
    esp_err_t init(){ return sx1262::init(); }
    esp_err_t configure(const config_t &config){ return sx1262::configure(config); }
    esp_err_t start_receive(){ return sx1262::start_receive(); }
    esp_err_t poll_receive(packet_t &packet, bool &available){ return sx1262::poll_receive(packet, available); }
    // optional IRQ-to-main notification, no ISR SPI.
    void set_wakeup_task(TaskHandle_t task){ sx1262::set_wakeup_task(task); }
    esp_err_t channel_busy(bool &busy){ return sx1262::channel_busy(busy); }
    esp_err_t start_transmit(const uint8_t *data, size_t length, uint32_t timeout_ms){ return sx1262::start_transmit(data, length, timeout_ms); }
    esp_err_t poll_transmit(bool &completed){ return sx1262::poll_transmit(completed); }
    esp_err_t finish_transmit(){ return sx1262::finish_transmit(); }
    esp_err_t abort_transmit(){ return sx1262::abort_transmit(); }
    bool is_in_receive_mode(){ return sx1262::is_in_receive_mode(); }
}
