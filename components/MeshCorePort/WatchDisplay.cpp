
#include "WatchDisplay.h"
#include "CO5300.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include "esp_timer.h"
#include "MIA_M10Q.h"

namespace watch {

    static bool ready = false;
    static bool gps_redraw = true;

    // 5x7 digit font; bit 4 is the leftmost pixel.
    static const uint8_t digits[10][7] = { {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}, {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}, {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E}, {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, {0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E}, {0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E}, {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}, {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x0E} };

    esp_err_t pairing_display_init(){
        esp_err_t err = CO5300::init();
        if(err != ESP_OK) return err;
        ready = true;
        return CO5300::fill_rect(0, 0, 410, 502, 0x0000);
    }

    void pairing_display_clear(){
        gps_redraw = true;
        if(ready) CO5300::fill_rect(0, 0, 410, 502, 0x0000);
    }

    void pairing_display_show(uint32_t pin){
        if(!ready || pin < 100000 || pin > 999999) return;
        pairing_display_clear();
        // Six digits across a 410 pixel display, enlarged 5x7 pixels.
        for(int index = 5; index >= 0; index--){
            int n = static_cast<int>(pin % 10);
            pin /= 10;
            const int x = 23 + index * 64;
            const int y = 205;
            for(int row = 0; row < 7; row++){
                for(int col = 0; col < 5; col++){
                    if(digits[n][row] & (0x10 >> col)){
                        CO5300::fill_rect(x + col * 10, y + row * 10, 9, 9, 0xFFFF);
                    }
                }
            }
        }
    }
    
    static const uint8_t gps_letters[26][7] = {
        {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, // A
        {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}, // B
        {0x0F, 0x10, 0x10, 0x10, 0x10, 0x10, 0x0F}, // C
        {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}, // D
        {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}, // E
        {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}, // F
        {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0E}, // G
        {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, // H
        {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}, // I
        {0x07, 0x02, 0x02, 0x02, 0x12, 0x12, 0x0C}, // J
        {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}, // K
        {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}, // L
        {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}, // M
        {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}, // N
        {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, // O
        {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}, // P
        {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}, // Q
        {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}, // R
        {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}, // S
        {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}, // T
        {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, // U
        {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}, // V
        {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}, // W
        {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}, // X
        {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}, // Y
        {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}  // Z
    };

    static const uint8_t gps_blank[7] = {};
    static const uint8_t gps_colon[7] = {0, 4, 4, 0, 4, 4, 0};
    static const uint8_t gps_dot[7] = {0, 0, 0, 0, 0, 6, 6};
    static const uint8_t gps_minus[7] = {0, 0, 0, 31, 0, 0, 0};

    static constexpr uint16_t GPS_WHITE = 0xFFFF;
    static constexpr uint16_t GPS_CYAN = 0x07FF;
    static constexpr uint16_t GPS_GREEN = 0x07E0;
    static constexpr uint16_t GPS_RED = 0xF800;
    static constexpr uint16_t GPS_YELLOW = 0xFFE0;

    static constexpr int GPS_ROWS = 11;
    static constexpr int GPS_SCALE = 3;

    static char gps_cached_text[GPS_ROWS][48] = {};
    static uint16_t gps_cached_color[GPS_ROWS] = {};
    static constexpr int GPS_ROW_WIDTH = 410;
    static constexpr int GPS_ROW_HEIGHT = 32;
    static uint16_t gps_row_pixels[GPS_ROW_WIDTH * GPS_ROW_HEIGHT] = {};

    static const uint8_t *gps_glyph(char ch){
        if(ch >= '0' && ch <= '9') return digits[ch - '0'];
        if(ch >= 'A' && ch <= 'Z') return gps_letters[ch - 'A'];
        if(ch == ':') return gps_colon;
        if(ch == '.') return gps_dot;
        if(ch == '-') return gps_minus;
        return gps_blank;
    }
    
    static void gps_draw_row(int index, const char *text, uint16_t color){
        if(index < 0 || index >= GPS_ROWS || text == nullptr) return;
        if(!gps_redraw && strcmp(gps_cached_text[index], text) == 0 && gps_cached_color[index] == color) return;

        memset(gps_row_pixels, 0, sizeof(gps_row_pixels));

        for(size_t i = 0; text[i] != 0; i++){
            int left = 72 + static_cast<int>(i) * 6 * GPS_SCALE;
            if(left + 5 * GPS_SCALE > GPS_ROW_WIDTH) break;

            const uint8_t *glyph = gps_glyph(text[i]);

            for(int gy = 0; gy < 7; gy++){
                for(int gx = 0; gx < 5; gx++){
                    if((glyph[gy] & (0x10 >> gx)) == 0) continue;

                    for(int dy = 0; dy < GPS_SCALE; dy++){
                        int py = 2 + gy * GPS_SCALE + dy;

                        for(int dx = 0; dx < GPS_SCALE; dx++){
                            int px = left + gx * GPS_SCALE + dx;
                            gps_row_pixels[py * GPS_ROW_WIDTH + px] = color;
                        }
                    }
                }
            }
        }

        int y = 52 + index * 40;
        esp_err_t err = CO5300::draw_rgb565(0, y, GPS_ROW_WIDTH, GPS_ROW_HEIGHT, gps_row_pixels);
        if(err != ESP_OK) return;

        snprintf(gps_cached_text[index], sizeof(gps_cached_text[index]), "%s", text);
        gps_cached_color[index] = color;
    }
}
