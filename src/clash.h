#pragma once

#include <stdbool.h>

#include "lvgl.h"

struct key_event;

/* Clash on this device, through its external controller: traffic, connections
 * and the proxy mode. */
void clash_create(lv_obj_t *parent);
void clash_open(void);
void clash_close(void);
/* False when the status is shown and Back should leave the app. */
bool clash_back(void);
/* Left and Right switch the mode while its row has the key focus. */
bool clash_key(const struct key_event *key);
