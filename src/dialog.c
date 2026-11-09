#include "app.h"
#include "input.h"
#include "theme.h"
#include "ui.h"

static lv_obj_t *root, *sheet;
static lv_group_t *group, *previous_group;
static void (*closed)(void);

bool dialog_is_open(void)
{
    return root != NULL;
}

void dialog_close(void)
{
    if (!root)
        return;
    void (*cb)(void) = closed;
    closed = NULL;
    lv_obj_delete(root);
    root = NULL;
    lv_group_delete(group);
    group = NULL;
    input_set_group(previous_group);
    idle_reset();
    if (cb)
        cb();
}

lv_obj_t *dialog_sheet(void)
{
    return sheet;
}

void dialog_on_close(void (*cb)(void))
{
    closed = cb;
}

static void on_backdrop(lv_event_t *e)
{
    if (lv_event_get_target_obj(e) == root)
        dialog_close();
}

void dialog_open(const char *title, const char *note)
{
    dialog_close();
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
    lv_obj_set_style_max_height(sheet, LV_PCT(100), 0);
    lv_obj_set_scrollable(sheet, true);
    lv_obj_set_scroll_dir(sheet, LV_DIR_VER);
    lv_obj_set_clickable(sheet, true);
    lv_obj_set_style_bg_color(sheet, theme.bg, 0);
    lv_obj_set_style_bg_opa(sheet, theme.surface_opa, 0);
    lv_obj_set_style_border_color(sheet, theme.line, 0);
    lv_obj_set_style_border_width(sheet, 1, 0);
    lv_obj_set_style_radius(sheet, S(24), 0);
    lv_obj_set_style_pad_all(sheet, S(16), 0);
    lv_obj_set_style_pad_top(sheet, S(20), 0);
    lv_obj_t *heading = ui_text(sheet, 21, true, theme.text);
    lv_obj_set_width(heading, LV_PCT(100));
    lv_label_set_text(heading, title);
    if (note)
        lv_obj_set_style_text_font(ui_note(sheet, note), F(13), 0);
    idle_reset();
}

lv_obj_t *dialog_button(const char *text, lv_event_cb_t cb, void *ctx)
{
    lv_group_t *outer = lv_group_get_default();
    lv_group_set_default(group);
    lv_obj_t *button = ui_button(sheet, text, cb, ctx);
    lv_group_set_default(outer);
    lv_obj_set_width(button, LV_PCT(100));
    if (lv_group_get_obj_count(group) == 1)
        lv_group_focus_obj(button);
    return button;
}

static void on_cancel(lv_event_t *e)
{
    dialog_close();
}

void dialog_cancel(void)
{
    lv_obj_t *cancel = dialog_button(L("取消", "Cancel"), on_cancel, NULL);
    lv_obj_set_style_bg_opa(cancel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(cancel, 0), theme.dim, 0);
}

bool dialog_key(const struct key_event *key)
{
    if (!root)
        return false;
    switch (key->kind) {
    case KEY_KIND_BACK:
        dialog_close();
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
