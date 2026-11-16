#include "torch.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#define TORCH_BRIGHTNESS "/sys/class/leds/aw3641:white:torch/brightness"
static int torch_fd = -1;

bool torch_supported(void)
{
    char profile[64];
    FILE *f = fopen("/etc/e5/device-profile", "r");
    if (!f) return false;
    bool read = fgets(profile, sizeof profile, f) != NULL;
    fclose(f);
    if (!read) return false;
    profile[strcspn(profile, "\r\n")] = 0;
    return !strcmp(profile, "e5");
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
        return result == 0;
    }
    if (!torch_supported()) { errno = ENODEV; return false; }
    if (access("/sys/module/sprd_flash_drv", F_OK)) {
        int module = open("/usr/libexec/e5-infoscreen/camera/sprd_flash_drv.ko", O_RDONLY | O_CLOEXEC);
        if (module < 0) return false;
        int result = syscall(SYS_finit_module, module, "", 0);
        int error = errno;
        close(module);
        if (result < 0 && error != EEXIST) { errno = error; return false; }
    }
    int fd = open("/dev/sprd_flash", O_RDWR | O_CLOEXEC);
    if (fd < 0) return false;
    FILE *f = fopen(TORCH_BRIGHTNESS, "w");
    bool written = false;
    if (f) {
        written = fprintf(f, "1\n") > 0;
        if (fclose(f)) written = false;
    }
    if (written) torch_fd = fd;
    else {
        int error = errno;
        close(fd);
        errno = error;
    }
    return written;
}
