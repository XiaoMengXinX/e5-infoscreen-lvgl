#pragma once

#include "lvgl.h"

/* Line icons drawn from 24×24 SVG path data in the text colour. */
lv_obj_t *icon_create(lv_obj_t *parent, const char *name, int size);
void icon_set(lv_obj_t *icon, const char *name);
