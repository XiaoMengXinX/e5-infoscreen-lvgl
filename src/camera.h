#pragma once
#include "app.h"
bool camera_supported(void);
void camera_create(lv_obj_t *parent);
void camera_open(void);
void camera_close(void);
void camera_scan(void (*on_code)(const char *activation));
bool camera_scanning(void);
bool camera_key(const struct key_event *key);
