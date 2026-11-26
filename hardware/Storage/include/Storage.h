#pragma once

#include "esp_err.h"

#include <stddef.h>
#include <stdint.h>

namespace hardware::storage {
    enum class Access : uint8_t { read_only, read_write };
    struct Handle { uint32_t id = 0; };

    esp_err_t init();
    esp_err_t init_partition(const char *partition);
    esp_err_t open(const char *name, Access access, Handle &handle);
    esp_err_t open_partition(const char *partition, const char *name, Access access, Handle &handle);
    esp_err_t read_blob(const Handle &handle, const char *key, void *value, size_t *length);
    esp_err_t write_blob(const Handle &handle, const char *key, const void *value, size_t length);
    esp_err_t read_string(const Handle &handle, const char *key, char *value, size_t *length);
    esp_err_t write_string(const Handle &handle, const char *key, const char *value);
    esp_err_t erase(const Handle &handle, const char *key);
    esp_err_t commit(const Handle &handle);
    void close(Handle &handle);
    bool is_not_found(esp_err_t error);
}
