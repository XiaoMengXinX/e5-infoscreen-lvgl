#include "ui.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "icons.h"
#include "input.h"
#include "keyboard.h"
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

/* A page that fits still scrolls and bounces: its content counts as a pixel taller than it is. */
static void page_self_size(lv_event_t *e)
{
    lv_point_t *size = lv_event_get_param(e);
    size->y = lv_obj_get_content_height(lv_event_get_target_obj(e)) + 1;
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
    lv_obj_add_event_cb(page, page_self_size, LV_EVENT_GET_SELF_SIZE, NULL);
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

static void fill_button(lv_obj_t *button, lv_color_t bg, lv_color_t fg)
{
    lv_obj_set_style_bg_color(button, bg, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(button, 0), fg, 0);
    /* Key focus keeps the usual ring on the edge, in the text color, with a dark gap so it
     * reads against the fill; the padding gives up what the gap border takes, so the button
     * keeps its size. */
    int32_t gap = 2 + S(3);
    lv_obj_set_style_outline_color(button, theme.text, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_border_color(button, theme.bg, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_border_width(button, gap, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_pad_ver(button, S(10) - gap, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_pad_hor(button, S(12) - gap, LV_STATE_FOCUS_KEY);
}

void ui_primary(lv_obj_t *button)
{
    fill_button(button, theme.accent, theme.bg);
}

void ui_danger(lv_obj_t *button)
{
    fill_button(button, theme.danger, theme.text);
}

void ui_figure(lv_obj_t *parent, const char *title, lv_obj_t **value, lv_obj_t **note)
{
    lv_obj_t *col = ui_box(parent, LV_FLEX_FLOW_COLUMN, 2);
    lv_obj_set_width(col, 0);
    lv_obj_set_flex_grow(col, 1);
    lv_label_set_text(ui_text(col, 11, false, theme.dim), title);
    *value = ui_text(col, 16, true, theme.text);
    lv_label_set_text_static(*value, "—");
    *note = ui_text(col, 10, false, theme.dim);
}

lv_obj_t *ui_field(lv_obj_t *parent, uint32_t max)
{
    lv_obj_t *area = lv_textarea_create(parent);
    lv_obj_set_width(area, LV_PCT(100));
    lv_textarea_set_one_line(area, true);
    lv_textarea_set_max_length(area, max);
    lv_obj_set_style_text_font(area, F(14), 0);
    lv_obj_set_style_text_color(area, theme.text, 0);
    lv_obj_set_style_text_color(area, theme.dim, LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_bg_color(area, theme.surface, 0);
    lv_obj_set_style_bg_opa(area, theme.surface_opa, 0);
    lv_obj_set_style_border_color(area, theme.line, 0);
    lv_obj_set_style_border_color(area, theme.accent, LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(area, 1, 0);
    lv_obj_set_style_radius(area, S(10), 0);
    lv_obj_set_style_pad_all(area, S(8), 0);
    keyboard_bind(area);
    ui_focusable(area);
    return area;
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

void ui_focus_key(lv_obj_t *obj)
{
    lv_group_focus_obj(obj);
    lv_obj_add_state(obj, LV_STATE_FOCUS_KEY);
}

void ui_focus_entry(lv_obj_t *entry)
{
    if (!entry)
        return;
    ui_focus_key(entry);
    lv_obj_scroll_to_view_recursive(entry, LV_ANIM_ON);
}

lv_obj_t *ui_end_entry(lv_obj_t *parent, bool last)
{
    uint32_t n = lv_obj_get_child_count(parent);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *child = lv_obj_get_child(parent, (int32_t)(last ? n - 1 - i : i));
        if (lv_obj_is_hidden(child))
            continue;
        if (lv_obj_get_group(child) == input_group() && !lv_obj_has_state(child, LV_STATE_DISABLED))
            return child;
        if ((child = ui_end_entry(child, last)))
            return child;
    }
    return NULL;
}

bool ui_inside(lv_obj_t *obj, lv_obj_t *parent)
{
    while (obj && obj != parent)
        obj = lv_obj_get_parent(obj);
    return obj != NULL;
}

lv_obj_t *ui_labeled_field(lv_obj_t *parent, const char *label, uint32_t max)
{
    lv_obj_t *box = ui_box(parent, LV_FLEX_FLOW_COLUMN, 4);
    lv_label_set_text(ui_text(box, 12, false, theme.dim), label);
    return ui_field(box, max);
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

lv_obj_t *ui_choices(lv_obj_t *parent)
{
    lv_obj_t *row = ui_box(parent, LV_FLEX_FLOW_ROW, 6);
    lv_obj_set_style_radius(row, S(11), 0);
    lv_obj_set_style_outline_color(row, theme.accent, 0);
    lv_obj_set_style_outline_pad(row, S(4), 0);
    return row;
}

lv_obj_t *ui_choice(lv_obj_t *row, const char *text, lv_event_cb_t cb, void *ctx)
{
    lv_obj_t *button = ui_button(row, text, cb, ctx);
    lv_obj_set_flex_grow(button, 1);
    lv_obj_set_style_pad_hor(button, S(4), 0);
    lv_obj_set_style_text_font(lv_obj_get_child(button, 0), F(12), 0);
    lv_group_remove_obj(button);
    return button;
}

void ui_choices_paint(lv_obj_t *row, int selected, bool keyed)
{
    for (int32_t i = 0; i < (int32_t)lv_obj_get_child_count(row); i++) {
        lv_obj_t *button = lv_obj_get_child(row, i);
        lv_obj_set_style_bg_color(button, i == selected ? theme.active : theme.button, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(button, 0), i == selected ? theme.accent : theme.text, 0);
    }
    lv_obj_set_style_outline_width(row, keyed ? 2 : 0, 0);
}

lv_obj_t *ui_sim_tabs(lv_obj_t *parent, lv_event_cb_t cb)
{
    lv_obj_t *tabs = ui_box(parent, LV_FLEX_FLOW_ROW, 0);
    lv_obj_set_style_bg_color(tabs, theme.surface, 0);
    lv_obj_set_style_bg_opa(tabs, theme.surface_opa, 0);
    lv_obj_set_style_radius(tabs, S(18), 0);
    lv_obj_set_style_pad_all(tabs, S(3), 0);
    for (int c = 0; c < 2; c++) {
        lv_obj_t *tab = ui_button(tabs, c ? "SIM 2" : "SIM 1", cb, (void *)(intptr_t)c);
        lv_group_remove_obj(tab);
        lv_obj_set_flex_grow(tab, 1);
        lv_obj_set_style_pad_ver(tab, S(5), 0);
        lv_obj_set_style_radius(tab, S(15), 0);
        lv_obj_set_style_bg_color(tab, theme.active, 0);
        lv_obj_set_style_text_font(lv_obj_get_child(tab, 0), F(12), 0);
    }
    return tabs;
}

void ui_sim_tabs_paint(lv_obj_t *tabs, int current, bool keyed)
{
    for (int32_t c = 0; c < 2; c++) {
        lv_obj_t *tab = lv_obj_get_child(tabs, c);
        lv_obj_set_style_bg_opa(tab, c == current ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(tab, 0), c == current ? theme.accent : theme.dim, 0);
        lv_obj_set_state(tab, LV_STATE_FOCUS_KEY, keyed && c == current);
    }
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

float ui_bezier(float t, float x1, float y1, float x2, float y2)
{
    if (t <= 0 || t >= 1)
        return t <= 0 ? 0 : 1;
    float ax = 1 - 3 * x2 + 3 * x1, bx = 3 * x2 - 6 * x1, cx = 3 * x1;
    float lo = 0, hi = 1, u = t;
    for (int i = 0; i < 12; i++) {
        float error = ((ax * u + bx) * u + cx) * u - t;
        if (fabsf(error) < 0.000001f)
            break;
        if (error < 0)
            lo = u;
        else
            hi = u;
        float derivative = (3 * ax * u + 2 * bx) * u + cx;
        float next = derivative > 0.000001f ? u - error / derivative : -1;
        u = next > lo && next < hi ? next : (lo + hi) / 2;
    }
    return 3 * (1 - u) * (1 - u) * u * y1 + 3 * (1 - u) * u * u * y2 + u * u * u;
}

float ui_release_progress(float t, float slope)
{
    return t * (slope + t * (3 - 2 * slope + t * (slope - 2)));
}

int32_t ui_ease_release(const lv_anim_t *a)
{
    float t = (float)a->act_time / a->duration;
    float slope = *(const float *)a->user_data;
    return a->start_value + (int32_t)lroundf((a->end_value - a->start_value) * ui_release_progress(t, slope));
}

void ui_spring(float *x, float *v, float target, float response, float damping, float dt)
{
    float w = 2 * 3.14159265f / response, y = *x - target;
    float decay = expf(-damping * w * dt);
    if (damping == 1) {
        float c = *v + w * y;
        *x = target + (y + c * dt) * decay;
        *v = (*v - w * c * dt) * decay;
    } else {
        float wd = w * sqrtf(1 - damping * damping), s = sinf(wd * dt), c = cosf(wd * dt);
        *x = target + decay * (y * c + (*v + damping * w * y) * s / wd);
        *v = decay * (*v * c - (damping * w * *v + w * w * y) * s / wd);
    }
}

int32_t ui_ease_slide(const lv_anim_t *a)
{
    float t = a->duration > 0 ? (float)a->act_time / (float)a->duration : 1;
    return a->start_value + (int32_t)lroundf((float)(a->end_value - a->start_value) * ui_bezier(t, 0.3f, 0, 0, 1));
}

int32_t ui_ease_out(const lv_anim_t *a)
{
    float t = a->duration > 0 ? (float)a->act_time / (float)a->duration : 1;
    float rest = 1 - (t < 0 ? 0 : t > 1 ? 1 : t);
    return a->start_value + (int32_t)lroundf((float)(a->end_value - a->start_value) * (1 - rest * rest * rest));
}
