#include "DRV2605.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

#include "board.h"
#include "board_pins.h"
#include "XL9555.h"

namespace hardware::drv2605 {
    // DRV2605 register map (TI SLOS825E).
    static constexpr const char *TAG = "DRV2605";
    static constexpr uint8_t REG_STATUS = 0x00;
    static constexpr uint8_t REG_MODE = 0x01;
    static constexpr uint8_t REG_RTP = 0x02;
    static constexpr uint8_t REG_LIBRARY = 0x03;
    static constexpr uint8_t REG_WAVEFORM = 0x04;
    static constexpr uint8_t REG_GO = 0x0C;
    static constexpr uint8_t REG_OVERDRIVE = 0x0D;
    static constexpr uint8_t REG_SUSTAIN_POS = 0x0E;
    static constexpr uint8_t REG_SUSTAIN_NEG = 0x0F;
    static constexpr uint8_t REG_BRAKE = 0x10;
    static constexpr uint8_t REG_AUDIO_MAX = 0x13;
    static constexpr uint8_t REG_FEEDBACK = 0x1A;
    static constexpr uint8_t REG_CONTROL3 = 0x1D;
    static constexpr uint8_t MODE_INTERNAL = 0x00;
    static constexpr uint8_t MODE_STANDBY = 0x40;
    static constexpr uint8_t LIBRARY_ERM = 1;
    static constexpr uint8_t MAX_EFFECT = 117;
    static constexpr size_t MAX_SEQUENCE = 8;
    static constexpr int64_t kPollUs = 50000;
    static constexpr int64_t kPlaybackLimitUs = 10000000;
    // Temporary boot test: ROM effect 2 (strong click, 60%).
    static constexpr bool kStartupTest = true;
    static constexpr uint8_t kStartupEffect = 15;

    static i2c_master_dev_handle_t s_device = nullptr;
    static bool s_ready = false;
    static bool s_busy = false;
    static bool s_fault = false;
    static int64_t s_started_us = 0;
    static int64_t s_next_poll_us = 0;

    // ------------------------------------------------------------------------
    // Shared I2C register access
    // ------------------------------------------------------------------------

    static esp_err_t read_register(uint8_t reg, uint8_t &value){
        if(!s_device) return ESP_ERR_INVALID_STATE;
        return i2c_master_transmit_receive(s_device, &reg, 1, &value, 1, 50);
    }

    static esp_err_t write_register(uint8_t reg, uint8_t value){
        if(!s_device) return ESP_ERR_INVALID_STATE;
        const uint8_t data[] = {reg, value};
        return i2c_master_transmit(s_device, data, sizeof(data), 50);
    }

    static esp_err_t update_register(uint8_t reg, uint8_t mask, uint8_t bits){
        uint8_t value = 0;
        esp_err_t err = read_register(reg, value);
        if(err != ESP_OK) return err;
        return write_register(reg, static_cast<uint8_t>((value & ~mask) | (bits & mask)));
    }

    // Disable EN on communication failure rather than leave an active motor.
    static esp_err_t fail_safe(esp_err_t cause){
        s_ready = false;
        s_busy = false;
        s_fault = true;
        const esp_err_t off = xl9555::set_haptic_enabled(false);
        if(off != ESP_OK) ESP_LOGE(TAG, "Haptic EN disable failed: %s", esp_err_to_name(off));
        ESP_LOGW(TAG, "Haptic operation failed: %s", esp_err_to_name(cause));
        return cause;
    }

    // ------------------------------------------------------------------------
    // Actuator profile and initialization
    // ------------------------------------------------------------------------

    esp_err_t configure(){
        if(!s_device || s_fault || s_busy) return ESP_ERR_INVALID_STATE;
        // Match LilyGoUltra::initDrv and SensorLib's ERM open-loop profile.
        // Preserve rated voltage and overdrive clamp; motor rating is unverified.
        esp_err_t err = write_register(REG_MODE, MODE_INTERNAL);
        if(err == ESP_OK) esp_rom_delay_us(1000);
        if(err == ESP_OK) err = write_register(REG_GO, 0);
        if(err == ESP_OK) err = write_register(REG_RTP, 0);
        if(err == ESP_OK) err = update_register(REG_FEEDBACK, 0x80, 0);
        if(err == ESP_OK) err = update_register(REG_CONTROL3, 0x20, 0x20);
        if(err == ESP_OK) err = write_register(REG_LIBRARY, LIBRARY_ERM);
        if(err == ESP_OK) err = write_register(REG_OVERDRIVE, 0);
        if(err == ESP_OK) err = write_register(REG_SUSTAIN_POS, 0);
        if(err == ESP_OK) err = write_register(REG_SUSTAIN_NEG, 0);
        if(err == ESP_OK) err = write_register(REG_BRAKE, 0);
        if(err == ESP_OK) err = write_register(REG_AUDIO_MAX, 0x64);
        for(size_t i = 0; i < MAX_SEQUENCE && err == ESP_OK; ++i) err = write_register(REG_WAVEFORM + i, 0);
        if(err == ESP_OK) err = write_register(REG_MODE, MODE_STANDBY);
        if(err != ESP_OK) return fail_safe(err);
        return ESP_OK;
    }

