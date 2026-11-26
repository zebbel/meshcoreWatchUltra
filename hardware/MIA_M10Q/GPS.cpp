#include "GPS.h"

// Generic GPS interface backed by the MIA-M10Q receiver.
namespace hardware::gps {
    void set_utc_callback(UtcCallback callback){ hardware::mia_m10q::set_utc_callback(callback); }
    esp_err_t init(){ return hardware::mia_m10q::init(); }
    void loop(){ hardware::mia_m10q::loop(); }
    bool get_fresh_fix(fix_t &fix){ return hardware::mia_m10q::get_fresh_fix(fix); }
    void request_fix(){ hardware::mia_m10q::request_fix(); }
    void get_status(status_t &status){ hardware::mia_m10q::get_status(status); }
    bool is_enabled(){ return hardware::mia_m10q::is_enabled(); }
    esp_err_t set_enabled(bool enabled){ return hardware::mia_m10q::set_enabled(enabled); }
}
