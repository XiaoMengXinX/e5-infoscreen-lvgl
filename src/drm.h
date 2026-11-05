#pragma once

#include <stdbool.h>

#include "lvgl.h"

/* A double-buffered DRM display on dumb buffers: each frame is drawn whole
 * into the hidden buffer and flipped on vblank. */
lv_display_t *drm_display_create(const char *path);

/* The frame on screen; the count of shown frames tells when it changes. */
const uint8_t *drm_display_front(uint32_t *pitch);
uint32_t drm_display_frames(void);
void drm_display_size(uint32_t *width, uint32_t *height);

/* Writes the frame on screen as a PNG. */
bool drm_display_save(const char *path);
