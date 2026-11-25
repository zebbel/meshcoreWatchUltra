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

#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_VARS_H*/