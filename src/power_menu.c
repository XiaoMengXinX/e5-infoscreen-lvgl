/* The settings API owns shutdown and boot selection, including NV cleanup. */
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "api.h"
#include "app.h"
#include "icons.h"
#include "input.h"
#include "theme.h"
#include "ui.h"
#include "util.h"

static const struct {
    const char *id, *icon, *zh, *en;
} actions[] = {
    {"poweroff", "power", "关机", "Power off"},
    {"reboot", "restart", "重启", "Restart"},
    {"android_once", "android", "切换到 Android", "Switch to Android"},
};

static lv_obj_t *status;
static int selected = -1;
static bool open, busy;

bool power_menu_is_open(void)
{
    return open;
}

static void closed(void)
{
    open = false;
    selected = -1;
}

void power_menu_close(void)
{
    if (!open || busy)
        return;
    dialog_close();
}

static void paint(void);

static void executed(int code, cJSON *r, void *ctx)
{
    if (api_reply_ok(r) || !open)
        return;
    busy = false;
    paint();
    ui_set_text(status, L("操作失败，请重试", "Operation failed. Try again."));
    lv_obj_set_hidden(status, false);
}

static void on_action(lv_event_t *e)
{
    selected = (int)(intptr_t)lv_event_get_user_data(e);
    paint();
}

static void on_confirm(lv_event_t *e)
{
    if (busy || selected < 0)
        return;
    busy = true;
    dialog_set_closable(false);
    lv_obj_t *sheet = dialog_sheet();
    for (uint32_t i = 0; i < lv_obj_get_child_count(sheet); i++)
        lv_obj_add_state(lv_obj_get_child(sheet, (int32_t)i), LV_STATE_DISABLED);
    ui_set_text(status, L("正在执行…", "Working…"));
    lv_obj_set_hidden(status, false);
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "id", actions[selected].id);
    api_post("/settings/system", body, executed, NULL);
}

static void on_cancel(lv_event_t *e)
{
    if (busy)
        return;
    if (selected >= 0) {
        selected = -1;
        paint();
    } else {
        power_menu_close();
    }
}

/* The menu is a dialog, reopened for the list and for an action's confirmation. */
static void paint(void)
{
    dialog_on_close(NULL);
    if (selected < 0) {
        dialog_open(L("电源菜单", "Power menu"), NULL);
    } else {
        char note[96];
        if (!strcmp(actions[selected].id, "android_once")) {
            snprintf(note, sizeof note, "%s", L("重启进入 Android，并将其设为默认启动系统。",
                                                "Restart into Android and make it the default system."));
        } else if (app.en) {
            char name[32];
            snprintf(name, sizeof name, "%s", actions[selected].en);
            for (char *c = name; *c; c++)
                *c = (char)tolower((unsigned char)*c);
            snprintf(note, sizeof note, "Confirm %s?", name);
        } else {
            snprintf(note, sizeof note, "确认%s？", actions[selected].zh);
        }
        dialog_open(L(actions[selected].zh, actions[selected].en), note);
    }
    dialog_on_close(closed);
    open = true;
    lv_obj_t *sheet = dialog_sheet(), *first = NULL;
    if (selected < 0) {
        lv_group_t *outer = lv_group_get_default();
        lv_group_set_default(input_group());
        for (int i = 0; i < 3; i++) {
            lv_obj_t *row = ui_row(sheet, actions[i].icon, L(actions[i].zh, actions[i].en), NULL, ROW_CHEVRON, on_action,
                                   (void *)(intptr_t)i);
            if (!strcmp(actions[i].id, "poweroff"))
                lv_obj_set_style_text_color(lv_obj_get_child(row, 0), theme.poor, 0);
            if (!first)
                first = row;
        }
        lv_group_set_default(outer);
    } else {
        ui_primary(dialog_button(L("确认", "Confirm"), on_confirm, NULL));
    }
    status = ui_note(sheet, "");
    lv_obj_set_style_text_font(status, F(13), 0);
    lv_obj_set_hidden(status, true);
    lv_obj_t *cancel = dialog_button_quiet(L("取消", "Cancel"), on_cancel, NULL);
    /* The keys start from the first action; a confirmation starts from Cancel. */
    lv_group_focus_obj(first ? first : cancel);
}

static void appear(void *obj, int32_t v)
{
    lv_obj_set_style_opa(obj, (lv_opa_t)(v * 255 / 100), 0);
    lv_obj_set_style_translate_y(obj, S(12) * (100 - v) / 100, 0);
    lv_obj_set_style_transform_scale(obj, 248 + 8 * v / 100, 0);
}

void power_menu_open(void)
{
    if (open)
        return;
    if (app.blank)
        screen_set_blank(false);
    if (app.blank)
        return;
    selected = -1;
    busy = false;
    paint();
    if (theme.motion) {
        lv_obj_t *sheet = dialog_sheet();
        lv_obj_set_style_transform_pivot_x(sheet, LV_PCT(50), 0);
        lv_obj_set_style_transform_pivot_y(sheet, LV_PCT(50), 0);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, sheet);
        lv_anim_set_exec_cb(&a, appear);
        lv_anim_set_values(&a, 0, 100);
        lv_anim_set_duration(&a, 200);
        lv_anim_set_path_cb(&a, ui_ease_out);
        lv_anim_start(&a);
    }
    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "ms", 80);
    api_post("/vibrate", body, NULL, NULL);
}

/* Back steps out of a confirmation before it closes the menu. */
bool power_menu_key(const struct key_event *key)
{
    if (!open)
        return false;
    if (busy)
        return true;
    if (key->kind == KEY_KIND_BACK)
        on_cancel(NULL);
    else
        dialog_key(key);
    return true;
}
