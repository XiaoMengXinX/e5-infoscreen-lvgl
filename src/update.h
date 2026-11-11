#pragma once

#include <stdbool.h>
#include <stddef.h>

/* The screen's own online update: check the latest release's build timestamp,
 * download and install its binary, or restore the one before it. */
void update_init(void);
/* interactive: report "up to date" and failures; an automatic check stays quiet. */
void update_check(bool interactive);
void update_rollback(void);
bool update_backup_exists(void);
bool update_auto_enabled(void);
bool update_set_auto(bool on);
/* The build time and abbreviated commit embedded by build.py. */
void update_version_text(char *time, size_t time_size, char *commit, size_t commit_size);
