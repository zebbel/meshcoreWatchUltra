#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_err.h"
#include "SDStorage.h"

namespace meshcore::sdstore {
    constexpr const char *ROOT = "/sdcard/meshcore";
    constexpr uint16_t VERSION = 1;
    struct Header {
        uint32_t magic;
        uint16_t version;
        uint16_t length;
        uint32_t checksum;
    };

    inline uint32_t crc32(const uint8_t *data, size_t length){
        uint32_t crc = 0xFFFFFFFFU;
        for(size_t i = 0; i < length; ++i){
            crc ^= data[i];
            for(int j = 0; j < 8; ++j) crc = (crc >> 1U) ^ (0xEDB88320U & (0U - (crc & 1U)));
        }
        return ~crc;
    }

    template<typename T>
    esp_err_t read(const char *path, uint32_t magic, T &value){
        struct Record { Header header; T payload; } item = {};
        size_t length = 0;
        esp_err_t err = hardware::sd::read_file(path, &item, sizeof(item), length);
        if(err != ESP_OK) return err;
        if(length != sizeof(item) || item.header.magic != magic || item.header.version != VERSION || item.header.length != sizeof(T)) return ESP_ERR_INVALID_CRC;
        if(crc32(reinterpret_cast<const uint8_t *>(&item.payload), sizeof(T)) != item.header.checksum) return ESP_ERR_INVALID_CRC;
        value = item.payload;
        return ESP_OK;
    }

    template<typename T>
    esp_err_t write(const char *path, uint32_t magic, const T &value, bool deferred = false){
        static_assert(sizeof(T) <= 65535U, "SD record too large");
        struct Record { Header header; T payload; } item = {};
        item.header.magic = magic;
        item.header.version = VERSION;
        item.header.length = sizeof(T);
        item.header.checksum = crc32(reinterpret_cast<const uint8_t *>(&value), sizeof(T));
        item.payload = value;
        if(deferred) return hardware::sd::defer_write(path, &item, sizeof(item));
        return hardware::sd::write_file(path, &item, sizeof(item));
    }

    inline void hex_key(const uint8_t *bytes, size_t length, char *out){
        static constexpr char HEX[] = "0123456789abcdef";
        for(size_t i = 0; i < length; ++i){
            out[i * 2] = HEX[bytes[i] >> 4];
            out[i * 2 + 1] = HEX[bytes[i] & 0x0F];
        }
        out[length * 2] = 0;
    }
}
