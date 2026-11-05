#pragma once

#include <stdbool.h>

#include "cJSON.h"
#include "lvgl.h"

struct theme {
    float scale;
    bool light, motion;
    bool status_bar, nav_bar;
    bool desktop_wallpaper, lock_wallpaper;
    lv_opa_t surface_opa;
    lv_color_t bg, chrome, surface, button, active, danger, nav;
    lv_color_t text, dim, line, accent, good, fair, poor, hero_start, hero_end;
};

extern struct theme theme;

/* Returns true when the configuration changes how the shell is built. */
bool theme_apply(const cJSON *screen);

int S(int px);
const lv_font_t *F(int px);
const lv_font_t *FB(int px);
lv_color_t grade_color(const char *word);
