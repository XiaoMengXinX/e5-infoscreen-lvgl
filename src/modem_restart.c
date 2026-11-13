#include "modem_restart.h"

#include <stdio.h>
#include <string.h>
#include <sys/wait.h>

#include "app.h"
#include "child.h"
#include "modem_restart_script.h"
#include "stk.h"
#include "theme.h"
#include "ui.h"
#include "util.h"

static struct {
    lv_obj_t *step, *button;
    struct ui_meter meter;
    struct child child;
    char error[512];
    void (*done)(void);
} task;

static void on_close(lv_event_t *e)
{
    dialog_close();
}

static void succeeded(void)
{
    dialog_close();
    stk_resume_after_modem_restart();
    void (*done)(void) = task.done;
    task.done = NULL;
    toast("%s", L("基带已重启", "Baseband restarted"));
    done();
}

static void failed(const char *message)
{
    stk_resume_after_modem_restart();
    ui_set_text(task.step, message);
    ui_meter_set(&task.meter, L("未完成", "Incomplete"), lv_bar_get_value(task.meter.bar), theme.danger);
    lv_obj_remove_state(task.button, LV_STATE_DISABLED);
    ui_button_text(task.button, L("关闭", "Close"));
    dialog_set_closable(true);
}

static void progress(const char *line)
{
    if (!strcmp(line, "@preparing")) {
        ui_set_text(task.step, L("正在检查设备状态…", "Checking device state…"));
        ui_meter_set(&task.meter, "5%", 5, theme.accent);
    } else if (!strcmp(line, "@data")) {
        ui_set_text(task.step, L("正在暂停数据连接…", "Pausing mobile data…"));
        ui_meter_set(&task.meter, "15%", 15, theme.accent);
    } else if (!strcmp(line, "@modem")) {
        ui_set_text(task.step, L("正在停止 ModemManager…", "Stopping ModemManager…"));
        ui_meter_set(&task.meter, "25%", 25, theme.accent);
    } else if (!strcmp(line, "@reset")) {
        ui_set_text(task.step, L("正在重启基带…", "Restarting the baseband…"));
        ui_meter_set(&task.meter, "40%", 40, theme.accent);
    } else if (!strcmp(line, "@restore")) {
        ui_set_text(task.step, L("正在恢复 SIM 网络设置…", "Restoring SIM network settings…"));
        ui_meter_set(&task.meter, "70%", 70, theme.accent);
    } else if (!strcmp(line, "@verify")) {
        ui_set_text(task.step, L("正在验证射频和数据连接…", "Verifying radio and data connections…"));
        ui_meter_set(&task.meter, "90%", 90, theme.accent);
    } else if (!strcmp(line, "@done")) {
        ui_meter_set(&task.meter, "100%", 100, theme.accent);
    } else if (!strncmp(line, "@error ", 7)) {
        char text[480];
        error_text(line + 7, text, sizeof text);
        snprintf(task.error, sizeof task.error, "%s", text);
    }
}

static void exited(int status)
{
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        succeeded();
    else
        failed(*task.error ? task.error : L("基带重启失败", "Baseband restart failed"));
}

void modem_restart_run(void (*done)(void))
{
    dialog_open(L("重启基带", "Restart baseband"), NULL);
    task = (typeof(task)){.done = done};
    lv_obj_t *sheet = dialog_sheet();
    task.step = ui_text(sheet, 13, false, theme.text);
    lv_obj_set_width(task.step, LV_PCT(100));
    lv_label_set_long_mode(task.step, LV_LABEL_LONG_MODE_WRAP);
    ui_set_text(task.step, L("正在准备…", "Preparing…"));
    task.meter = ui_meter(sheet, L("进度", "Progress"));
    task.button = dialog_button(L("完成后关闭", "Closes when finished"), on_close, NULL);
    lv_obj_add_state(task.button, LV_STATE_DISABLED);
    dialog_set_closable(false);
    stk_suspend_for_modem_restart();
    if (!child_start(&task.child, (char *const[]){"/usr/bin/python3", "-c", (char *)MODEM_RESTART_SCRIPT, NULL}, progress,
                     exited)) {
        stk_resume_after_modem_restart();
        dialog_close();
        toast("%s", L("无法启动基带重启", "Cannot start baseband restart"));
    }
}
