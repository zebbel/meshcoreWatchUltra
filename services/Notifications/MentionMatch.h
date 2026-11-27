#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace services::notifications::detail {
    inline uint8_t fold_ascii(uint8_t c){ return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; }
    inline bool name_char(uint8_t c){
        c = fold_ascii(c);
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c >= 128;
    }
    // MeshCore channel text has a "Sender: body" prefix. Match body only.
    // ASCII case-insensitive, exact current node name; UTF-8 bytes match exactly.
    inline bool mentions(const uint8_t *text, size_t length, const char *name){
        if(!text || !name || !*name) return false;
        const size_t n = strlen(name);
        size_t body = 0;
        for(size_t i = 0; i + 1 < length && i <= 31; ++i){
            if(text[i] == ':' && text[i + 1] == ' '){ body = i + 2; break; }
        }
        for(size_t i = body; i < length; ++i){
            if(text[i] != '@' || n > length - i - 1) continue;
            if(i > body && name_char(text[i - 1])) continue;
            size_t j = 0;
            while(j < n && fold_ascii(text[i + 1 + j]) == fold_ascii(static_cast<uint8_t>(name[j]))) ++j;
            if(j != n) continue;
            const size_t end = i + 1 + n;
            if(end == length || !name_char(text[end])) return true;
        }
        return false;
    }
}
