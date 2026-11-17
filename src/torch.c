#include "torch.h"
#include "camera_driver.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define TORCH_BRIGHTNESS "/sys/class/leds/aw3641:white:torch/brightness"
static int torch_fd = -1;
static void (*on_change)(bool enabled);

void torch_on_change(void (*changed)(bool enabled))
{
    on_change = changed;
}

bool torch_supported(void)
{
    char device[64];
    camera_driver_device(device, sizeof device);
    return !strcmp(device, "e5");
}

bool torch_available(void)
{
    return torch_supported() && access(TORCH_BRIGHTNESS, W_OK) == 0;
}

bool torch_enabled(void)
{
    return torch_fd >= 0;
}

bool torch_set(bool enabled)
{
    if (enabled == torch_enabled()) return true;
    if (!enabled) {
        /* Releasing the flash device switches the light off, including on process exit. */
        int result = close(torch_fd);
        torch_fd = -1;
        if (on_change) on_change(false);
        return result == 0;
    }
    if (!torch_supported()) { errno = ENODEV; return false; }
    if (camera_driver_load("sprd_flash_drv") < 0) return false;
    int fd = open("/dev/sprd_flash", O_RDWR | O_CLOEXEC);
    if (fd < 0) return false;
    FILE *f = fopen(TORCH_BRIGHTNESS, "w");
    bool written = false;
    if (f) {
        written = fprintf(f, "1\n") > 0;
        if (fclose(f)) written = false;
    }
    if (written) {
        torch_fd = fd;
        if (on_change) on_change(true);
    } else {
        int error = errno;
        close(fd);
        errno = error;
    }
    return written;
}
