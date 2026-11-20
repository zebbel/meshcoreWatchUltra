#include "WatchSettings.h"

#include <string.h>
#include "esp_log.h"
#include "nvs.h"

namespace watch {

    static const char *TAG = "WatchSettings";
    static constexpr char NAMESPACE[] = "meshcore";
    static constexpr char KEY[] = "device_cfg6";

    sx1262::config_t WatchSettings::radio() const{
        sx1262::config_t cfg;
        cfg.frequency_hz = freq_khz * 1000U;
        cfg.bandwidth_hz = bandwidth_hz;
        cfg.spreading_factor = sf;
        cfg.coding_rate = cr;
        cfg.tx_power_dbm = tx_power_dbm;
        cfg.sync_word = 0x12;
        cfg.preamble_symbols = 32;
        cfg.crc_enabled = true;
        return cfg;
    }

    bool WatchSettings::valid() const{
        if(magic != MAGIC || version != VERSION) return false;
        // Constrain the entire occupied signal to the 869.4-869.65 MHz test sub-band.
        // Do not interpret this as a substitute for local spectrum compliance checks.
        const uint64_t center = static_cast<uint64_t>(freq_khz) * 1000ULL;
        if(center < 869400000ULL || center > 869650000ULL) return false;
        if(center < 869400000ULL + bandwidth_hz / 2 ||
           center + bandwidth_hz / 2 > 869650000ULL) return false;
        if(bandwidth_hz != 62500 && bandwidth_hz != 125000 &&
           bandwidth_hz != 250000 && bandwidth_hz != 500000) return false;
        if(sf < 7 || sf > 12 || cr < 5 || cr > 8) return false;
        if(tx_power_dbm < -9 || tx_power_dbm > 14) return false;
        if(rx_delay_milli > 20000 || airtime_factor_milli < 9000 || airtime_factor_milli > 20000) return false;
        if(latitude_micro < -90000000 || latitude_micro > 90000000 ||
           longitude_micro < -180000000 || longitude_micro > 180000000) return false;
        if(ble_pin != 0 && (ble_pin < 100000 || ble_pin > 999999)) return false;
        if((telemetry_modes & 0xC0) != 0 || advert_loc_policy > 2 || multi_acks > 4) return false;
        if(path_hash_mode > 2 || autoadd_max_hops > 64) return false;
        if(memchr(default_scope_name, 0, sizeof(default_scope_name)) == nullptr) return false;
        return true;
    }

    esp_err_t WatchSettingsStore::load(){
        nvs_handle_t h;
        esp_err_t err = nvs_open(NAMESPACE, NVS_READONLY, &h);
        if(err != ESP_OK) return err;
        WatchSettings persisted = {};
        size_t length = sizeof(persisted);
        err = nvs_get_blob(h, KEY, &persisted, &length);
        nvs_close(h);
        if(err == ESP_ERR_NVS_NOT_FOUND){
            ESP_LOGI(TAG, "No saved device settings; using existing M5 radio profile");
            current_ = WatchSettings{};
            return ESP_OK;
        }
        if(err != ESP_OK) return err;
        if(length != sizeof(persisted) || !persisted.valid()){
            ESP_LOGE(TAG, "Invalid settings blob; refusing to overwrite it");
            return ESP_ERR_INVALID_CRC;
        }
        current_ = persisted;
        ESP_LOGI(TAG, "Settings: %lu kHz BW=%lu SF=%u CR=4/%u TX=%d dBm",
                 static_cast<unsigned long>(current_.freq_khz),
                 static_cast<unsigned long>(current_.bandwidth_hz),
                 current_.sf, current_.cr, current_.tx_power_dbm);
        return ESP_OK;
    }

    esp_err_t WatchSettingsStore::save(const WatchSettings &next){
        if(!next.valid()) return ESP_ERR_INVALID_ARG;
        nvs_handle_t h;
        esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &h);
        if(err != ESP_OK) return err;
        err = nvs_set_blob(h, KEY, &next, sizeof(next));
        if(err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
        if(err == ESP_OK) current_ = next;
        return err;
    }

}
