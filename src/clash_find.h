#pragma once

#include <stdbool.h>

struct clash_core {
    int pid;
    char name[32];
    /* Empty when the controller listens on every address or on loopback. */
    char host[64];
    /* 0 when the core runs without a controller. */
    int port;
    char secret[128];
};

/* The running Clash (mihomo) and sing-box cores, up to max, with the controller
 * their command line and configuration give. */
int clash_find(struct clash_core *cores, int max);
/* A process's resident memory in bytes and the seconds since it started. */
bool clash_process(int pid, double *memory, double *uptime);
