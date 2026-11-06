#pragma once

#include <stdbool.h>

#include "lvgl.h"

struct key_event;

/* Sets the modem's dual-SIM channel mapping, for the boot service; 0 when set. */
int stk_map_channels(void);
void stk_create(lv_obj_t *parent);
void stk_open(void);
void stk_close(void);
/* The toolkit's own handling of the arrows; true when it took the key. */
bool stk_key(const struct key_event *key);
/* False when the view is at its top and Back should leave it. */
bool stk_back(void);
