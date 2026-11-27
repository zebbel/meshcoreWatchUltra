#include "Notifications.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "DRV2605.h"
#include "MeshCorePort.h"
#include "Storage.h"
#include "MentionMatch.h"

namespace services::notifications {
    namespace {
        constexpr const char *TAG = "Notifications";
        constexpr const char *kNamespace = "notifications";
        constexpr int64_t pending_lifetime_us = 10000000;
        constexpr uint8_t private_effect = 16; // 1000 ms alert, 100%.
        constexpr uint8_t channel_effect = 14; // Strong buzz, 100%.
        ChannelMode s_channel = ChannelMode::enabled;
        bool s_private = true;
        bool s_loaded = false;
        bool registered = false;
        int64_t private_until = 0;
        int64_t channel_until = 0;

        // Separate NVS settings; do not alter the MeshCore SD settings format.
        void load(){
            if(s_loaded) return;
            s_loaded = true;
            hardware::storage::Handle h{};
            esp_err_t err = hardware::storage::open(kNamespace, hardware::storage::Access::read_only, h);
            if(err != ESP_OK){
                if(!hardware::storage::is_not_found(err)) ESP_LOGW(TAG, "Settings unavailable: %s", esp_err_to_name(err));
                return;
            }
            uint8_t value = 0;
            size_t size = sizeof(value);
            err = hardware::storage::read_blob(h, "channel_mode", &value, &size);
            if(err == ESP_OK && size == 1 && value <= 2) s_channel = static_cast<ChannelMode>(value);
            size = sizeof(value);
            err = hardware::storage::read_blob(h, "private", &value, &size);
            if(err == ESP_OK && size == 1 && value <= 1) s_private = value != 0;
            hardware::storage::close(h);
        }

        esp_err_t save(const char *key, uint8_t value){
            hardware::storage::Handle h{};
            esp_err_t err = hardware::storage::open(kNamespace, hardware::storage::Access::read_write, h);
            if(err != ESP_OK) return err;
            err = hardware::storage::write_blob(h, key, &value, sizeof(value));
            if(err == ESP_OK) err = hardware::storage::commit(h);
            hardware::storage::close(h);
            return err;
        }

        // The receive callback only records a bounded pending event. No I2C or NVS.
        void received(bool channel, const char *name, const uint8_t *text, size_t length){
            if(!hardware::drv2605::ready() || !text || length == 0) return;
            const int64_t expires = esp_timer_get_time() + pending_lifetime_us;
            if(!channel){
                if(s_private) private_until = expires;
                return;
            }
            if(s_channel == ChannelMode::enabled || (s_channel == ChannelMode::mentions_only && detail::mentions(text, length, name))) channel_until = expires;
        }
    }

    ChannelMode channel_mode(){ load(); return s_channel; }
    bool private_enabled(){ load(); return s_private; }

    esp_err_t set_channel_mode(ChannelMode mode){
        if(static_cast<uint8_t>(mode) > 2) return ESP_ERR_INVALID_ARG;
        load();
        if(mode == s_channel) return ESP_OK;
        const esp_err_t err = save("channel_mode", static_cast<uint8_t>(mode));
        if(err == ESP_OK){ s_channel = mode; channel_until = 0; }
        return err;
    }

    esp_err_t set_private_enabled(bool enabled){
        load();
        if(enabled == s_private) return ESP_OK;
        const esp_err_t err = save("private", enabled ? 1 : 0);
        if(err == ESP_OK){ s_private = enabled; private_until = 0; }
        return err;
    }

    void loop(){
        load();
        if(!registered) registered = meshcore::set_message_callback(received);
        if(!hardware::drv2605::ready()){ private_until = 0; channel_until = 0; return; }
        const int64_t now = esp_timer_get_time();
        if(private_until <= now) private_until = 0;
        if(channel_until <= now) channel_until = 0;
        if(hardware::drv2605::busy()) return;
        if(!private_until && !channel_until) return;
        const bool direct = private_until != 0;
        if(direct) private_until = 0;
        else channel_until = 0;
        const esp_err_t err = hardware::drv2605::play_effect(direct ? private_effect : channel_effect);
        if(err == ESP_OK) ESP_LOGI(TAG, "%s message vibration", direct ? "Private" : "Channel");
        else ESP_LOGW(TAG, "Haptic playback failed: %s", esp_err_to_name(err));
    }
}
