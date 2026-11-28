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
    SCREEN_ID_WATCH_FACE = 2,
    SCREEN_ID_HOME = 3,
    SCREEN_ID_CHARGING = 4,
    SCREEN_ID_PAIRING = 5,
    _SCREEN_ID_LAST = 5
};

typedef struct _objects_t {
    lv_obj_t *main;
    lv_obj_t *watch_face;
    lv_obj_t *home;
    lv_obj_t *charging;
    lv_obj_t *pairing;
    lv_obj_t *battery_label;
    lv_obj_t *time_label;
    lv_obj_t *battery_bar;
    lv_obj_t *charge_lightning_icon;
    lv_obj_t *bluetooth_icon;
    lv_obj_t *gps_icon;
    lv_obj_t *obj0;
    lv_obj_t *charge_percent_label;
    lv_obj_t *charge_percentage_arc;
    lv_obj_t *pairing_label;
} objects_t;

extern objects_t objects;

void create_screen_main();
void tick_screen_main();

void create_screen_watch_face();
void tick_screen_watch_face();

void create_screen_home();
void tick_screen_home();

void create_screen_charging();
void tick_screen_charging();

void create_screen_pairing();
void tick_screen_pairing();

void tick_screen_by_id(enum ScreensEnum screenId);
void tick_screen(int screen_index);

void create_screens();

#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_SCREENS_H*/