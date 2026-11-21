
#include "MIA_M10Q.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "AXP2101.h"
#include "board_pins.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"

namespace MIA_M10Q {

    static const char *TAG = "MIA_M10Q";
    static constexpr uart_port_t GPS_UART = UART_NUM_1;

    static constexpr uint32_t FIX_MAX_AGE_MS = 120000;
    static constexpr uint32_t ACQUISITION_PERIOD_MS = 15 * 60 * 1000;
    static constexpr uint32_t ACQUISITION_TIMEOUT_MS = 3 * 60 * 1000;
    static constexpr uint32_t FIX_HOLD_MS = 15000;
    static constexpr uint32_t MIN_OFF_MS = 30000;

    // This component is polled from the main application task.
    static bool initialized = false;
    static bool powered = false;
    static bool have_cycle_fix = false;

    static uint32_t started_ms = 0;
    static uint32_t first_fix_ms = 0;
    static uint32_t stopped_ms = 0;
    static uint32_t next_cycle_ms = 0;

    static char sentence[128] = {};
    static size_t sentence_length = 0;
    static fix_t latest = {};

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

    static bool angle(const char *text, char hemisphere,
                      bool latitude, double &out){
        double degrees_minutes = 0;

        if(!number(text, degrees_minutes) ||
           degrees_minutes < 0) return false;

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

        if(star == nullptr ||
           star[1] == 0 || star[2] == 0 || star[3] != 0)
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
        if(count < 11 ||
           strlen(fields[0]) != 6 ||
           strcmp(fields[0] + 3, "GGA") != 0)
            return false;

        // 0 means there is no valid GNSS fix.
        if(fields[6][0] < '1' ||
           fields[6][0] > '8' ||
           fields[6][1] != 0)
            return false;

        if(strlen(fields[3]) != 1 ||
           strlen(fields[5]) != 1 ||
           strcmp(fields[10], "M") != 0)
            return false;

        fix_t result = {};

        if(!angle(fields[2], fields[3][0],
                  true, result.latitude) ||
           !angle(fields[4], fields[5][0],
                  false, result.longitude) ||
           !number(fields[9], result.altitude)){
            return false;
        }

        if(result.altitude < -1000 ||
           result.altitude > 20000)
            return false;

        result.valid = true;
        fix = result;
        return true;
    }

    esp_err_t init(){
        if(initialized) return ESP_OK;

        uart_config_t config = {};
        config.baud_rate = board::hardware::GPS_BAUD;
        config.data_bits = UART_DATA_8_BITS;
        config.parity = UART_PARITY_DISABLE;
        config.stop_bits = UART_STOP_BITS_1;
        config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
        config.source_clk = UART_SCLK_DEFAULT;

        esp_err_t err = uart_driver_install(
            GPS_UART, 2048, 0, 0, nullptr, 0);

        if(err != ESP_OK){
            ESP_LOGE(TAG, "GNSS UART driver failed: %s",
                     esp_err_to_name(err));
            return err;
        }

        err = uart_param_config(GPS_UART, &config);

        if(err == ESP_OK){
            err = uart_set_pin(
                GPS_UART,
                board::pins::GPS_TX,
                board::pins::GPS_RX,
                UART_PIN_NO_CHANGE,
                UART_PIN_NO_CHANGE);
        }

        if(err != ESP_OK){
            ESP_LOGE(TAG, "GNSS UART configuration failed: %s",
                     esp_err_to_name(err));
            uart_driver_delete(GPS_UART);
            return err;
        }

        initialized = true;
        next_cycle_ms = millis_now();

        ESP_LOGI(TAG, "MIA_M10Q initialized at %lu baud",
                 static_cast<unsigned long>(
                     board::hardware::GPS_BAUD));

        return ESP_OK;
    }

    static void start_acquisition(uint32_t now){
        if(!initialized || powered)
            return;

        esp_err_t err = AXP2101::enable(AXP2101::rail_t::gps);

        if(err != ESP_OK){
            ESP_LOGW(TAG, "GNSS power-on failed: %s",
                     esp_err_to_name(err));
            next_cycle_ms = now + 60000;
            return;
        }

        uart_flush_input(GPS_UART);

        sentence_length = 0;
        powered = true;
        have_cycle_fix = false;
        first_fix_ms = 0;
        started_ms = now;

        ESP_LOGI(TAG, "GNSS acquisition started");
    }

    static void stop_acquisition(uint32_t now){
        if(!powered)
            return;

        esp_err_t err = AXP2101::disable(AXP2101::rail_t::gps);

        if(err != ESP_OK){
            ESP_LOGW(TAG, "GNSS power-off failed: %s",
                     esp_err_to_name(err));
            return;
        }

        powered = false;
        stopped_ms = now;
        next_cycle_ms = now + ACQUISITION_PERIOD_MS;

        ESP_LOGI(TAG, "GNSS acquisition finished, fix=%d",
                 have_cycle_fix);
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

            if(parse_gga(sentence, parsed)){
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

        if(!powered){
            if(static_cast<int32_t>(now - next_cycle_ms) >= 0)
                start_acquisition(now);
            return;
        }

        uint8_t bytes[128];

        int n = uart_read_bytes(
            GPS_UART, bytes, sizeof(bytes), 0);

        if(n > 0){
            for(int i = 0; i < n; i++)
                consume(bytes[i], now);
        }

        if(now - started_ms >= ACQUISITION_TIMEOUT_MS ||
           (have_cycle_fix &&
            now - first_fix_ms >= FIX_HOLD_MS)){
            stop_acquisition(now);
        }
    }

    bool get_fresh_fix(fix_t &fix){
        if(!latest.valid ||
           millis_now() - latest.updated_ms > FIX_MAX_AGE_MS){
            return false;
        }

        fix = latest;
        return true;
    }

    void request_fix(){
        if(!initialized || powered)
            return;

        fix_t fix;

        if(get_fresh_fix(fix))
            return;

        const uint32_t now = millis_now();

        if(stopped_ms != 0 &&
           now - stopped_ms < MIN_OFF_MS){
            return;
        }

        next_cycle_ms = now;
    }

}
