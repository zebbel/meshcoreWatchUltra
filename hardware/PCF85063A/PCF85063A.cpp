#include "PCF85063A.h"

#include <time.h>

#include "board.h"
#include "board_pins.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

namespace hardware::rtc {
namespace {
    constexpr uint8_t CONTROL_1 = 0x00;
    constexpr uint8_t TIME_START = 0x04;
    constexpr uint8_t CTRL_STOP = 0x20;
    constexpr uint8_t CTRL_12H = 0x02;
    constexpr uint8_t SEC_OS = 0x80;
    constexpr uint32_t MIN_VALID_UTC = 1577836800U; // 2020-01-01 00:00:00 UTC
    constexpr uint32_t MAX_VALID_UTC = 4102444799U; // 2099-12-31 23:59:59 UTC
    constexpr int I2C_TIMEOUT_MS = 100;
    static i2c_master_dev_handle_t device = nullptr;

    esp_err_t read_regs(uint8_t reg, uint8_t *data, size_t length) {
        if(device == nullptr) return ESP_ERR_INVALID_STATE;
        return i2c_master_transmit_receive(device, &reg, 1, data, length, I2C_TIMEOUT_MS);
    }

    esp_err_t write_regs(const uint8_t *data, size_t length) {
        if(device == nullptr) return ESP_ERR_INVALID_STATE;
        return i2c_master_transmit(device, data, length, I2C_TIMEOUT_MS);
    }

    esp_err_t write_control(uint8_t value) {
        const uint8_t data[2] = {CONTROL_1, value};
        return write_regs(data, sizeof(data));
    }

    bool decode_bcd(uint8_t raw, uint8_t mask, unsigned &out) {
        const unsigned bcd = raw & mask;
        if((bcd & 0x0fU) > 9U || ((bcd >> 4U) & 0x0fU) > 9U) return false;
        out = (bcd >> 4U) * 10U + (bcd & 0x0fU);
        return true;
    }

    uint8_t to_bcd(unsigned value) {
        return static_cast<uint8_t>((value / 10U) * 16U + value % 10U);
    }

    bool is_leap(unsigned year) {
        return year % 4U == 0U && (year % 100U != 0U || year % 400U == 0U);
    }

