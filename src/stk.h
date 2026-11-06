#pragma once

#include <stdbool.h>

#include "lvgl.h"

void stk_create(lv_obj_t *parent);
void stk_open(void);
void stk_close(void);
/* False when the view is at its top and Back should leave it. */
bool stk_back(void);
