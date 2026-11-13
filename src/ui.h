#pragma once

#include <stdbool.h>

#include "lvgl.h"

enum {
    ROW_CHEVRON = 1 << 0,
    ROW_ON = 1 << 1,
    ROW_INFO = 1 << 2,
    ROW_CHECK = 1 << 3,
    ROW_DANGER = 1 << 4,
};

struct ui_meter {
    lv_obj_t *value;
    lv_obj_t *bar;
};

lv_obj_t *ui_box(lv_obj_t *parent, lv_flex_flow_t flow, int gap);
lv_obj_t *ui_page(lv_obj_t *parent);
lv_obj_t *ui_card(lv_obj_t *parent);
lv_obj_t *ui_text(lv_obj_t *parent, int size, bool bold, lv_color_t color);
lv_obj_t *ui_kv(lv_obj_t *card, const char *label);
lv_obj_t *ui_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *ctx);
lv_obj_t *ui_note(lv_obj_t *parent, const char *text);
/* A row of choices, worked by touch, or by Left and Right once Enter hands the
 * arrow keys to it; the outline shows they have. */
lv_obj_t *ui_choices(lv_obj_t *parent);
lv_obj_t *ui_choice(lv_obj_t *row, const char *text, lv_event_cb_t cb, void *ctx);
void ui_choices_paint(lv_obj_t *row, int selected, bool keyed);
lv_obj_t *ui_heading(lv_obj_t *parent, const char *text);
lv_obj_t *ui_row(lv_obj_t *parent, const char *icon, const char *label, const char *value, unsigned flags,
                 lv_event_cb_t cb, void *ctx);
struct ui_meter ui_meter(lv_obj_t *parent, const char *title);
/* One of the equal columns in a row: a title, a large value and a note under it. */
void ui_figure(lv_obj_t *parent, const char *title, lv_obj_t **value, lv_obj_t **note);

void ui_set_text(lv_obj_t *label, const char *text);
void ui_set_fmt(lv_obj_t *label, const char *fmt, ...);
void ui_button_text(lv_obj_t *button, const char *text);
void ui_meter_set(struct ui_meter *meter, const char *text, int percent, lv_color_t color);
void ui_focusable(lv_obj_t *obj);
/* Focuses obj and shows the focus as the keys do. */
void ui_focus_key(lv_obj_t *obj);
/* ui_focus_key scrolled into view; NULL does nothing. */
void ui_focus_entry(lv_obj_t *entry);
/* The first (or last) descendant of parent the keys can reach. */
lv_obj_t *ui_end_entry(lv_obj_t *parent, bool last);
bool ui_inside(lv_obj_t *obj, lv_obj_t *parent);
/* A one-line text field with the on-screen keyboard; max 0 is unlimited. Set its
 * text after any accepted characters, which filter it. */
lv_obj_t *ui_field(lv_obj_t *parent, uint32_t max);
/* A ui_field under a dim caption. */
lv_obj_t *ui_labeled_field(lv_obj_t *parent, const char *label, uint32_t max);
/* The SIM 1 / SIM 2 switch above an app's content; cb gets the slot as its user
 * data. The keys reach it only through Left and Right on it. */
lv_obj_t *ui_sim_tabs(lv_obj_t *parent, lv_event_cb_t cb);
void ui_sim_tabs_paint(lv_obj_t *tabs, int current, bool keyed);
/* Fills a ui_button with the accent color as the main action. */
void ui_primary(lv_obj_t *button);
/* Fills a ui_button with the danger color as the destructive action. */
void ui_danger(lv_obj_t *button);

/* A small spinner, hidden and still until shown; track is the color behind it. */
lv_obj_t *ui_spinner(lv_obj_t *parent, int size, lv_color_t track);
void ui_spinner_show(lv_obj_t *spinner, bool on);

/* The gap between the blocks of a list or a nested view, tighter when compact. */
int ui_stack_gap(void);

/* The progress at time t, 0 to 1, of CSS cubic-bezier(x1, y1, x2, y2). */
float ui_bezier(float t, float x1, float y1, float x2, float y2);
/* Monotone release with an initial slope in [0, 3] and zero final speed. */
float ui_release_progress(float t, float slope);
/* The animation's user_data points to its initial normalized slope. */
int32_t ui_ease_release(const lv_anim_t *a);
/* response is the natural period in seconds; damping is in (0, 1]. */
void ui_spring(float *x, float *v, float target, float response, float damping, float dt);
int32_t ui_ease_out(const lv_anim_t *a);
/* For a slide that starts from rest. */
int32_t ui_ease_slide(const lv_anim_t *a);
