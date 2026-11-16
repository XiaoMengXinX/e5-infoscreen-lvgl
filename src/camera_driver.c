#include "camera_driver.h"
#include "camera_driver_script.h"
#include "child.h"
#include "torch.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/syscall.h>
#include <unistd.h>

#define DRIVER_DIR "/usr/libexec/e5-infoscreen/camera"
static const char *files[] = {"sprd_sensor.ko", "sprd_flash_drv.ko", "sprd_camera.ko", "build.commit", "kernel.release"};
static struct child installer;
static char stage[256];
static void (*completed)(bool);
static void (*progress_line)(const char *);
static bool error_reported;

static void report(const char *line)
{
    if (!strncmp(line, "@error ", 7)) error_reported = true;
    progress_line(line);
}

static bool profile(char *value, size_t size)
{
    FILE *f = fopen("/etc/e5/device-profile", "r");
    if (!f) return false;
    bool read = fgets(value, size, f) != NULL;
    fclose(f);
    if (!read) return false;
    value[strcspn(value, "\r\n")] = 0;
    return !strcmp(value, "e5") || !strcmp(value, "lym10k") || !strcmp(value, "t512n");
}

bool camera_driver_supported(void)
{
    char value[64];
    return profile(value, sizeof value);
}

bool camera_driver_ready(void)
{
    struct utsname running;
    char version[256];
    FILE *f = fopen(DRIVER_DIR "/kernel.release", "r");
    if (!f) return false;
    bool read = fgets(version, sizeof version, f) != NULL;
    fclose(f);
    if (!read || uname(&running)) return false;
    version[strcspn(version, "\r\n")] = 0;
    if (strcmp(version, running.release)) return false;
    for (size_t i = 0; i < 3; i++) {
        char path[256];
        snprintf(path, sizeof path, DRIVER_DIR "/%s", files[i]);
        if (access(path, R_OK)) return false;
    }
    return true;
}

bool camera_driver_present(void)
{
    for (size_t i = 0; i < sizeof files / sizeof *files; i++) {
        char path[256];
        snprintf(path, sizeof path, DRIVER_DIR "/%s", files[i]);
        if (!access(path, F_OK)) return true;
    }
    return false;
}

void camera_driver_commit(char *value, size_t size)
{
    value[0] = 0;
    FILE *f = fopen(DRIVER_DIR "/build.commit", "r");
    if (!f) return;
    if (fgets(value, size, f)) value[strcspn(value, "\r\n")] = 0;
    fclose(f);
}

static bool unload(void)
{
    if (!torch_set(false)) return false;
    const char *modules[] = {"sprd_camera", "sprd_flash_drv", "sprd_sensor"};
    for (size_t i = 0; i < sizeof modules / sizeof *modules; i++)
        if (syscall(SYS_delete_module, modules[i], O_NONBLOCK) < 0 && errno != ENOENT)
            return false;
    return true;
}

bool camera_driver_uninstall(void)
{
    if (!unload()) return false;
    for (size_t i = 0; i < sizeof files / sizeof *files; i++) {
        char path[256];
        snprintf(path, sizeof path, DRIVER_DIR "/%s", files[i]);
        if (unlink(path) < 0 && errno != ENOENT) return false;
    }
    return true;
}

static void clean_stage(void)
{
    if (!*stage) return;
    const char *temporary[] = {"manifest.json", "driver.tar.gz"};
    char path[512];
    for (size_t i = 0; i < sizeof files / sizeof *files; i++) {
        snprintf(path, sizeof path, "%s/%s", stage, files[i]);
        unlink(path);
    }
    for (size_t i = 0; i < sizeof temporary / sizeof *temporary; i++) {
        snprintf(path, sizeof path, "%s/%s", stage, temporary[i]);
        unlink(path);
    }
    rmdir(stage);
    *stage = 0;
}

static void installed(int status)
{
    bool success = status == 0;
    if (success && !unload()) {
        char error[256];
        snprintf(error, sizeof error, "@error Cannot unload camera drivers: %s", strerror(errno));
        report(error);
        success = false;
    }
    if (success) {
        unlink(DRIVER_DIR "/kernel.release");
        for (size_t i = 0; i < sizeof files / sizeof *files; i++) {
            char source[512], target[256];
            snprintf(source, sizeof source, "%s/%s", stage, files[i]);
            snprintf(target, sizeof target, DRIVER_DIR "/%s", files[i]);
            if (rename(source, target)) {
                report("@error Cannot install camera module files");
                success = false;
                break;
            }
        }
    }
    clean_stage();
    if (!success && !error_reported) report("@error Camera driver installer exited unexpectedly");
    completed(success);
}

void camera_driver_install(void (*progress)(const char *), void (*done)(bool))
{
    char value[64];
    completed = done;
    progress_line = progress;
    error_reported = false;
    if (!profile(value, sizeof value)) { progress("@error Unsupported camera device"); done(false); return; }
    mkdir(DRIVER_DIR, 0755);
    snprintf(stage, sizeof stage, DRIVER_DIR "/.install-XXXXXX");
    if (!mkdtemp(stage)) {
        *stage = 0;
        progress("@error Cannot create camera driver installation directory");
        done(false);
        return;
    }
    if (!child_start(&installer, (char *const[]){"/usr/bin/python3", "-u", "-c", (char *)CAMERA_DRIVER_SCRIPT, value, stage, NULL}, report, installed)) {
        progress("@error Cannot start camera driver installer");
        clean_stage();
        done(false);
    }
}

void camera_driver_cancel(void)
{
    child_kill(&installer);
    clean_stage();
}
