
#include "MIA_M10Q.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "AXP2101.h"
#include "board_pins.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "Storage.h"

namespace MIA_M10Q {
    static UtcCallback s_utc_callback = nullptr;
    void set_utc_callback(UtcCallback callback) { s_utc_callback = callback; }

    static const char *TAG = "MIA_M10Q";
    static constexpr uart_port_t GPS_UART = UART_NUM_1;

    static constexpr uint32_t FIX_MAX_AGE_MS = 120000;
    static constexpr uint32_t ACQUISITION_PERIOD_MS = 15U * 60U * 1000U;
    static constexpr uint32_t MAX_RETRY_PERIOD_MS = 120U * 60U * 1000U;
    static constexpr uint32_t ACQUISITION_TIMEOUT_MS = 5 * 60 * 1000;
    static constexpr uint32_t FIX_HOLD_MS = 15000;
    static constexpr uint32_t MIN_OFF_MS = 30000;

    // This component is polled from the main application task.
    static bool initialized = false;
    static bool powered = false;
    static bool gps_enabled = true;
    static bool have_cycle_fix = false;
    // RAM-only retry state: restart with the normal interval after reboot.
    static uint8_t consecutive_misses = 0;

    static uint32_t started_ms = 0;
    static uint32_t first_fix_ms = 0;
    static uint32_t stopped_ms = 0;
    static uint32_t next_cycle_ms = 0;

    static char sentence[128] = {};
    static size_t sentence_length = 0;
    static fix_t latest = {};
    static status_t diagnostics = {};
    static bool test_mode = false;

    static uint32_t millis_now(){
        return static_cast<uint32_t>(esp_timer_get_time() / 1000);
    }

