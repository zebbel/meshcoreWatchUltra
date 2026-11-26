#include "GPIO.h"

#include "XL9555.h"

namespace hardware::gpio {
    esp_err_t init(){ return hardware::xl9555::init(); }
}
