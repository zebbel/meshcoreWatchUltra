#ifndef EEZ_LVGL_UI_SCREENS_H
#define EEZ_LVGL_UI_SCREENS_H

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

// Screens

enum ScreensEnum {
    _SCREEN_ID_FIRST = 1,
    SCREEN_ID_MAIN = 1,
    SCREEN_ID_CHARGING = 2,
    _SCREEN_ID_LAST = 2
};

typedef struct _objects_t {
    lv_obj_t *main;
    lv_obj_t *charging;
    lv_obj_t *battery_label;
    lv_obj_t *time_label;
    lv_obj_t *battery_bar;
    lv_obj_t *charge_lightning_icon_1;
    lv_obj_t *bluetooth_icon;
    lv_obj_t *charge_percent_label;
    lv_obj_t *charge_prcentage_arc;
} objects_t;

extern objects_t objects;

void create_screen_main();
void tick_screen_main();

void create_screen_charging();
void tick_screen_charging();

void tick_screen_by_id(enum ScreensEnum screenId);
void tick_screen(int screen_index);

void create_screens();

#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_SCREENS_H*/