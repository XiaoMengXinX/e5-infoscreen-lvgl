#pragma once

#include "lvgl.h"

enum wallpaper { WALLPAPER_DESKTOP, WALLPAPER_LOCK };

/* The wallpaper as opaque pixels the size of the screen, washed with the
 * background colour where text lies over it; NULL without one. Decoded on first
 * use after wallpaper_reset. */
const lv_draw_buf_t *wallpaper_get(enum wallpaper which);
void wallpaper_background(lv_obj_t *obj);
/* Drops both, for a theme or a wallpaper that has changed; nothing may still show them. */
void wallpaper_reset(void);
