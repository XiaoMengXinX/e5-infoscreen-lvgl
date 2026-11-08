#pragma once

#include <stdbool.h>

#include "lvgl.h"

/* A triple-buffered DRM display: each frame is drawn in ordinary memory, copied into
 * the spare dumb buffer, and flipped on vblank, a frame behind the one on screen. */
lv_display_t *drm_display_create(const char *path);

/* The last frame handed to the display, in ordinary memory; drm_display_frames()
 * tells when it changes. */
const uint8_t *drm_display_front(uint32_t *pitch);
uint32_t drm_display_frames(void);
void drm_display_size(uint32_t *width, uint32_t *height);

/* Writes the last frame handed to the display as a PNG. */
bool drm_display_save(const char *path);
