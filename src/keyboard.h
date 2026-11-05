#pragma once

#include "lvgl.h"

/* Shows the shared on-screen keyboard while the text area has focus. */
void keyboard_bind(lv_obj_t *textarea);
void keyboard_close(void);
