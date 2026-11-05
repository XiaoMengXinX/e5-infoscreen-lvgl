#pragma once

#include <stdbool.h>

#include "lvgl.h"

/* Builds the speed test view; the test itself outlives a rebuild of the view. */
void speedtest_create(lv_obj_t *parent);
void speedtest_reset(void);
void speedtest_stop(void);
bool speedtest_running(void);
