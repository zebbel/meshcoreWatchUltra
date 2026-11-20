#include <stdio.h>
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "MeshCoreWatch";

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "LilyGO T-Watch Ultra started");
    ESP_LOGI(TAG, "ESP-IDF is running");

    size_t psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);

    ESP_LOGI(TAG, "PSRAM detected: %u bytes", (unsigned int)psram);
}
