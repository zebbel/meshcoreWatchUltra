#include "Settings.h"
#include <string.h>
#include "esp_log.h"
#include "MeshCoreSD.h"

namespace meshcore {
    static const char *TAG = "Settings";
    static constexpr const char *PATH = "/sdcard/meshcore/settings.bin";
    static constexpr uint32_t MAGIC = 0x4D435346U;

    hardware::lora::config_t Settings::radio() const{
        hardware::lora::config_t cfg;
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

    bool Settings::valid() const{
        if(magic != MAGIC_SETTINGS || version != VERSION) return false;
        const uint64_t center = static_cast<uint64_t>(freq_khz) * 1000ULL;
        if(center < 869400000ULL || center > 869650000ULL) return false;
        if(center < 869400000ULL + bandwidth_hz / 2 || center + bandwidth_hz / 2 > 869650000ULL) return false;
        if(bandwidth_hz != 62500 && bandwidth_hz != 125000 && bandwidth_hz != 250000 && bandwidth_hz != 500000) return false;
        if(sf < 7 || sf > 12 || cr < 5 || cr > 8) return false;
        if(tx_power_dbm < -9 || tx_power_dbm > 14) return false;
        if(rx_delay_milli > 20000 || airtime_factor_milli < 9000 || airtime_factor_milli > 20000) return false;
        if(latitude_micro < -90000000 || latitude_micro > 90000000 || longitude_micro < -180000000 || longitude_micro > 180000000) return false;
        if((telemetry_modes & 0xC0) != 0 || advert_loc_policy > 2 || multi_acks > 4) return false;
        if(path_hash_mode > 2 || autoadd_max_hops > 64) return false;
        if(memchr(default_scope_name, 0, sizeof(default_scope_name)) == nullptr) return false;
        return true;
    }

    esp_err_t SettingsStore::load(){
        Settings persisted = {};
        esp_err_t err = sdstore::read(PATH, MAGIC, persisted);
        if(err == ESP_ERR_NOT_FOUND){
            ESP_LOGI(TAG, "Creating MeshCore settings defaults on SD");
            return save(Settings{});
        }
        if(err != ESP_OK) return err;
        if(!persisted.valid()) return ESP_ERR_INVALID_CRC;
        current_ = persisted;
        return ESP_OK;
    }

    esp_err_t SettingsStore::save(const Settings &next){
        if(!next.valid()) return ESP_ERR_INVALID_ARG;
        esp_err_t err = sdstore::write(PATH, MAGIC, next);
        if(err == ESP_OK) current_ = next;
        return err;
    }
}