    static int hex_digit(char ch){
        if(ch >= '0' && ch <= '9') return ch - '0';
        if(ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        if(ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        return -1;
    }

    static bool number(const char *text, double &value){
        if(text == nullptr || text[0] == 0) return false;

        char *end = nullptr;
        value = strtod(text, &end);

        return end != text && *end == 0 && isfinite(value);
    }

    static bool angle(const char *text, char hemisphere, bool latitude, double &out){
        double degrees_minutes = 0;

        if(!number(text, degrees_minutes) || degrees_minutes < 0) return false;

        int degrees = static_cast<int>(degrees_minutes / 100);
        double minutes = degrees_minutes - degrees * 100;

        const int maximum = latitude ? 90 : 180;

        if(minutes < 0 || minutes >= 60 || degrees > maximum)
            return false;
        if(degrees == maximum && minutes > 0)
            return false;

        if(latitude && hemisphere != 'N' && hemisphere != 'S')
            return false;
        if(!latitude && hemisphere != 'E' && hemisphere != 'W')
            return false;

        out = degrees + minutes / 60.0;

        if(hemisphere == 'S' || hemisphere == 'W')
            out = -out;

        return true;
    }

    static bool parse_gga(const char *input, fix_t &fix){
        if(input == nullptr || input[0] != '$')
            return false;

        const char *star = strchr(input, '*');

        if(star == nullptr || star[1] == 0 || star[2] == 0 || star[3] != 0)
            return false;

        int hi = hex_digit(star[1]);
        int lo = hex_digit(star[2]);

        if(hi < 0 || lo < 0)
            return false;

        uint8_t checksum = 0;

        for(const char *p = input + 1; p < star; p++){
            checksum ^= static_cast<uint8_t>(*p);
        }

        if(checksum != static_cast<uint8_t>((hi << 4) | lo))
            return false;

        if(star - input >= 128)
            return false;

        char copy[128] = {};
        memcpy(copy, input, static_cast<size_t>(star - input));

        char *fields[16] = {};
        size_t count = 0;
        fields[count++] = copy;

        for(char *p = copy; *p != 0; p++){
            if(*p == ','){
                *p = 0;
                if(count < 16)
                    fields[count++] = p + 1;
            }
        }

        // GGA supplies position, fix quality, and altitude.
        // Accept GP and GN talker IDs.
        if(count < 11 || strlen(fields[0]) != 6 || strcmp(fields[0] + 3, "GGA") != 0)
            return false;

        // 0 means there is no valid GNSS fix.
        if(fields[6][0] < '1' || fields[6][0] > '8' || fields[6][1] != 0)
            return false;

        if(strlen(fields[3]) != 1 || strlen(fields[5]) != 1 || strcmp(fields[10], "M") != 0)
            return false;

        fix_t result = {};

        if(!angle(fields[2], fields[3][0], true, result.latitude) || !angle(fields[4], fields[5][0], false, result.longitude) || !number(fields[9], result.altitude)){
            return false;
        }

        if(result.altitude < -1000 || result.altitude > 20000)
            return false;

        result.valid = true;
        fix = result;
        return true;
    }

    static void parse_gga_diagnostics(const char *nmea, uint32_t now){
        if(nmea == nullptr || nmea[0] != '$') return;

        const char *star = strchr(nmea, '*');
        if(star == nullptr || star[1] == 0 || star[2] == 0 || star[3] != 0) return;

        int hi = hex_digit(star[1]);
        int lo = hex_digit(star[2]);
        if(hi < 0 || lo < 0) return;

        uint8_t checksum = 0;
        for(const char *p = nmea + 1; p < star; p++) checksum ^= static_cast<uint8_t>(*p);
        if(checksum != static_cast<uint8_t>((hi << 4) | lo)) return;

        size_t length = static_cast<size_t>(star - nmea);
        if(length >= 128) return;

        char copy[128] = {};
        memcpy(copy, nmea, length);

        char *fields[16] = {};
        size_t count = 0;
        fields[count++] = copy;

        for(char *p = copy; *p != 0; p++){
            if(*p == ','){
                *p = 0;
                if(count < 16) fields[count++] = p + 1;
            }
        }

        if(count < 9 || strlen(fields[0]) != 6 || strcmp(fields[0] + 3, "GGA") != 0) return;
        if(strlen(fields[6]) != 1 || fields[6][0] < '0' || fields[6][0] > '8') return;

        char *end = nullptr;
        long satellites = strtol(fields[7], &end, 10);
        if(end == fields[7] || *end != 0 || satellites < 0 || satellites > 99) satellites = 0;

        double hdop = 0;
        if(!number(fields[8], hdop) || hdop < 0 || hdop > 99.9) hdop = 0;

        diagnostics.has_gga = true;
        diagnostics.gga_count++;
        diagnostics.last_gga_ms = now;
        diagnostics.fix_quality = static_cast<uint8_t>(fields[6][0] - '0');
        diagnostics.satellites = static_cast<uint8_t>(satellites);
        diagnostics.hdop = static_cast<float>(hdop);

        memset(diagnostics.utc, 0, sizeof(diagnostics.utc));

        if(strlen(fields[1]) >= 6){
            bool valid_time = true;
            for(int i = 0; i < 6; i++) valid_time &= fields[1][i] >= '0' && fields[1][i] <= '9';
            if(valid_time) memcpy(diagnostics.utc, fields[1], 6);
        }
    }

    // NMEA RMC contains both UTC date and time; GGA provides only time of day.
    // A valid status and checksum are required before we trust it as clock input.
    static bool parse_rmc_utc(const char *input, uint32_t &utc_seconds) {
        if(input == nullptr || input[0] != '$') return false;
        const char *star = strchr(input, '*');
        if(star == nullptr || star[1] == 0 || star[2] == 0 || star[3] != 0) return false;
        const int hi = hex_digit(star[1]);
        const int lo = hex_digit(star[2]);
        if(hi < 0 || lo < 0) return false;
        uint8_t checksum = 0;
        for(const char *p = input + 1; p < star; ++p) checksum ^= static_cast<uint8_t>(*p);
        if(checksum != static_cast<uint8_t>((hi << 4) | lo)) return false;

        const size_t length = static_cast<size_t>(star - input);
        if(length >= 128) return false;
        char copy[128] = {};
        memcpy(copy, input, length);
        char *fields[16] = {};
        size_t count = 0;
        fields[count++] = copy;
        for(char *p = copy; *p != 0; ++p) {
            if(*p == ',') {
                *p = 0;
                if(count < 16) fields[count++] = p + 1;
            }
        }
        if(count < 10 || strlen(fields[0]) != 6 ||
           strcmp(fields[0] + 3, "RMC") != 0 || strcmp(fields[2], "A") != 0) return false;

        const char *t = fields[1]; // hhmmss[.sss]
        const char *d = fields[9]; // ddmmyy
        if(strlen(t) < 6 || strlen(d) != 6) return false;
        for(int i = 0; i < 6; ++i) {
            if(t[i] < '0' || t[i] > '9' || d[i] < '0' || d[i] > '9') return false;
        }
        if(t[6] != 0 && t[6] != '.') return false;
        if(t[6] == '.' && t[7] == 0) return false;
        if(t[6] == '.') {
            for(int i = 7; t[i] != 0; ++i) if(t[i] < '0' || t[i] > '9') return false;
        }
        const unsigned hh = static_cast<unsigned>((t[0]-'0')*10 + t[1]-'0');
        const unsigned mm = static_cast<unsigned>((t[2]-'0')*10 + t[3]-'0');
        const unsigned ss = static_cast<unsigned>((t[4]-'0')*10 + t[5]-'0');
        const unsigned day = static_cast<unsigned>((d[0]-'0')*10 + d[1]-'0');
        const unsigned mon = static_cast<unsigned>((d[2]-'0')*10 + d[3]-'0');
        const unsigned year = 2000U + static_cast<unsigned>((d[4]-'0')*10 + d[5]-'0');
        if(year < 2020U || year > 2099U || mon < 1U || mon > 12U ||
           day < 1U || hh > 23U || mm > 59U || ss > 59U) return false;
        static constexpr unsigned DAYS[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
        const unsigned leap = (year % 4U == 0U && (year % 100U != 0U || year % 400U == 0U)) ? 1U : 0U;
        if(day > DAYS[mon - 1U] + (mon == 2U ? leap : 0U)) return false;

        // Calendar to Unix seconds, ignoring the host's local timezone.
        int y = static_cast<int>(year) - (mon <= 2U);
        const int era = (y >= 0 ? y : y - 399) / 400;
        const unsigned yoe = static_cast<unsigned>(y - era * 400);
        const unsigned mp = mon > 2U ? mon - 3U : mon + 9U;
        const unsigned doy = (153U * mp + 2U) / 5U + day - 1U;
        const unsigned ydoe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
        const int64_t days = static_cast<int64_t>(era) * 146097LL + ydoe - 719468LL;
        const int64_t epoch = days * 86400LL + hh * 3600LL + mm * 60LL + ss;
        if(epoch < 1577836800LL || epoch > 4102444799LL) return false;
        utc_seconds = static_cast<uint32_t>(epoch);
        return true;
    }

    esp_err_t init(){
        if(initialized) return ESP_OK;

        uart_config_t config = {};
        config.baud_rate = hardware::board::specs::GPS_BAUD;
        config.data_bits = UART_DATA_8_BITS;
        config.parity = UART_PARITY_DISABLE;
        config.stop_bits = UART_STOP_BITS_1;
        config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
        config.source_clk = UART_SCLK_DEFAULT;

        esp_err_t err = uart_driver_install(GPS_UART, 2048, 0, 0, nullptr, 0);

        if(err != ESP_OK){
            ESP_LOGE(TAG, "GNSS UART driver failed: %s", esp_err_to_name(err));
            return err;
        }

        err = uart_param_config(GPS_UART, &config);

        if(err == ESP_OK){
            err = uart_set_pin(GPS_UART, hardware::board::pins::GPS_TX, hardware::board::pins::GPS_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        }

        if(err != ESP_OK){
            ESP_LOGE(TAG, "GNSS UART configuration failed: %s", esp_err_to_name(err));
            uart_driver_delete(GPS_UART);
            return err;
        }

        initialized = true;
        next_cycle_ms = millis_now();


        hardware::storage::Handle gps_store;
        if(hardware::storage::open("gps", hardware::storage::Access::read_only, gps_store) == ESP_OK){
            uint8_t value = 1;
            size_t length = sizeof(value);
            if(hardware::storage::read_blob(gps_store, "enabled", &value, &length) == ESP_OK && length == 1 && value <= 1) gps_enabled = value != 0;
            hardware::storage::close(gps_store);
        }

        ESP_LOGI(TAG, "GPS setting loaded: enabled=%d", gps_enabled);


        ESP_LOGI(TAG, "MIA_M10Q initialized at %lu baud", static_cast<unsigned long>(hardware::board::specs::GPS_BAUD));

        return ESP_OK;
    }

    static void start_acquisition(uint32_t now){
        if(!initialized || powered)
            return;

        esp_err_t err = AXP2101::enable(AXP2101::rail_t::gps);

        if(err != ESP_OK){
            ESP_LOGW(TAG, "GNSS power-on failed: %s", esp_err_to_name(err));
            next_cycle_ms = now + 60000;
            return;
        }

        uart_flush_input(GPS_UART);

        sentence_length = 0;
        powered = true;
        have_cycle_fix = false;
        first_fix_ms = 0;
        started_ms = now;

        ESP_LOGD(TAG, "GNSS acquisition started");
    }

    static void stop_acquisition(uint32_t now){
        if(!powered)
            return;

        esp_err_t err = AXP2101::disable(AXP2101::rail_t::gps);

        if(err != ESP_OK){
            ESP_LOGW(TAG, "GNSS power-off failed: %s", esp_err_to_name(err));
            return;
        }

        powered = false;
        stopped_ms = now;

        // Failed acquisition: 30, 60, then 120 minutes between attempts.
        // Any valid fix returns the wait to 15 minutes. Disabling GPS
        // does not count as an acquisition failure.
        if (have_cycle_fix || !gps_enabled) {
            consecutive_misses = 0;
        } else if (consecutive_misses < 3) {
            ++consecutive_misses;
        }
        uint32_t retry_ms = ACQUISITION_PERIOD_MS;
        for (uint8_t i = 0; i < consecutive_misses; ++i) {
            retry_ms *= 2U;
        }
        if (retry_ms > MAX_RETRY_PERIOD_MS) retry_ms = MAX_RETRY_PERIOD_MS;
        next_cycle_ms = now + retry_ms;

        ESP_LOGD(TAG, "GNSS acquisition finished, fix=%d, next attempt in %lu min",
                 have_cycle_fix, static_cast<unsigned long>(retry_ms / 60000U));
    }

    static void consume(uint8_t ch, uint32_t now){
        if(ch == '\r')
            return;

        if(ch == '$'){
            sentence_length = 0;
            sentence[sentence_length++] = '$';
            return;
        }

        if(sentence_length == 0)
            return;

        if(ch == '\n'){
            sentence[sentence_length] = 0;

            fix_t parsed = {};

            const bool valid_fix = parse_gga(sentence, parsed);

            // Diagnostic only; does not print private coordinates.
            if(strlen(sentence) >= 6 && strncmp(sentence + 3, "GGA", 3) == 0){
                parse_gga_diagnostics(sentence, now);
            }

            uint32_t utc_seconds = 0;
            if(parse_rmc_utc(sentence, utc_seconds)) {
                // Throttled in System; never writes RTC on every RMC sentence.
                if (s_utc_callback) (void)s_utc_callback(utc_seconds);
            }

            if(valid_fix){
                parsed.updated_ms = now;
                latest = parsed;

                if(!have_cycle_fix){
                    have_cycle_fix = true;
                    first_fix_ms = now;
                    ESP_LOGI(TAG, "GNSS fix acquired");
                }
            }

            sentence_length = 0;
        }else if(sentence_length < sizeof(sentence) - 1){
            sentence[sentence_length++] = static_cast<char>(ch);
        }else{
            sentence_length = 0;
        }
    }

    void loop(){
        if(!initialized)
            return;

        const uint32_t now = millis_now();

        if(!gps_enabled){
            if(powered) stop_acquisition(now);
            return;
        }

        if(!powered){
            if(static_cast<int32_t>(now - next_cycle_ms) >= 0)
                start_acquisition(now);
            return;
        }

        uint8_t bytes[128];

        int n = uart_read_bytes(GPS_UART, bytes, sizeof(bytes), 0);

        if(n > 0){
            for(int i = 0; i < n; i++)
                consume(bytes[i], now);
        }

        if(!test_mode && (now - started_ms >= ACQUISITION_TIMEOUT_MS || (have_cycle_fix && now - first_fix_ms >= FIX_HOLD_MS))){
            stop_acquisition(now);
        }
    }

    bool get_fresh_fix(fix_t &fix){
        if(!gps_enabled || !latest.valid || millis_now() - latest.updated_ms > FIX_MAX_AGE_MS){
            return false;
        }

        fix = latest;
        return true;
    }

    void request_fix(){
        if(!initialized || !gps_enabled || powered) return;

        fix_t fix;

        if(get_fresh_fix(fix))
            return;

        const uint32_t now = millis_now();

        if(stopped_ms != 0 && now - stopped_ms < MIN_OFF_MS){
            return;
        }

        next_cycle_ms = now;
    }

    void get_status(status_t &status){
        status = diagnostics;
        status.initialized = initialized;
        status.powered = powered;
    }

    bool is_enabled(){
        return gps_enabled;
    }

    esp_err_t set_enabled(bool enabled){
        if(!initialized) return ESP_ERR_INVALID_STATE;
        if(gps_enabled == enabled) return ESP_OK;

        hardware::storage::Handle h;
        esp_err_t err = hardware::storage::open("gps", hardware::storage::Access::read_write, h);
        if(err != ESP_OK) return err;
        const uint8_t value = enabled ? 1 : 0;
        err = hardware::storage::write_blob(h, "enabled", &value, sizeof(value));
        if(err == ESP_OK) err = hardware::storage::commit(h);
        hardware::storage::close(h);
        if(err != ESP_OK) return err;

        gps_enabled = enabled;
        latest = {};
        sentence_length = 0;

        if(enabled){
            consecutive_misses = 0;
            next_cycle_ms = millis_now();
            ESP_LOGI(TAG, "GPS enabled");
        }else{
            if(powered) stop_acquisition(millis_now());
            ESP_LOGI(TAG, "GPS disabled");
            if(powered) return ESP_FAIL;
        }

        return ESP_OK;
    }

}
