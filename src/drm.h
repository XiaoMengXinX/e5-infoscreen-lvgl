#pragma once

#include <stdbool.h>

#include "lvgl.h"

/* A double-buffered DRM display on dumb buffers: each frame is drawn whole
 * into the hidden buffer and flipped on vblank. */
lv_display_t *drm_display_create(const char *path);

/* Writes the frame on screen as a PNG. */
bool drm_display_save(const char *path);
