#pragma once

#include <stdbool.h>

/* Runs an install script in a dialog. The script prints "@name" lines naming
 * its step (check, update, add, download <version> <host>, install) and
 * "done/total" lines or a percentage as it goes. done runs once the script
 * succeeds; a failure leaves the dialog open with a note. */
void install_run(const char *title, const char *script, void (*done)(void));

void install_cancel(void);

bool install_running(void);
