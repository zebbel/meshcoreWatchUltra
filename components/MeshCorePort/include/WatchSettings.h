#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "SX1262.h"

namespace watch {

    // Persisted POD layout: only append fields after increasing VERSION.
    // No private identity or BLE bonds are stored here.
    struct WatchSettings {
        static constexpr uint32_t MAGIC = 0x57434836; // WCH6
        static constexpr uint16_t VERSION = 1;
        uint32_t magic = MAGIC;
        uint16_t version = VERSION;
        uint16_t reserved = 0;
        uint32_t freq_khz = 869618;
        uint32_t bandwidth_hz = 62500;
        uint32_t rx_delay_milli = 1000;
        uint32_t airtime_factor_milli = 9000;
        int32_t latitude_micro = 0;
        int32_t longitude_micro = 0;
        uint32_t ble_pin = 0; // 0 = fresh random display code at every new pairing
        uint8_t sf = 8;
        uint8_t cr = 8;
        int8_t tx_power_dbm = 10;
        uint8_t manual_contact_mode = 0;
        uint8_t telemetry_modes = 0;
        uint8_t advert_loc_policy = 0;
        uint8_t multi_acks = 0;
        uint8_t path_hash_mode = 0;
        uint8_t autoadd_config = 0;
        uint8_t autoadd_max_hops = 0;
        uint8_t reserved2 = 0;
        char default_scope_name[31] = {};
        uint8_t default_scope_key[16] = {};

        sx1262::config_t radio() const;
        bool valid() const;
    };

    class WatchSettingsStore {
    public:
        esp_err_t load();
        esp_err_t save(const WatchSettings &next);
        const WatchSettings &get() const { return current_; }
    private:
        WatchSettings current_ = {};
    };

}
