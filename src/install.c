#include "install.h"

#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "app.h"
#include "theme.h"
#include "ui.h"

static struct {
    lv_obj_t *step, *button;
    struct ui_meter meter;
    pid_t pid;
    int fd;
    char line[512];
    size_t len;
    bool open;
    void (*done)(void);
} install = {.pid = -1, .fd = -1};

static lv_timer_t *reader;

static int reap(void)
{
    int status = 0;
    close(install.fd);
    install.fd = -1;
    while (waitpid(install.pid, &status, 0) < 0 && errno == EINTR) {
    }
    install.pid = -1;
    return status;
}

static void finish(void)
{
    int status = reap();
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        void (*done)(void) = install.done;
        install.done = NULL;
        dialog_close();
        done();
        return;
    }
    ui_set_text(install.step, L("安装失败，请检查网络后重试", "The install failed; check the network and retry"));
    ui_meter_set(&install.meter, L("未完成", "Incomplete"), lv_bar_get_value(install.meter.bar), theme.danger);
    ui_button_text(install.button, L("关闭", "Close"));
}

static void stop_script(void)
{
    kill(-install.pid, SIGKILL);
    lv_timer_delete(reader);
    reader = NULL;
    reap();
}

void install_cancel(void)
{
    if (install.pid >= 0)
        stop_script();
    install.done = NULL;
    if (!install.open)
        return;
    install.open = false;
    dialog_close();
}

bool install_running(void)
{
    return install.pid >= 0;
}

static void install_progress(int percent)
{
    char text[8];
    snprintf(text, sizeof text, "%d%%", percent);
    ui_meter_set(&install.meter, text, percent, theme.accent);
}

static void install_line(const char *line)
{
    unsigned long long done, total;
    if (!strcmp(line, "@check")) {
        ui_set_text(install.step, L("正在获取最新版本…", "Finding the latest version…"));
    } else if (!strcmp(line, "@update")) {
        ui_set_text(install.step, L("正在更新软件源…", "Updating the package feeds…"));
        ui_set_text(install.meter.value, L("准备中", "Preparing"));
    } else if (!strcmp(line, "@add")) {
        ui_set_text(install.step, L("正在下载安装…", "Downloading and installing…"));
        install_progress(0);
    } else if (!strncmp(line, "@download ", 10)) {
        char version[32] = "", host[64] = "", step[128];
        sscanf(line + 10, "%31s %63s", version, host);
        snprintf(step, sizeof step, L("正在从 %s 下载 v%s…", "Downloading v%2$s from %1$s…"), *host ? host : "github.com",
                 version);
        ui_set_text(install.step, step);
        install_progress(0);
    } else if (!strcmp(line, "@install")) {
        ui_set_text(install.step, L("正在安装…", "Installing…"));
    } else if (sscanf(line, "%llu/%llu", &done, &total) == 2 && total) {
        install_progress((int)(done * 100 / total));
    } else {
        const char *mark = strrchr(line, '%');
        if (!mark)
            return;
        /* curl's bar ends "  42.3%"; uclient-fetch's has " 42% |". */
        const char *digits = mark;
        while (digits > line && ((digits[-1] >= '0' && digits[-1] <= '9') || digits[-1] == '.'))
            digits--;
        if (digits != mark)
            install_progress((int)strtod(digits, NULL));
    }
}

static void take_line(void)
{
    install.line[install.len] = 0;
    install.len = 0;
    if (*install.line)
        install_line(install.line);
}

static void poll_output(lv_timer_t *t)
{
    /* A step can stay silent for a while; the screen stays up until the script ends. */
    idle_reset();
    char buf[1024];
    for (;;) {
        ssize_t n = read(install.fd, buf, sizeof buf);
        if (n > 0) {
            for (ssize_t i = 0; i < n; i++) {
                if (buf[i] == '\r' || buf[i] == '\n')
                    take_line();
                else if (install.len < sizeof install.line - 1)
                    install.line[install.len++] = buf[i];
            }
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && errno == EAGAIN)
            return;
        if (install.len)
            take_line();
        /* EOF, or EIO once the script has exited and closed the terminal. */
        lv_timer_delete(t);
        reader = NULL;
        finish();
        return;
    }
}

static void on_close(lv_event_t *e)
{
    dialog_close();
}

static void on_closed(void)
{
    install_cancel();
}

void install_run(const char *title, const char *script, void (*done)(void))
{
    dialog_open(title, NULL);
    install = (typeof(install)){.pid = -1, .fd = -1, .done = done, .open = true};
    lv_obj_t *sheet = dialog_sheet();
    install.step = ui_text(sheet, 13, false, theme.text);
    lv_obj_set_width(install.step, LV_PCT(100));
    lv_label_set_long_mode(install.step, LV_LABEL_LONG_MODE_WRAP);
    install.meter = ui_meter(sheet, L("进度", "Progress"));
    install.button = dialog_button(L("取消", "Cancel"), on_close, NULL);
    dialog_on_close(on_closed);
    int fd;
    pid_t pid = forkpty(&fd, NULL, NULL, NULL);
    if (pid < 0) {
        install_cancel();
        toast("%s", L("无法启动安装", "Cannot start the install"));
        return;
    }
    if (pid == 0) {
        execl("/bin/sh", "sh", "-c", script, (char *)NULL);
        _exit(127);
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    install.pid = pid;
    install.fd = fd;
    reader = lv_timer_create(poll_output, 50, NULL);
}
