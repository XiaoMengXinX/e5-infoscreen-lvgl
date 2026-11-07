#pragma once

#include <stdbool.h>

#include "lvgl.h"

struct key_event;

/* The battery view, over the home page's own content. */
void battery_create(lv_obj_t *parent, lv_obj_t *home);
void battery_open(void);
/* False when the view was not open. */
bool battery_close(void);
bool battery_is_open(void);
/* Enter hands Left and Right to the history's range choices. */
bool battery_key(const struct key_event *key);
void battery_touch(void);
