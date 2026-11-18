#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char directory[] = "/tmp/e5-torch-test-XXXXXX";
static const char *device = "e5";
static int changes;
static bool last_state, fail_writes;

static void local_path(const char *path, char *out, size_t size)
{
    assert(!strcmp(path, "/sys/class/leds/e5:white:torch/brightness"));
    snprintf(out, size, "%s/brightness", directory);
}

static FILE *test_fopen(const char *path, const char *mode)
{
    char local[256];
    local_path(path, local, sizeof local);
    if (!strcmp(mode, "w")) {
        if (fail_writes) { errno = EACCES; return NULL; }
        if (access(local, F_OK)) return NULL;
    }
    return fopen(local, mode);
}

static int test_access(const char *path, int mode)
{
    char local[256];
    local_path(path, local, sizeof local);
    return access(local, mode);
}

#define fopen test_fopen
#define access test_access
#include "../src/torch.c"
#undef fopen
#undef access

void camera_driver_device(char *value, size_t size)
{
    snprintf(value, size, "%s", device);
}

static void changed(bool enabled)
{
    changes++;
    last_state = enabled;
}

static void external_state(bool enabled)
{
    char local[256];
    local_path(TORCH_BRIGHTNESS, local, sizeof local);
    FILE *f = fopen(local, "w");
    assert(f);
    fprintf(f, "%d\n", enabled);
    fclose(f);
}

int main(void)
{
    assert(mkdtemp(directory));
    torch_on_change(changed);
    assert(torch_set(false));
    assert(!torch_available());
    assert(!torch_set(true) && errno == ENOENT);

    external_state(false);
    assert(torch_available());
    assert(torch_set(true) && torch_enabled());
    assert(last_state && changes == 1);
    external_state(false);
    torch_sync();
    assert(!torch_enabled() && !last_state && changes == 2);
    torch_sync();
    assert(changes == 2);
    external_state(true);
    torch_sync();
    assert(last_state && changes == 3);
    assert(torch_set(false) && !torch_enabled());
    assert(!last_state && changes == 4);

    fail_writes = true;
    assert(!torch_set(true) && errno == EACCES);
    assert(!torch_set(false) && errno == EACCES);
    assert(!torch_enabled() && changes == 4);
    fail_writes = false;

    device = "lym10k";
    assert(!torch_available() && !torch_enabled());
    assert(!torch_set(true) && errno == ENODEV);
    assert(torch_set(false));
    char local[256];
    local_path(TORCH_BRIGHTNESS, local, sizeof local);
    assert(!unlink(local) && !rmdir(directory));
    puts("torch: system LED, external state, write errors and device gating passed");
}
