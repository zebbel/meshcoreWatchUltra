
#include "WatchDisplay.h"
#include "CO5300.h"
#include "esp_log.h"

namespace watch {

    static bool ready = false;
    // 5x7 digit font; bit 4 is the leftmost pixel.
    static const uint8_t digits[10][7] = {
        {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E},
        {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
        {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},
        {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E},
        {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},
        {0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E},
        {0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E},
        {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
        {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},
        {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x0E}
    };

    esp_err_t pairing_display_init(){
        esp_err_t err = CO5300::init();
        if(err != ESP_OK) return err;
        ready = true;
        return CO5300::fill_rect(0, 0, 410, 502, 0x0000);
    }

    void pairing_display_clear(){
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

}
