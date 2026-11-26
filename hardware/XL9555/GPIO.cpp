#include "GPIO.h"

#include "XL9555.h"

// Generic GPIO initialization backed by the XL9555 expander.
namespace hardware::gpio {
    esp_err_t init(){ return hardware::xl9555::init(); }
}