    esp_err_t init(){
        if(s_ready) return ESP_OK;
        if(s_device || s_fault){
            const esp_err_t err = shutdown();
            if(err != ESP_OK) return err;
        }
        if(!board::i2c_bus()) return ESP_ERR_INVALID_STATE;
        i2c_device_config_t cfg{};
        cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        cfg.device_address = board::addresses::HAPTIC;
        cfg.scl_speed_hz = 100000;
        esp_err_t err = i2c_master_bus_add_device(board::i2c_bus(), &cfg, &s_device);
        if(err != ESP_OK) return err;
        err = xl9555::set_haptic_enabled(true);
        if(err == ESP_OK) esp_rom_delay_us(2000);
        uint8_t status = 0;
        if(err == ESP_OK) err = read_register(REG_STATUS, status);
        const uint8_t id = status >> 5;
        // Only accept the ROM-library DRV2605/DRV2605L family.
        if(err == ESP_OK && id != 3 && id != 7) err = ESP_ERR_NOT_SUPPORTED;
        if(err == ESP_OK) err = configure();
        if(err != ESP_OK){
            (void)shutdown();
            return err;
        }
        s_ready = true;
        ESP_LOGI(TAG, "Ready: device ID=%u, ERM library=%u, standby", id, LIBRARY_ERM);
        return ESP_OK;
    }

    // ------------------------------------------------------------------------
    // Nonblocking ROM waveform playback
    // ------------------------------------------------------------------------

    esp_err_t play_sequence(const uint8_t *effects, size_t count){
        if(!effects || count == 0 || count > MAX_SEQUENCE) return ESP_ERR_INVALID_ARG;
        for(size_t i = 0; i < count; ++i) if(effects[i] == 0 || effects[i] > MAX_EFFECT) return ESP_ERR_INVALID_ARG;
        if(!s_ready || s_busy) return ESP_ERR_INVALID_STATE;
        esp_err_t err = write_register(REG_MODE, MODE_INTERNAL);
        if(err == ESP_OK) esp_rom_delay_us(1000);
        // Write all slots to remove any stale sequence from earlier playback.
        for(size_t i = 0; i < MAX_SEQUENCE && err == ESP_OK; ++i) err = write_register(REG_WAVEFORM + i, i < count ? effects[i] : 0);
        if(err == ESP_OK) err = write_register(REG_GO, 1);
        if(err != ESP_OK) return fail_safe(err);
        s_busy = true;
        s_started_us = esp_timer_get_time();
        s_next_poll_us = s_started_us + kPollUs;
        return ESP_OK;
    }

    esp_err_t play_effect(uint8_t effect){ return play_sequence(&effect, 1); }

    esp_err_t stop(){
        if(!s_ready) return ESP_ERR_INVALID_STATE;
        if(!s_busy) return ESP_OK;
        esp_err_t err = write_register(REG_GO, 0);
        if(err == ESP_OK) err = write_register(REG_MODE, MODE_STANDBY);
        if(err != ESP_OK) return fail_safe(err);
        s_busy = false;
        return ESP_OK;
    }

    void loop(){
        // Retry hardware disable after an unsuccessful communication recovery.
        if(s_fault){
            const int64_t now = esp_timer_get_time();
            if(now < s_next_poll_us) return;
            s_next_poll_us = now + 1000000;
            if(xl9555::set_haptic_enabled(false) == ESP_OK) s_fault = false;
            return;
        }
        if(!s_ready) return;
        static bool startup_test_attempted = false;
        if(kStartupTest && !startup_test_attempted){
            startup_test_attempted = true;
            if(!s_busy){
                const esp_err_t err = play_effect(kStartupEffect);
                if(err == ESP_OK) ESP_LOGI(TAG, "Startup vibration requested");
                else ESP_LOGW(TAG, "Startup vibration failed: %s", esp_err_to_name(err));
            }
        }
        if(!s_busy) return;
        const int64_t now = esp_timer_get_time();
        if(now < s_next_poll_us) return;
        s_next_poll_us = now + kPollUs;
        if(now - s_started_us >= kPlaybackLimitUs){
            ESP_LOGW(TAG, "Playback timeout; stopping motor");
            (void)stop();
            return;
        }
        uint8_t go = 0;
        esp_err_t err = read_register(REG_GO, go);
        if(err != ESP_OK){ (void)fail_safe(err); return; }
        if(go & 1) return;
        err = write_register(REG_MODE, MODE_STANDBY);
        if(err != ESP_OK){ (void)fail_safe(err); return; }
        s_busy = false;
        ESP_LOGD(TAG, "Playback completed; standby");
    }

    // ------------------------------------------------------------------------
    // State and shutdown
    // ------------------------------------------------------------------------

    bool ready(){ return s_ready; }
    bool busy(){ return s_busy; }

    esp_err_t shutdown(){
        // EN low stops the actuator independently of the DRV2605 I2C state.
        const esp_err_t off = xl9555::set_haptic_enabled(false);
        s_ready = false;
        s_busy = false;
        s_fault = (off != ESP_OK);
        if(s_device){
            const esp_err_t err = i2c_master_bus_rm_device(s_device);
            if(err != ESP_OK) return err;
            s_device = nullptr;
        }
        return off;
    }
}