    bool valid_date(unsigned year, unsigned month, unsigned day) {
        if(year < 2020U || year > 2099U || month < 1U || month > 12U) return false;
        constexpr unsigned DAYS[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
        return day >= 1U && day <= DAYS[month - 1U] + (month == 2U && is_leap(year) ? 1U : 0U);
    }

    // Civil date to days since Unix epoch, independent of local TZ and timegm().
    int64_t days_from_civil(int year, unsigned month, unsigned day) {
        year -= (month <= 2U);
        const int era = (year >= 0 ? year : year - 399) / 400;
        const unsigned yoe = static_cast<unsigned>(year - era * 400);
        const unsigned mp = month > 2U ? month - 3U : month + 9U;
        const unsigned doy = (153U * mp + 2U) / 5U + day - 1U;
        const unsigned ydoe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
        return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(ydoe) - 719468;
    }
}

esp_err_t init() {
    if(device != nullptr) return ESP_OK;
    if(hardware::board::i2c_bus() == nullptr) return ESP_ERR_INVALID_STATE;

    i2c_device_config_t cfg = {};
    cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    cfg.device_address = hardware::board::addresses::RTC;
    cfg.scl_speed_hz = 100000;

    esp_err_t err = i2c_master_bus_add_device(hardware::board::i2c_bus(), &cfg, &device);
    if(err != ESP_OK) return err;

    uint8_t ctrl = 0;
    err = read_regs(CONTROL_1, &ctrl, sizeof(ctrl));
    if(err != ESP_OK) {
        i2c_master_bus_rm_device(device);
        device = nullptr;
        return err;
    }
    ESP_LOGI("PCF85063A", "RTC accessible at 0x%02X", hardware::board::addresses::RTC);
    return ESP_OK;
}

esp_err_t read_utc(uint32_t &unix_seconds) {
    unix_seconds = 0;
    uint8_t ctrl = 0;
    esp_err_t err = read_regs(CONTROL_1, &ctrl, sizeof(ctrl));
    if(err != ESP_OK) return err;
    if((ctrl & CTRL_STOP) != 0U) return ESP_ERR_INVALID_STATE;

    uint8_t registers[7] = {};
    err = read_regs(TIME_START, registers, sizeof(registers));
    if(err != ESP_OK) return err;
    if((registers[0] & SEC_OS) != 0U) return ESP_ERR_INVALID_STATE;

    unsigned second=0, minute=0, hour=0, day=0, weekday=0, month=0, yy=0;
    const uint8_t hour_mask = (ctrl & CTRL_12H) ? 0x1f : 0x3f;
    if(!decode_bcd(registers[0], 0x7f, second) ||
       !decode_bcd(registers[1], 0x7f, minute) ||
       !decode_bcd(registers[2], hour_mask, hour) ||
       !decode_bcd(registers[3], 0x3f, day) ||
       !decode_bcd(registers[4], 0x07, weekday) ||
       !decode_bcd(registers[5], 0x1f, month) ||
       !decode_bcd(registers[6], 0xff, yy)) return ESP_ERR_INVALID_RESPONSE;

    if((ctrl & CTRL_12H) != 0U) {
        if(hour < 1 || hour > 12) return ESP_ERR_INVALID_RESPONSE;
        hour = (hour % 12U) + ((registers[2] & 0x20U) ? 12U : 0U);
    }

    const unsigned year = 2000U + yy;
    if(!valid_date(year, month, day) || hour > 23U || minute > 59U ||
       second > 59U || weekday > 6U) return ESP_ERR_INVALID_RESPONSE;

    const int64_t seconds = days_from_civil(static_cast<int>(year), month, day) * 86400LL +
        static_cast<int64_t>(hour) * 3600LL + static_cast<int64_t>(minute) * 60LL + second;
    if(seconds < MIN_VALID_UTC || seconds > MAX_VALID_UTC) return ESP_ERR_INVALID_RESPONSE;

    unix_seconds = static_cast<uint32_t>(seconds);
    return ESP_OK;
}

esp_err_t write_utc(uint32_t unix_seconds) {
    if(unix_seconds < MIN_VALID_UTC || unix_seconds > MAX_VALID_UTC)
        return ESP_ERR_INVALID_ARG;
    if(device == nullptr) return ESP_ERR_INVALID_STATE;

    const time_t raw = static_cast<time_t>(unix_seconds);
    struct tm utc = {};
    if(gmtime_r(&raw, &utc) == nullptr) return ESP_ERR_INVALID_ARG;

    uint8_t ctrl = 0;
    esp_err_t err = read_regs(CONTROL_1, &ctrl, sizeof(ctrl));
    if(err != ESP_OK) return err;

    // Freeze the counters while setting all seven registers as one I2C write.
    // Explicitly select 24-hour mode; leave other control bits unchanged.
    const uint8_t stopped = static_cast<uint8_t>((ctrl & ~CTRL_12H) | CTRL_STOP);
    err = write_control(stopped);
    if(err != ESP_OK) return err;

    const uint8_t values[8] = {
        TIME_START,
        to_bcd(static_cast<unsigned>(utc.tm_sec)),
        to_bcd(static_cast<unsigned>(utc.tm_min)),
        to_bcd(static_cast<unsigned>(utc.tm_hour)),
        to_bcd(static_cast<unsigned>(utc.tm_mday)),
        to_bcd(static_cast<unsigned>(utc.tm_wday)),
        to_bcd(static_cast<unsigned>(utc.tm_mon + 1)),
        to_bcd(static_cast<unsigned>((utc.tm_year + 1900) % 100))
    };
    err = write_regs(values, sizeof(values));

    // Always try to restart the oscillator, even if the calendar write failed.
    const esp_err_t release_err = write_control(static_cast<uint8_t>(ctrl & ~(CTRL_STOP | CTRL_12H)));
    if(err != ESP_OK) return err;
    if(release_err != ESP_OK) return release_err;

    uint32_t verified = 0;
    err = read_utc(verified);
    if(err != ESP_OK) return err;
    const int64_t delta = static_cast<int64_t>(verified) - unix_seconds;
    return (delta >= -1 && delta <= 1) ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}
}
