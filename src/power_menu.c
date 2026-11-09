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

static lv_obj_t *root, *sheet, *status;
static lv_group_t *group, *previous_group;
static int selected = -1;
static bool busy;

bool power_menu_is_open(void)
{
    return root != NULL;
}

void power_menu_close(void)
{
    if (!root || busy)
        return;
    lv_obj_delete(root);
    root = NULL;
    lv_group_delete(group);
    group = NULL;
    input_set_group(previous_group);
    selected = -1;
    idle_reset();
}

static void paint(void);

static void executed(int code, cJSON *r, void *ctx)
{
    if (api_reply_ok(r) || !root)
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

static void on_backdrop(lv_event_t *e)
{
    if (lv_event_get_target_obj(e) == root)
        power_menu_close();
}

static void paint(void)
{
    lv_obj_clean(sheet);
    lv_group_remove_all_objs(group);
    lv_group_t *outer = lv_group_get_default();
    lv_group_set_default(group);
    lv_obj_t *title = ui_text(sheet, 21, true, theme.text), *first = NULL;
    lv_obj_set_style_pad_bottom(title, S(8), 0);
    if (selected < 0) {
        lv_label_set_text(title, L("电源菜单", "Power menu"));
        for (int i = 0; i < 3; i++) {
            lv_obj_t *row = ui_row(sheet, actions[i].icon, L(actions[i].zh, actions[i].en), NULL, ROW_CHEVRON, on_action,
                                   (void *)(intptr_t)i);
            if (!strcmp(actions[i].id, "poweroff"))
                lv_obj_set_style_text_color(lv_obj_get_child(row, 0), theme.poor, 0);
            if (!first)
                first = row;
        }
    } else {
        lv_label_set_text(title, L(actions[selected].zh, actions[selected].en));
        lv_obj_t *note = ui_note(sheet, "");
        lv_obj_set_style_text_font(note, F(13), 0);
        if (!strcmp(actions[selected].id, "android_once"))
            lv_label_set_text(note, L("重启进入 Android，并将其设为默认启动系统。",
                                      "Restart into Android and make it the default system."));
        else if (app.en) {
            char name[32];
            snprintf(name, sizeof name, "%s", actions[selected].en);
            for (char *c = name; *c; c++)
                *c = (char)tolower((unsigned char)*c);
            lv_label_set_text_fmt(note, "Confirm %s?", name);
        } else {
            lv_label_set_text_fmt(note, "确认%s？", actions[selected].zh);
        }
        lv_obj_t *confirm = ui_button(sheet, L("确认", "Confirm"), on_confirm, NULL);
        lv_obj_set_width(confirm, LV_PCT(100));
        ui_primary(confirm);
    }
    status = ui_note(sheet, "");
    lv_obj_set_style_text_font(status, F(13), 0);
    lv_obj_set_hidden(status, true);
    lv_obj_t *cancel = ui_button(sheet, L("取消", "Cancel"), on_cancel, NULL);
    lv_obj_set_width(cancel, LV_PCT(100));
    lv_obj_set_style_bg_opa(cancel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(cancel, 0), theme.dim, 0);
    /* The keys start from the first action; a confirmation starts from Cancel. */
    lv_group_focus_obj(first ? first : cancel);
    lv_group_set_default(outer);
}

static void appear(void *obj, int32_t v)
{
    lv_obj_set_style_opa(obj, (lv_opa_t)(v * 255 / 100), 0);
    lv_obj_set_style_translate_y(obj, S(12) * (100 - v) / 100, 0);
    lv_obj_set_style_transform_scale(obj, 248 + 8 * v / 100, 0);
}

void power_menu_open(void)
{
    if (root)
        return;
    if (app.blank)
        screen_set_blank(false);
    if (app.blank)
        return;
    previous_group = input_group();
    group = lv_group_create();
    input_set_group(group);
    root = lv_obj_create(lv_layer_sys());
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_60, 0);
    lv_obj_set_style_pad_all(root, S(18), 0);
    lv_obj_add_event_cb(root, on_backdrop, LV_EVENT_CLICKED, NULL);
    sheet = ui_box(root, LV_FLEX_FLOW_COLUMN, 8);
    lv_obj_center(sheet);
    lv_obj_set_clickable(sheet, true);
    lv_obj_set_style_bg_color(sheet, theme.bg, 0);
    lv_obj_set_style_bg_opa(sheet, theme.surface_opa, 0);
    lv_obj_set_style_border_color(sheet, theme.line, 0);
    lv_obj_set_style_border_width(sheet, 1, 0);
    lv_obj_set_style_radius(sheet, S(24), 0);
    lv_obj_set_style_pad_all(sheet, S(16), 0);
    lv_obj_set_style_pad_top(sheet, S(20), 0);
    lv_obj_set_style_transform_pivot_x(sheet, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(sheet, LV_PCT(50), 0);
    selected = -1;
    busy = false;
    paint();
    if (theme.motion) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, sheet);
        lv_anim_set_exec_cb(&a, appear);
        lv_anim_set_values(&a, 0, 100);
        lv_anim_set_duration(&a, 200);
        lv_anim_set_path_cb(&a, ui_ease_out);
        lv_anim_start(&a);
    }
    idle_reset();
    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "ms", 80);
    api_post("/vibrate", body, NULL, NULL);
}

bool power_menu_key(const struct key_event *key)
{
    if (!root)
        return false;
    if (busy)
        return true;
    switch (key->kind) {
    case KEY_KIND_BACK:
        on_cancel(NULL);
        break;
    case KEY_KIND_OK:
        if (!key->repeat)
            input_focus_key(LV_KEY_ENTER);
        break;
    case KEY_KIND_UP:
    case KEY_KIND_LEFT:
        input_focus_move(group, -1);
        break;
    case KEY_KIND_DOWN:
    case KEY_KIND_RIGHT:
        input_focus_move(group, 1);
        break;
    default:
        break;
    }
    return true;
}
