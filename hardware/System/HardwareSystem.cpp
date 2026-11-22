#include "HardwareSystem.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_partition.h"
#include "bootloader_random.h"

namespace hardware::system {
    int64_t micros(){ return esp_timer_get_time(); }
    uint32_t millis(){ return static_cast<uint32_t>(esp_timer_get_time() / 1000LL); }
    void random_bytes(uint8_t *out, size_t length){ if(out != nullptr && length != 0) esp_fill_random(out, length); }
    void enable_boot_random(){ bootloader_random_enable(); }
    void disable_boot_random(){ bootloader_random_disable(); }
    void restart(){ esp_restart(); }
    uint32_t storage_partition_kib(){
        const esp_partition_t *partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "storage");
        return partition == nullptr ? 0 : partition->size / 1024U;
    }
}
