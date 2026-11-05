#include "ui.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "icons.h"
#include "theme.h"

static void bare(lv_obj_t *obj)
{
    lv_obj_remove_style_all(obj);
    lv_obj_set_clickable(obj, false);
    lv_obj_set_scrollable(obj, false);
}

lv_obj_t *ui_box(lv_obj_t *parent, lv_flex_flow_t flow, int gap)
{
    lv_obj_t *box = lv_obj_create(parent);
    bare(box);
    lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, flow);
    lv_obj_set_style_pad_gap(box, S(gap), 0);
    if (flow == LV_FLEX_FLOW_ROW || flow == LV_FLEX_FLOW_ROW_WRAP)
        lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return box;
}

lv_obj_t *ui_page(lv_obj_t *parent)
{
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(page, S(theme.compact ? 5 : 8), 0);
    lv_obj_set_style_pad_hor(page, S(12), 0);
    lv_obj_set_style_pad_bottom(page, S(14), 0);
    lv_obj_set_style_pad_gap(page, S(theme.compact ? 6 : 10), 0);
    lv_obj_set_scroll_dir(page, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_momentum(page, true);
    return page;
}

int ui_stack_gap(void)
{
    return theme.compact ? 8 : 10;
}

lv_obj_t *ui_card(lv_obj_t *parent)
{
    lv_obj_t *card = ui_box(parent, LV_FLEX_FLOW_COLUMN, 6);
    lv_obj_set_style_bg_color(card, theme.surface, 0);
    lv_obj_set_style_bg_opa(card, theme.surface_opa, 0);
    lv_obj_set_style_radius(card, S(theme.compact ? 12 : 15), 0);
    lv_obj_set_style_pad_ver(card, S(theme.compact ? 9 : 12), 0);
    lv_obj_set_style_pad_hor(card, S(theme.compact ? 11 : 12), 0);
    return card;
}

lv_obj_t *ui_text(lv_obj_t *parent, int size, bool bold, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, bold ? FB(size) : F(size), 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_label_set_text_static(label, "");
    return label;
}

lv_obj_t *ui_kv(lv_obj_t *card, const char *label)
{
    lv_obj_t *row = ui_box(card, LV_FLEX_FLOW_ROW, 10);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_t *name = ui_text(row, 13, false, theme.dim);
    lv_label_set_text(name, label);
    lv_obj_t *value = ui_text(row, 13, false, theme.text);
    lv_obj_set_flex_grow(value, 1);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(value, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text_static(value, "--");
    return value;
}

static void focus_style(lv_obj_t *obj)
{
    lv_obj_set_style_outline_color(obj, theme.accent, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_width(obj, 2, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_pad(obj, -2, LV_STATE_FOCUS_KEY);
}

void ui_focusable(lv_obj_t *obj)
{
    focus_style(obj);
    /* Only input fields take focus from a tap; elsewhere it is the arrow keys. */
    if (!lv_obj_check_type(obj, &lv_textarea_class))
        lv_obj_set_click_focusable(obj, false);
    lv_group_t *group = lv_group_get_default();
    if (group)
        lv_group_add_obj(group, obj);
}

lv_obj_t *ui_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *ctx)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_remove_style_all(button);
    lv_obj_set_style_bg_color(button, theme.button, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(button, lv_color_mix(lv_color_white(), theme.button, 40), LV_STATE_PRESSED);
    lv_obj_set_style_opa(button, LV_OPA_50, LV_STATE_DISABLED);
    lv_obj_set_style_radius(button, S(11), 0);
    lv_obj_set_style_pad_ver(button, S(10), 0);
    lv_obj_set_style_pad_hor(button, S(12), 0);
    lv_obj_set_height(button, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(button, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(button, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *label = ui_text(button, 14, false, theme.text);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    if (cb)
        lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, ctx);
    ui_focusable(button);
    return button;
}

void ui_button_text(lv_obj_t *button, const char *text)
{
    ui_set_text(lv_obj_get_child(button, 0), text);
}

lv_obj_t *ui_note(lv_obj_t *parent, const char *text)
{
    lv_obj_t *note = ui_text(parent, 10, false, theme.dim);
    lv_obj_set_width(note, LV_PCT(100));
    lv_obj_set_style_pad_hor(note, S(10), 0);
    lv_obj_set_style_text_line_space(note, S(3), 0);
    lv_label_set_long_mode(note, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(note, text);
    return note;
}

lv_obj_t *ui_heading(lv_obj_t *parent, const char *text)
{
    lv_obj_t *heading = ui_text(parent, 12, false, theme.dim);
    lv_obj_set_style_pad_left(heading, S(4), 0);
    lv_obj_set_style_pad_top(heading, S(4), 0);
    lv_label_set_text(heading, text);
    return heading;
}

/* The name keeps at most 48 % of the row, as the web page's does, so a long value
 * wraps beside it instead of squeezing it away. */
static void cap_name(lv_event_t *e)
{
    lv_obj_t *row = lv_event_get_target_obj(e);
    lv_obj_set_style_max_width(lv_event_get_user_data(e), lv_obj_get_content_width(row) * 48 / 100, 0);
}

lv_obj_t *ui_row(lv_obj_t *parent, const char *icon, const char *label, const char *value, unsigned flags,
                 lv_event_cb_t cb, void *ctx)
{
    lv_obj_t *row = ui_box(parent, LV_FLEX_FLOW_ROW, 10);
    lv_obj_set_style_bg_color(row, (flags & ROW_DANGER) ? theme.danger : theme.surface, 0);
    lv_obj_set_style_bg_opa(row, theme.surface_opa, 0);
    lv_obj_set_style_radius(row, S(12), 0);
    lv_obj_set_style_pad_hor(row, S(theme.compact ? 11 : 12), 0);
    lv_obj_set_style_pad_ver(row, S(theme.compact ? 9 : 12), 0);
    lv_obj_set_style_min_height(row, S(theme.compact ? 42 : 46), 0);
    if (icon) {
        lv_obj_t *glyph = icon_create(row, icon, S(20));
        lv_obj_set_style_text_color(glyph, theme.accent, 0);
    }
    lv_obj_t *name = ui_text(row, 13, false, theme.text);
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(name, label);
    if (!value || !*value) {
        lv_obj_set_flex_grow(name, 1);
    } else {
        /* A pixel cap, not a percentage: a wrapping label measures itself against
         * the raw value, so LV_PCT would let the name run on one line. */
        lv_obj_add_event_cb(row, cap_name, LV_EVENT_SIZE_CHANGED, name);
        lv_obj_t *text = ui_text(row, 12, false, (flags & ROW_ON) ? theme.accent : theme.dim);
        lv_obj_set_flex_grow(text, 1);
        lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_long_mode(text, LV_LABEL_LONG_MODE_WRAP);
        lv_label_set_text(text, value);
    }
    if (flags & ROW_CHECK)
        lv_obj_set_style_text_color(icon_create(row, "check", S(18)), theme.accent, 0);
    else if (flags & ROW_CHEVRON)
        lv_label_set_text_static(ui_text(row, 16, false, theme.dim), "›");
    if (!(flags & ROW_INFO)) {
        lv_obj_set_clickable(row, true);
        lv_obj_set_style_bg_color(row, lv_color_mix(lv_color_white(), theme.surface, 30), LV_STATE_PRESSED);
        lv_obj_set_style_opa(row, LV_OPA_50, LV_STATE_DISABLED);
        if (cb)
            lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, ctx);
        ui_focusable(row);
    }
    return row;
}

struct ui_meter ui_meter(lv_obj_t *parent, const char *title)
{
    struct ui_meter m;
    lv_obj_t *head = ui_box(parent, LV_FLEX_FLOW_ROW, 8);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_label_set_text(ui_text(head, 12, false, theme.dim), title);
    m.value = ui_text(head, 12, false, theme.text);
    lv_label_set_text_static(m.value, "--");
    m.bar = lv_bar_create(parent);
    lv_obj_remove_style_all(m.bar);
    lv_obj_set_size(m.bar, LV_PCT(100), S(5));
    lv_obj_set_style_bg_color(m.bar, theme.line, 0);
    lv_obj_set_style_bg_opa(m.bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(m.bar, S(4), 0);
    lv_obj_set_style_bg_color(m.bar, theme.accent, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(m.bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(m.bar, S(4), LV_PART_INDICATOR);
    lv_bar_set_range(m.bar, 0, 100);
    return m;
}

void ui_meter_set(struct ui_meter *m, const char *text, int percent, lv_color_t color)
{
    ui_set_text(m->value, text);
    lv_bar_set_value(m->bar, percent, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(m->bar, color, LV_PART_INDICATOR);
}

void ui_set_text(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text ? text : ""))
        lv_label_set_text(label, text ? text : "");
}

void ui_set_fmt(lv_obj_t *label, const char *fmt, ...)
{
    char text[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof text, fmt, args);
    va_end(args);
    ui_set_text(label, text);
}

int32_t ui_ease_out(const lv_anim_t *a)
{
    float t = a->duration > 0 ? (float)a->act_time / (float)a->duration : 1;
    float rest = 1 - (t < 0 ? 0 : t > 1 ? 1 : t);
    return a->start_value + (int32_t)lroundf((float)(a->end_value - a->start_value) * (1 - rest * rest * rest));
}
