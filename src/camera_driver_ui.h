#pragma once
#include <stdbool.h>

void camera_driver_ui_open(void (*done)(bool));
void camera_driver_ui_cancel(void);
