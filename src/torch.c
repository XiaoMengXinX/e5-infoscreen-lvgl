#include "torch.h"
#include "camera_driver.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define TORCH_BRIGHTNESS "/sys/class/leds/e5:white:torch/brightness"
static void (*on_change)(bool enabled);
static bool reported;

void torch_on_change(void (*changed)(bool enabled))
{
    on_change = changed;
    reported = torch_enabled();
}

bool torch_supported(void)
{
    char device[64];
    camera_driver_device(device, sizeof device);
    return !strcmp(device, "e5");
}

bool torch_available(void)
{
    return torch_supported() && !access(TORCH_BRIGHTNESS, R_OK | W_OK);
}

bool torch_enabled(void)
{
    if (!torch_supported()) return false;
    FILE *f = fopen(TORCH_BRIGHTNESS, "r");
    if (!f) return false;
    int value = fgetc(f);
    fclose(f);
    return value == '1';
}

void torch_sync(void)
{
    bool enabled = torch_enabled();
    if (reported == enabled) return;
    reported = enabled;
    if (on_change) on_change(enabled);
}

bool torch_set(bool enabled)
{
    if (!torch_supported()) {
        if (!enabled) return true;
        errno = ENODEV;
        return false;
    }
    FILE *f = fopen(TORCH_BRIGHTNESS, "w");
    /* Camera cleanup also runs on systems without a torch device. */
    if (!f) return !enabled && errno == ENOENT;
    bool written = fprintf(f, "%d\n", enabled) > 0;
    if (fclose(f)) written = false;
    if (written) torch_sync();
    return written;
}
