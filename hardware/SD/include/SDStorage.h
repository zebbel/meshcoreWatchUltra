#pragma once

#include "esp_err.h"

#include <stddef.h>
#include <stdint.h>

namespace hardware::sd {
    constexpr size_t MAX_PATH = 160;

    // Call once after board, PMU and GPIO expander initialization.
    esp_err_t init();
    bool card_present();
    bool ready();
    bool writable();
    uint32_t capacity_kib(); // Last mounted card capacity; does not power up SD.

    // Scope owns the SD power rail and the shared SPI2 gate. Nested scopes on
    // the SAME FreeRTOS task are supported. All FILE*s must close before exit.
    class Session {
    public:
        Session();
        ~Session();
        void close();
        bool ok() const { return ok_; }
        esp_err_t error() const { return error_; }
        Session(const Session &) = delete;
        Session &operator=(const Session &) = delete;
    private:
        bool locked_ = false;
        bool ok_ = false;
        esp_err_t error_ = ESP_FAIL;
    };

    // Absolute path must start with /sdcard/; file I/O is wrapped in Session.
    // Recover .bak on missing main file; .tmp is not trusted on its own.
    esp_err_t read_file(const char *path, void *out, size_t capacity, size_t &length);
    esp_err_t write_file(const char *path, const void *bytes, size_t length);
    esp_err_t remove_file(const char *path);
    esp_err_t ensure_directory(const char *path);
    bool exists(const char *path);
}
