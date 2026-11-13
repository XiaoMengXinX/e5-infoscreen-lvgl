#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#include "lvgl.h"

/* A program run on a pseudo-terminal and read on the main thread. Each line it
 * prints, ended by \r or \n, reaches on_line as it is; on_exit gets its wait
 * status once it has ended and been reaped. The screen stays awake meanwhile. */
struct child {
    pid_t pid;
    int fd;
    lv_timer_t *reader;
    void (*on_line)(const char *line);
    void (*on_exit)(int status);
    char line[512];
    size_t len;
};

/* Executes argv[0] with argv; false when the program cannot be started. */
bool child_start(struct child *c, char *const argv[], void (*on_line)(const char *line), void (*on_exit)(int status));
bool child_running(const struct child *c);
/* Kills the program with its process group and reaps it; on_exit does not run. */
void child_kill(struct child *c);
