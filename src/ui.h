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
lv_obj_t *ui_heading(lv_obj_t *parent, const char *text);
lv_obj_t *ui_row(lv_obj_t *parent, const char *icon, const char *label, const char *value, unsigned flags,
                 lv_event_cb_t cb, void *ctx);
struct ui_meter ui_meter(lv_obj_t *parent, const char *title);

void ui_set_text(lv_obj_t *label, const char *text);
void ui_set_fmt(lv_obj_t *label, const char *fmt, ...);
void ui_button_text(lv_obj_t *button, const char *text);
void ui_meter_set(struct ui_meter *meter, const char *text, int percent, lv_color_t color);
void ui_focusable(lv_obj_t *obj);
/* Fills a ui_button with the accent color as the main action. */
void ui_primary(lv_obj_t *button);

/* The gap between the blocks of a list or a nested view, tighter when compact. */
int ui_stack_gap(void);

/* Cubic ease-out: it starts at three times the average speed, so an animation that
 * takes over from a finger can be timed to match the finger's speed. */
/* The progress at time t, 0 to 1, of CSS cubic-bezier(x1, y1, x2, y2). */
float ui_bezier(float t, float x1, float y1, float x2, float y2);
int32_t ui_ease_out(const lv_anim_t *a);
/* Gathers speed over the first frames, then settles: for a slide that starts from rest. */
int32_t ui_ease_slide(const lv_anim_t *a);
