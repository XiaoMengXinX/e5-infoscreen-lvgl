#pragma once

#include <stdbool.h>

#include "lvgl.h"

/* Builds the speed test view; the test itself outlives a rebuild of the view. */
void speedtest_create(lv_obj_t *parent);
void speedtest_reset(void);
void speedtest_stop(void);
/* Back during a test: stops it and clears what it measured, so the backend can be
 * switched; false when no test runs. */
bool speedtest_cancel(void);
bool speedtest_running(void);
