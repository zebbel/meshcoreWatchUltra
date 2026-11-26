#include "Storage.h"

#include "nvs.h"
#include "nvs_flash.h"

// NVS adapter: translate application handles and access modes into ESP-IDF NVS operations.
namespace hardware::storage {
    static nvs_open_mode_t native_mode(Access access){ return access == Access::read_write ? NVS_READWRITE : NVS_READONLY; }
    static nvs_handle_t native_handle(const Handle &handle){ return static_cast<nvs_handle_t>(handle.id); }

    esp_err_t init(){ return nvs_flash_init(); }
    esp_err_t init_partition(const char *partition){ return nvs_flash_init_partition(partition); }
    esp_err_t open(const char *name, Access access, Handle &handle){
        nvs_handle_t native = 0;
        const esp_err_t err = nvs_open(name, native_mode(access), &native);
        if(err == ESP_OK) handle.id = static_cast<uint32_t>(native);
        return err;
    }
    esp_err_t open_partition(const char *partition, const char *name, Access access, Handle &handle){
        nvs_handle_t native = 0;
        const esp_err_t err = nvs_open_from_partition(partition, name, native_mode(access), &native);
        if(err == ESP_OK) handle.id = static_cast<uint32_t>(native);
        return err;
    }
    esp_err_t read_blob(const Handle &handle, const char *key, void *value, size_t *length){ return nvs_get_blob(native_handle(handle), key, value, length); }
    esp_err_t write_blob(const Handle &handle, const char *key, const void *value, size_t length){ return nvs_set_blob(native_handle(handle), key, value, length); }
    esp_err_t read_string(const Handle &handle, const char *key, char *value, size_t *length){ return nvs_get_str(native_handle(handle), key, value, length); }
    esp_err_t write_string(const Handle &handle, const char *key, const char *value){ return nvs_set_str(native_handle(handle), key, value); }
    esp_err_t erase(const Handle &handle, const char *key){ return nvs_erase_key(native_handle(handle), key); }
    esp_err_t commit(const Handle &handle){ return nvs_commit(native_handle(handle)); }
    void close(Handle &handle){ if(handle.id != 0) nvs_close(native_handle(handle)); handle.id = 0; }
    bool is_not_found(esp_err_t error){return error == ESP_ERR_NVS_NOT_FOUND;}
}
