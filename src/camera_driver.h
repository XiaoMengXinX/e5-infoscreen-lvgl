#pragma once
#include <stdbool.h>
#include <stddef.h>

bool camera_driver_supported(void);
bool camera_driver_ready(void);
bool camera_driver_present(void);
void camera_driver_commit(char *value, size_t size);
bool camera_driver_uninstall(void);
void camera_driver_install(void (*progress)(const char *), void (*done)(bool));
void camera_driver_cancel(void);
