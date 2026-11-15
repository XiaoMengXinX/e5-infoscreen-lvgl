#pragma once
#include <stdbool.h>

bool camera_driver_supported(void);
bool camera_driver_ready(void);
void camera_driver_install(void (*progress)(const char *), void (*done)(bool));
void camera_driver_cancel(void);
