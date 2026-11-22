#include "DisplayTest.h"
#include "lvgl.h"

namespace ui::watch_display {
namespace {
    lv_obj_t *make_color_tile(lv_obj_t *parent, int x, int y, uint32_t rgb) {
        lv_obj_t *tile = lv_obj_create(parent);
        lv_obj_set_size(tile, 42, 42);
        lv_obj_set_pos(tile, x, y);
        lv_obj_set_style_radius(tile, 10, 0);
        lv_obj_set_style_bg_color(tile, lv_color_hex(rgb), 0);
        lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(tile, 0, 0);
        lv_obj_set_style_pad_all(tile, 0, 0);
        lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
        return tile;
    }

    void create_test_screen() {
        lv_obj_t *screen = lv_screen_active();
        lv_obj_set_style_bg_color(screen, lv_color_hex(0x070B14), 0);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
        lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

        // Limit content to a central safe area of the watch's rounded face.
        lv_obj_t *panel = lv_obj_create(screen);
        lv_obj_set_size(panel, 320, 320);
        lv_obj_center(panel);
        lv_obj_set_style_bg_color(panel, lv_color_hex(0x101C2A), 0);
        lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(panel, lv_color_hex(0x34506A), 0);
        lv_obj_set_style_border_width(panel, 2, 0);
        lv_obj_set_style_radius(panel, 36, 0);
        lv_obj_set_style_pad_all(panel, 0, 0);
        lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *title = lv_label_create(panel);
        lv_label_set_text(title, "MESHCORE WATCH");
        lv_obj_set_style_text_color(title, lv_color_hex(0xE5F3FF), 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 45);

        lv_obj_t *subtitle = lv_label_create(panel);
        lv_label_set_text(subtitle, "Display foundation");
        lv_obj_set_style_text_color(subtitle, lv_color_hex(0x67DCEC), 0);
        lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, 95);

        make_color_tile(panel, 52, 146, 0xF44336);  // RGB red
        make_color_tile(panel, 110, 146, 0x4CAF50); // RGB green
        make_color_tile(panel, 168, 146, 0x2196F3); // RGB blue
        make_color_tile(panel, 226, 146, 0xFFFFFF); // white

        lv_obj_t *details = lv_label_create(panel);
        lv_label_set_text(details, "CO5300  /  QSPI  /  RGB565");
        lv_obj_set_style_text_color(details, lv_color_hex(0xAABACB), 0);
        lv_obj_align(details, LV_ALIGN_BOTTOM_MID, 0, -70);

        lv_obj_t *footer = lv_label_create(panel);
        lv_label_set_text(footer, "LVGL 9  |  410 x 502");
        lv_obj_set_style_text_color(footer, lv_color_hex(0x75869C), 0);
        lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, -39);
    }
} // namespace

void render_test_screen() { create_test_screen(); }
} // namespace ui::watch_display
