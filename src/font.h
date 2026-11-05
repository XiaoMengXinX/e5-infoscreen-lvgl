#pragma once

#include <stdbool.h>

#include "lvgl.h"

void font_init(void);
const lv_font_t *font_get(int px, bool bold);
