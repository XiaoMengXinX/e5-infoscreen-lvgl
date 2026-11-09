#pragma once

#include <stdbool.h>

#include "lvgl.h"

lv_display_t *drm_display_create(const char *path);
int drm_display_fd(void);
bool drm_display_can_draw(void);
/* Milliseconds to the outstanding flip's deadline, or -1 with no pending flip. */
int drm_display_wait(void);
void drm_display_dispatch(void);
/* Panel refresh period, in milliseconds. */
double drm_display_period(void);

/* The last frame handed to the display, in ordinary memory; drm_display_frames()
 * tells when it changes. */
const uint8_t *drm_display_front(uint32_t *pitch);
uint32_t drm_display_frames(void);
void drm_display_size(uint32_t *width, uint32_t *height);

/* Writes the last frame handed to the display as a PNG. */
bool drm_display_save(const char *path);
