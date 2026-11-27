#ifndef EEZ_LVGL_UI_VARS_H
#define EEZ_LVGL_UI_VARS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// enum declarations

// Flow global variables

enum FlowGlobalVariables {
    FLOW_GLOBAL_VARIABLE_NONE
};

// Native global variables

extern bool get_var_is_charging();
extern void set_var_is_charging(bool value);
extern int32_t get_var_battery_percent();
extern void set_var_battery_percent(int32_t value);
extern const char *get_var_clock_time();
extern void set_var_clock_time(const char *value);
extern int32_t get_var_brightness();
extern void set_var_brightness(int32_t value);
extern bool get_var_ble_connected();
extern void set_var_ble_connected(bool value);
extern const char *get_var_ble_pairing_pin();
extern void set_var_ble_pairing_pin(const char *value);
extern bool get_var_gps_enabled();
extern void set_var_gps_enabled(bool value);
extern bool get_var_gps_fix();
extern void set_var_gps_fix(bool value);
extern int32_t get_var_gps_satellites();
extern void set_var_gps_satellites(int32_t value);
extern bool get_var_gps_failed();
extern void set_var_gps_failed(bool value);
extern bool get_var_gps_acquiring();
extern void set_var_gps_acquiring(bool value);

#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_VARS_H*/