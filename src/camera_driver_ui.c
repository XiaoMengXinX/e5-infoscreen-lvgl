#include "camera_driver_ui.h"
#include "camera_driver.h"
#include "app.h"
#include "theme.h"
#include "ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
    lv_obj_t *step, *button;
    struct ui_meter meter;
    void (*done)(bool);
} driver_ui;

void camera_driver_ui_cancel(void)
{
    dialog_on_close(NULL);
    dialog_close();
    camera_driver_cancel();
    driver_ui.done = NULL;
}

static void closed(void)
{
    void (*done)(bool) = driver_ui.done;
    camera_driver_ui_cancel();
    done(false);
}

static void progress(const char *line)
{
    if (!strcmp(line, "@check"))
        ui_set_text(driver_ui.step, L("正在检查相机驱动…", "Checking camera driver…"));
    else if (!strcmp(line, "@download")) {
        ui_set_text(driver_ui.step, L("正在从GitHub下载相机驱动…", "Downloading camera driver from GitHub…"));
        ui_meter_set(&driver_ui.meter, "0%", 0, theme.accent);
    } else if (!strcmp(line, "@install"))
        ui_set_text(driver_ui.step, L("正在校验并安装相机驱动…", "Verifying and installing camera driver…"));
    else if (!strncmp(line, "@error ", 7))
        ui_set_fmt(driver_ui.step, "%s\n%s", L("相机驱动安装失败", "Camera driver installation failed"), line + 7);
    else {
        const char *mark = strrchr(line, '%');
        if (!mark) return;
        const char *digits = mark;
        while (digits > line && ((digits[-1] >= '0' && digits[-1] <= '9') || digits[-1] == '.')) digits--;
        if (digits == mark) return;
        int percent = (int)strtod(digits, NULL);
        char text[16];
        snprintf(text, sizeof text, "%d%%", percent);
        ui_meter_set(&driver_ui.meter, text, percent, theme.accent);
    }
}

static void installed(bool success)
{
    if (success) {
        void (*done)(bool) = driver_ui.done;
        driver_ui.done = NULL;
        dialog_on_close(NULL);
        dialog_close();
        done(true);
    } else {
        ui_meter_set(&driver_ui.meter, L("未完成", "Incomplete"),
                     lv_bar_get_value(driver_ui.meter.bar), theme.danger);
        ui_button_text(driver_ui.button, L("关闭", "Close"));
    }
}

static void on_cancel(lv_event_t *event)
{
    dialog_close();
}

static void on_install(lv_event_t *event)
{
    dialog_on_close(NULL);
    dialog_open(L("安装相机驱动", "Install camera driver"), NULL);
    driver_ui.step = ui_note(dialog_sheet(), L("正在检查相机驱动…", "Checking camera driver…"));
    lv_obj_set_width(driver_ui.step, LV_PCT(100));
    lv_label_set_long_mode(driver_ui.step, LV_LABEL_LONG_MODE_WRAP);
    driver_ui.meter = ui_meter(dialog_sheet(), L("下载进度", "Download progress"));
    ui_meter_set(&driver_ui.meter, L("准备中", "Preparing"), 0, theme.accent);
    driver_ui.button = dialog_button_quiet(L("取消", "Cancel"), on_cancel, NULL);
    dialog_on_close(closed);
    camera_driver_install(progress, installed);
}

void camera_driver_ui_open(void (*done)(bool))
{
    bool ready = camera_driver_ready();
    dialog_open(L("安装相机驱动", "Install camera driver"),
                ready ? L("从GitHub下载并重新安装最新的匹配驱动？", "Download and reinstall the latest matching driver from GitHub?")
                      : L("使用相机需要从GitHub下载并安装匹配的驱动，是否安装？",
                          "The camera needs a matching driver from GitHub. Download and install it?"));
    driver_ui.done = done;
    dialog_button(ready ? L("重新安装", "Reinstall") : L("下载安装", "Install"), on_install, NULL);
    dialog_button_quiet(L("取消", "Cancel"), on_cancel, NULL);
    dialog_on_close(closed);
}
