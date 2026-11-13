#include "install.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#include "app.h"
#include "child.h"
#include "theme.h"
#include "ui.h"

static struct {
    lv_obj_t *step, *button;
    struct ui_meter meter;
    struct child child;
    bool open;
    void (*done)(void);
} install;

static void exited(int status)
{
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

void install_cancel(void)
{
    child_kill(&install.child);
    install.done = NULL;
    if (!install.open)
        return;
    install.open = false;
    dialog_close();
}

bool install_running(void)
{
    return child_running(&install.child);
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
    install = (typeof(install)){.done = done, .open = true};
    lv_obj_t *sheet = dialog_sheet();
    install.step = ui_text(sheet, 13, false, theme.text);
    lv_obj_set_width(install.step, LV_PCT(100));
    lv_label_set_long_mode(install.step, LV_LABEL_LONG_MODE_WRAP);
    install.meter = ui_meter(sheet, L("进度", "Progress"));
    install.button = dialog_button(L("取消", "Cancel"), on_close, NULL);
    dialog_on_close(on_closed);
    if (!child_start(&install.child, (char *const[]){"/bin/sh", "-c", (char *)script, NULL}, install_line, exited)) {
        install_cancel();
        toast("%s", L("无法启动安装", "Cannot start the install"));
    }
}
