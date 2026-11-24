#include "GPS.h"

namespace hardware::gps {
    void set_utc_callback(UtcCallback callback) { MIA_M10Q::set_utc_callback(callback); }
    esp_err_t init(){ return MIA_M10Q::init(); }
    void loop(){ MIA_M10Q::loop(); }
    bool get_fresh_fix(fix_t &fix){ return MIA_M10Q::get_fresh_fix(fix); }
    void request_fix(){ MIA_M10Q::request_fix(); }
    void get_status(status_t &status){ MIA_M10Q::get_status(status); }
    bool is_enabled(){ return MIA_M10Q::is_enabled(); }
    esp_err_t set_enabled(bool enabled){ return MIA_M10Q::set_enabled(enabled); }
}
