#pragma once
#include <stdbool.h>
#include <stddef.h>

/* The board named in /etc/e5/device-profile, empty when it is missing. */
void camera_driver_device(char *value, size_t size);
bool camera_driver_supported(void);
/* Loads an installed module unless the kernel already has it: 1 when this call
 * loaded it, 0 when it was already loaded, -1 with errno on failure. */
int camera_driver_load(const char *module);
bool camera_driver_ready(void);
bool camera_driver_present(void);
void camera_driver_commit(char *value, size_t size);
bool camera_driver_uninstall(void);
void camera_driver_install(void (*progress)(const char *), void (*done)(bool));
void camera_driver_cancel(void);
