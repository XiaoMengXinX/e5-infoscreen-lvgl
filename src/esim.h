#pragma once

#include <stdbool.h>

#include "lvgl.h"

struct key_event;

void esim_create(lv_obj_t *parent);
void esim_open(void);
void esim_close(void);
/* False when the view is at its top and Back should leave it. */
bool esim_back(void);
/* The app's own handling of the arrows; true when it took the key. */
bool esim_key(const struct key_event *key);
