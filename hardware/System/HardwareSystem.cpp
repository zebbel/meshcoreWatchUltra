#include "HardwareSystem.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "bootloader_random.h"
#include "PCF85063A.h"
#include "esp_log.h"
#include <sys/time.h>

namespace hardware::system {
    namespace {
        constexpr uint32_t MIN_UTC = 1577836800U; // 2020-01-01
        constexpr uint32_t MAX_UTC = 4102444799U; // 2099-12-31
        constexpr int64_t GPS_SYNC_INTERVAL_US = 15LL * 60LL * 1000000LL;
        uint32_t utc_anchor = 0;
        int64_t uptime_anchor = 0;
        int64_t last_gps_sync = 0;
        bool gps_synced = false;
        bool clock_initialized = false;

        bool plausible(uint32_t utc) { return utc >= MIN_UTC && utc <= MAX_UTC; }

        void set_software_clock(uint32_t utc) {
            utc_anchor = utc;
            uptime_anchor = esp_timer_get_time();
            const timeval tv = {static_cast<time_t>(utc), 0};
            if(settimeofday(&tv, nullptr) != 0)
                ESP_LOGW("Time", "System wall clock could not be updated");
        }
    }

    esp_err_t initialize_utc_clock() {
        if(clock_initialized) return ESP_OK;
        esp_err_t err = hardware::rtc::init();
        if(err != ESP_OK) return err;
        clock_initialized = true;

        uint32_t restored = 0;
        err = hardware::rtc::read_utc(restored);
        if(err == ESP_OK) {
            set_software_clock(restored);
            ESP_LOGI("Time", "UTC restored from PCF85063A");
        } else {
            ESP_LOGW("Time", "RTC has no trusted time; waiting for GPS or companion (%s)", esp_err_to_name(err));
        }
        return ESP_OK;
    }

    uint32_t utc_time() {
        if(utc_anchor == 0) return 0;
        const int64_t elapsed = (esp_timer_get_time() - uptime_anchor) / 1000000LL;
        const int64_t current = static_cast<int64_t>(utc_anchor) + elapsed;
        return current >= MIN_UTC && current <= MAX_UTC ? static_cast<uint32_t>(current) : 0;
    }

    esp_err_t set_utc_time(uint32_t epoch_seconds) {
        if(!plausible(epoch_seconds)) return ESP_ERR_INVALID_ARG;
        set_software_clock(epoch_seconds);
        // Do not lose the software time if the RTC is temporarily unavailable.
        return hardware::rtc::write_utc(epoch_seconds);
    }

    esp_err_t sync_gps_utc(uint32_t epoch_seconds) {
        if(!plausible(epoch_seconds)) return ESP_ERR_INVALID_ARG;
        const int64_t now_us = esp_timer_get_time();
        if(gps_synced && now_us - last_gps_sync < GPS_SYNC_INTERVAL_US) return ESP_OK;
        gps_synced = true;
        last_gps_sync = now_us;

        const int64_t drift = static_cast<int64_t>(utc_time()) - epoch_seconds;
        if(utc_anchor != 0 && drift >= -1 && drift <= 1) return ESP_OK;

        const esp_err_t err = set_utc_time(epoch_seconds);
        if(err == ESP_OK) ESP_LOGI("Time", "UTC synchronized from GNSS RMC");
        else ESP_LOGW("Time", "GNSS time active; RTC persistence failed: %s", esp_err_to_name(err));
        return err;
    }
    int64_t micros(){ return esp_timer_get_time(); }
    uint32_t millis(){ return static_cast<uint32_t>(esp_timer_get_time() / 1000LL); }
    void random_bytes(uint8_t *out, size_t length){ if(out != nullptr && length != 0) esp_fill_random(out, length); }
    void enable_boot_random(){ bootloader_random_enable(); }
    void disable_boot_random(){ bootloader_random_disable(); }
    void restart(){ esp_restart(); }

}
