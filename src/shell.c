#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "src/misc/cache/instance/lv_image_cache.h"

#include "api.h"
#include "drm.h"
#include "app.h"
#include "font.h"
#include "icons.h"
#include "input.h"
#include "keyboard.h"
#include "theme.h"
#include "ui.h"
#include "util.h"

#define POLL_AWAKE_MS 2000
#define POLL_BLANK_MS 5000
#define STATUS_H 28
#define TOOLBAR_H 44
#define NAV_H 52

/* Kernel key repeat outruns the page slide and the focus walk. */
#define NAV_STEP_MS 260
#define NAV_FOCUS_MS 150

struct app app = {.brightness = 120};

static const struct page_ops *const page_ops[PAGE_COUNT] = {
    &overview_page, &signal_page, &traffic_page, &sms_page, &hotspot_page,
    &device_page, &details_page, &settings_page, &apps_page,
};
static const int nav_defaults[5] = {PAGE_OVERVIEW, PAGE_SIGNAL, PAGE_TRAFFIC, PAGE_SMS, PAGE_SETTINGS};

static struct {
    lv_obj_t *root, *wallpaper, *content, *menu;
    lv_obj_t *bar, *bar_sim, *bar_op, *bar_tech, *bar_notes, *bar_sms, *bar_bars, *bar_dual[2], *bar_bat, *bar_pct, *bar_clock, *bar_rec;
    lv_obj_t *toolbar, *back_icon, *title, *spinner;
    lv_obj_t *nav, *nav_pill, *nav_buttons[5];
    lv_obj_t *pages[PAGE_COUNT], *errors[PAGE_COUNT];
    lv_obj_t *toast, *osd, *osd_label, *osd_bar, *osd_value;
    lv_obj_t *notice, *notice_title, *notice_body;
    lv_obj_t *picture;
    lv_group_t *group, *notice_group, *picture_group;
} ui;

static int nav_slots[5];
static struct {
    bool dragging;
    int32_t from;
} nav_drag;
/* The direction of a page step, +1 or -1; 0 when a page is opened directly. */
static int step_dir;
/* +1 when the new page comes in from the right, -1 from the left. */
static int slide_dir = 1;
/* While the page has no slot in the bar the pill waits past an edge: +1 right,
 * -1 left, 0 when it sits under a slot. */
static int pill_side;
static int loading[PAGE_COUNT];
static bool load_failed[PAGE_COUNT];
static lv_timer_t *poll_timer, *idle_timer, *clock_timer, *toast_timer, *osd_timer, *spinner_timer;
static bool poll_busy, poll_again, first_status = true;
static uint32_t blank_changed_at;
static bool supply_known, supply_online;

static struct {
    int level, pending;
    bool busy;
} backlight = {.pending = -1};

static struct {
    lv_timer_t *timer;
    bool fired;
    uint32_t last_press;
} power_key;
static uint32_t last_back;
static uint32_t last_step, last_focus;
static bool volume_busy;
static int volume_held;
static bool volume_repeating;
#define SCREENSHOT_PATH "/tmp/screen.png"
static lv_timer_t *volume_timer, *chord_timer;
/* Both volume keys are down and the hold has not yet become a recording prompt. */
static bool chord;

static struct {
    char key[96];
    cJSON *list;
    char dismissed[16][96];
    int dismissed_count;
    char seen[16][96];
    int seen_count;
} notes;

const cJSON *screen_config(void)
{
    return jget(app.status, "screen");
}

lv_obj_t *shell_root(void)
{
    return ui.root;
}

lv_obj_t *shell_content(void)
{
    return ui.content;
}

int shell_status_bar_height(void)
{
    return theme.status_bar ? STATUS_H : 0;
}

/* ---------- small drawn indicators ---------- */

static void bars_draw(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);
    int on = (int)(intptr_t)lv_obj_get_user_data(obj);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    int32_t w = lv_area_get_width(&a), h = lv_area_get_height(&a), bar = (w - 3) / 4;
    static const int heights[] = {30, 50, 75, 100};
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = lv_obj_get_style_text_color(obj, 0);
    dsc.radius = 1;
    for (int i = 0; i < 4; i++) {
        int32_t bh = h * heights[i] / 100;
        lv_area_t r = {a.x1 + i * (bar + 1), a.y2 - bh + 1, a.x1 + i * (bar + 1) + bar - 1, a.y2};
        dsc.bg_opa = i < on ? LV_OPA_COVER : LV_OPA_20;
        lv_draw_rect(lv_event_get_layer(e), &dsc, &r);
    }
}

static lv_obj_t *bars_create(lv_obj_t *parent, int w, int h)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_clickable(obj, false);
    lv_obj_set_style_text_color(obj, theme.text, 0);
    lv_obj_add_event_cb(obj, bars_draw, LV_EVENT_DRAW_MAIN, NULL);
    return obj;
}

static void bars_set(lv_obj_t *obj, int on)
{
    if ((int)(intptr_t)lv_obj_get_user_data(obj) == on)
        return;
    lv_obj_set_user_data(obj, (void *)(intptr_t)on);
    lv_obj_invalidate(obj);
}

#define BOLT_W 11
#define BOLT_H 15

static float edge_distance(float px, float py, lv_point_precise_t a, lv_point_precise_t b)
{
    float dx = b.x - a.x, dy = b.y - a.y;
    float t = fminf(1, fmaxf(0, ((px - a.x) * dx + (py - a.y) * dy) / (dx * dx + dy * dy)));
    return hypotf(px - (a.x + t * dx), py - (a.y + t * dy));
}

/* The bolt as anti-aliased coverage: its outline grown by the ring, or its core. */
static lv_draw_buf_t *bolt_mask(bool ring)
{
    static lv_draw_buf_t *masks[2];
    if (masks[ring])
        return masks[ring];
    /* "M7 1 2 9h4l-1 6 7-9H7z" from a 12 × 16 box at 9 × 13, its outline centred in the mask. */
    static const float path[][2] = {{7, 1}, {2, 9}, {6, 9}, {5, 15}, {12, 6}, {7, 6}};
    enum { N = sizeof path / sizeof *path };
    lv_point_precise_t p[N];
    float x0 = INFINITY, x1 = -INFINITY, y0 = INFINITY, y1 = -INFINITY;
    for (int i = 0; i < N; i++) {
        p[i] = (lv_point_precise_t){path[i][0] * 9 / 12, path[i][1] * 13 / 16};
        x0 = fminf(x0, p[i].x), x1 = fmaxf(x1, p[i].x), y0 = fminf(y0, p[i].y), y1 = fmaxf(y1, p[i].y);
    }
    for (int i = 0; i < N; i++)
        p[i].x += (BOLT_W - x0 - x1) / 2, p[i].y += (BOLT_H - y0 - y1) / 2;
    lv_draw_buf_t *buf = lv_draw_buf_create(BOLT_W, BOLT_H, LV_COLOR_FORMAT_A8, 0);
    for (int y = 0; y < BOLT_H; y++) {
        uint8_t *row = lv_draw_buf_goto_xy(buf, 0, y);
        for (int x = 0; x < BOLT_W; x++) {
            float px = x + 0.5f, py = y + 0.5f, d = INFINITY;
            bool inside = false;
            for (int i = 0, j = N - 1; i < N; j = i++) {
                d = fminf(d, edge_distance(px, py, p[j], p[i]));
                if ((p[i].y > py) != (p[j].y > py) && px < p[j].x + (py - p[j].y) * (p[i].x - p[j].x) / (p[i].y - p[j].y))
                    inside = !inside;
            }
            /* Signed distance into the shape; the ring is a pixel outside the outline. */
            float depth = (inside ? d : -d) + (ring ? 1.0f : 0.0f);
            row[x] = (uint8_t)(fminf(1, fmaxf(0, depth + 0.5f)) * 255 + 0.5f);
        }
    }
    return masks[ring] = buf;
}

static void battery_draw(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);
    int level = (int)(intptr_t)lv_obj_get_user_data(obj);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_opa = LV_OPA_TRANSP;
    dsc.border_color = theme.dim;
    dsc.border_width = 1;
    dsc.radius = 2;
    lv_area_t body = {a.x1, a.y1 + 2, a.x2 - 2, a.y2 - 2};
    lv_draw_rect(lv_event_get_layer(e), &dsc, &body);
    lv_area_t tip = {a.x2 - 1, body.y1 + 3, a.x2, body.y2 - 3};
    dsc.border_width = 0;
    dsc.bg_opa = LV_OPA_COVER;
    dsc.bg_color = theme.dim;
    lv_draw_rect(lv_event_get_layer(e), &dsc, &tip);
    int32_t inner = lv_area_get_width(&body) - 4;
    lv_area_t fill = {body.x1 + 2, body.y1 + 2, body.x1 + 2 + inner * (level & 0xff) / 100 - 1, body.y2 - 2};
    dsc.bg_color = (level & 0x100) ? theme.accent : (level & 0x200) ? theme.poor : theme.good;
    dsc.radius = 1;
    if (fill.x2 >= fill.x1)
        lv_draw_rect(lv_event_get_layer(e), &dsc, &fill);
    if (level & 0x400) {
        /* The web page's bolt, 9 × 13 over the battery, outlined in the bar colour. */
        lv_draw_image_dsc_t img;
        lv_draw_image_dsc_init(&img);
        img.recolor_opa = LV_OPA_COVER;
        int32_t cx = (body.x1 + body.x2 + 1) / 2, cy = (a.y1 + a.y2 + 1) / 2;
        lv_area_t at = {cx - BOLT_W / 2, cy - BOLT_H / 2, cx - BOLT_W / 2 + BOLT_W - 1, cy - BOLT_H / 2 + BOLT_H - 1};
        img.src = bolt_mask(true);
        img.recolor = theme.desktop_wallpaper ? theme.bg : theme.chrome;
        lv_draw_image(lv_event_get_layer(e), &img, &at);
        img.src = bolt_mask(false);
        img.recolor = theme.text;
        lv_draw_image(lv_event_get_layer(e), &img, &at);
    }
}

/* ---------- toasts, volume ---------- */

static void toast_hide(lv_timer_t *t)
{
    lv_obj_set_hidden(ui.toast, true);
}

void toast(const char *fmt, ...)
{
    char text[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof text, fmt, args);
    va_end(args);
    ui_set_text(ui.toast, text);
    lv_obj_set_hidden(ui.toast, false);
    lv_obj_move_foreground(ui.toast);
    lv_timer_reset(toast_timer);
    lv_timer_resume(toast_timer);
}

static void osd_hide(lv_timer_t *t)
{
    lv_obj_set_hidden(ui.osd, true);
}

static void volume_done(int status, cJSON *r, void *ctx)
{
    volume_busy = false;
    if (!jtrue(r, "available") || app.blank)
        return;
    idle_reset();
    const char *kind = jstr(r, "kind");
    int level = (int)jnum_or(r, "level", 0), max = (int)jnum_or(r, "max", 15);
    ui_set_text(ui.osd_label, kind && !strcmp(kind, "call") ? L("通话音量", "Call volume") : L("音量", "Volume"));
    if (level > 0)
        ui_set_fmt(ui.osd_value, "%d", level);
    else
        ui_set_text(ui.osd_value, L("静音", "Muted"));
    lv_bar_set_value(ui.osd_bar, max > 0 ? level * 100 / max : 0, LV_ANIM_OFF);
    lv_obj_set_hidden(ui.osd, false);
    lv_timer_reset(osd_timer);
    lv_timer_resume(osd_timer);
}

static void volume_step(int step);

/* A held volume key keeps stepping, without queueing requests behind a slow one.
 * The first step waits briefly so that pressing both keys together only takes a screenshot. */
static void volume_repeat(lv_timer_t *t)
{
    lv_timer_set_period(t, volume_repeating ? 150 : 400);
    volume_repeating = true;
    if (volume_held)
        volume_step(volume_held);
}

static void screenshot(void)
{
    lv_timer_pause(osd_timer);
    lv_obj_set_hidden(ui.osd, true);
    lv_obj_set_hidden(ui.toast, true);
    lv_refr_now(NULL);
    bool saved = drm_display_save(SCREENSHOT_PATH);
    toast(saved ? L("截图已保存到 " SCREENSHOT_PATH, "Screenshot saved to " SCREENSHOT_PATH)
                : L("截图失败", "Screenshot failed"));
}

static void chord_hold(lv_timer_t *t)
{
    lv_timer_pause(t);
    chord = false;
    record_prompt();
}

void shell_record_indicator(int seconds)
{
    lv_obj_set_hidden(ui.bar_rec, seconds < 0);
    if (seconds >= 0)
        ui_set_fmt(ui.bar_rec, "● %d:%02d", seconds / 60, seconds % 60);
}

static void volume_step(int step)
{
    if (volume_busy)
        return;
    volume_busy = true;
    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "step", step);
    api_post("/volume", body, volume_done, NULL);
}

/* ---------- backlight, idle ---------- */

static void backlight_done(int status, cJSON *r, void *ctx);

static void backlight_send(int level)
{
    if (backlight.busy) {
        backlight.pending = level;
        return;
    }
    backlight.busy = true;
    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "level", level);
    api_post("/backlight", body, backlight_done, NULL);
}

static void backlight_done(int status, cJSON *r, void *ctx)
{
    backlight.busy = false;
    if (!api_reply_ok(r))
        toast("%s", L("屏幕状态同步失败，请按电源键重试", "Press power to retry"));
    if (backlight.pending >= 0) {
        int level = backlight.pending;
        backlight.pending = -1;
        backlight_send(level);
    }
}

static void idle_fire(lv_timer_t *t)
{
    lv_timer_pause(idle_timer);
    screen_set_blank(true);
}

void idle_reset(void)
{
    lv_timer_pause(idle_timer);
    int idle = (int)jnum_or(screen_config(), "idle", 60);
    if (app.blank || idle <= 0 || power_menu_is_open() || phone_keep_awake())
        return;
    lv_timer_set_period(idle_timer, (uint32_t)idle * 1000);
    lv_timer_reset(idle_timer);
    lv_timer_resume(idle_timer);
}

static void clock_tick(lv_timer_t *t);
static void render_status(void);

void screen_set_blank(bool blank)
{
    if (blank == app.blank || (!blank && app.keylock))
        return;
    app.blank = blank;
    blank_changed_at = lv_tick_get();
    lock_power(blank);
    backlight_send(blank ? 0 : app.brightness);
    clock_tick(NULL);
    if (!blank) {
        render_status();
        shell_poll_now();
        phone_screen_wake();
    }
    idle_reset();
}

bool shell_touch_allowed(void)
{
    return !app.blank && !app.keylock && !jfalse(screen_config(), "touch") && !lock_touch_blocked();
}

/* ---------- pages ---------- */

static int page_order(int out[PAGE_COUNT])
{
    int n = 0;
    bool used[PAGE_COUNT] = {0};
    for (int i = 0; i < 5; i++)
        if (!used[nav_slots[i]]) {
            used[nav_slots[i]] = true;
            out[n++] = nav_slots[i];
        }
    for (int i = 0; i < PAGE_COUNT; i++)
        if (!used[i])
            out[n++] = i;
    return n;
}

static int page_position(int page)
{
    int order[PAGE_COUNT];
    page_order(order);
    for (int i = 0; i < PAGE_COUNT; i++)
        if (order[i] == page)
            return i;
    return 0;
}

static const char *page_label(int page)
{
    return L(page_ops[page]->zh, page_ops[page]->en);
}

/* ---------- bottom navigation ---------- */

static void navigate(int page);

static void nav_highlight(int slot)
{
    for (int i = 0; i < 5; i++)
        lv_obj_set_style_text_color(ui.nav_buttons[i], i == slot ? theme.accent : theme.dim, 0);
}

static void pill_x(void *obj, int32_t x)
{
    lv_obj_set_x(obj, x);
}

static void pill_glide(int32_t x, bool animate, lv_anim_path_cb_t path)
{
    lv_anim_delete(ui.nav_pill, pill_x);
    if (!animate) {
        lv_obj_set_x(ui.nav_pill, x);
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, ui.nav_pill);
    lv_anim_set_exec_cb(&a, pill_x);
    lv_anim_set_values(&a, lv_obj_get_style_x(ui.nav_pill, 0), x);
    lv_anim_set_duration(&a, 320);
    lv_anim_set_path_cb(&a, path);
    lv_anim_start(&a);
}

/* Clear of the bar's padding on the side the pages move towards. */
static int32_t pill_off(int dir)
{
    return dir < 0 ? -lv_obj_get_style_width(ui.nav_pill, 0) - lv_obj_get_style_pad_left(ui.nav, 0)
                   : lv_obj_get_width(ui.nav);
}

/* Glides the glass pill under a slot, settling with a slight overshoot. A step
 * brings a waiting pill in moving the way of the step; opening a page directly
 * brings it from the side where it waits. */
static void pill_to(int slot, bool animate)
{
    lv_obj_t *b = ui.nav_buttons[slot];
    lv_obj_set_size(ui.nav_pill, lv_obj_get_width(b), lv_obj_get_height(b));
    lv_obj_set_y(ui.nav_pill, lv_obj_get_y(b));
    if (pill_side) {
        lv_anim_delete(ui.nav_pill, pill_x);
        lv_obj_set_x(ui.nav_pill, pill_off(step_dir ? -step_dir : pill_side));
        pill_side = 0;
    }
    pill_glide(lv_obj_get_x(b), animate, lv_anim_path_overshoot);
}

static int32_t slot_center(int slot)
{
    return lv_obj_get_x(ui.nav_buttons[slot]) + lv_obj_get_width(ui.nav_buttons[slot]) / 2;
}

static int nav_nearest(int32_t x)
{
    int best = 0;
    for (int i = 1; i < 5; i++)
        if (LV_ABS(slot_center(i) - x) < LV_ABS(slot_center(best) - x))
            best = i;
    return best;
}

static int slot_of(int page)
{
    for (int i = 0; i < 5; i++)
        if (nav_slots[i] == page)
            return i;
    return -1;
}

static void nav_sync(void)
{
    if (nav_drag.dragging)
        return;
    int active = slot_of(app.page);
    nav_highlight(active);
    lv_obj_update_layout(ui.nav);
    bool shown = !lv_obj_is_hidden(ui.nav_pill);
    if (active < 0) {
        /* A step carries it on past the end of the bar; opening a page directly
         * takes it out with the old page. */
        if (shown && !pill_side) {
            pill_side = step_dir ? step_dir : -slide_dir;
            pill_glide(pill_off(pill_side), theme.motion, lv_anim_path_ease_in);
        }
        return;
    }
    lv_obj_set_hidden(ui.nav_pill, false);
    pill_to(active, theme.motion && shown);
}

/* A tap picks a slot; a horizontal drag carries the pill, lighting the slot
 * under it, and the page changes to the nearest slot when the finger lifts. */
static void on_nav(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    lv_area_t area;
    lv_obj_get_content_coords(ui.nav, &area);
    int32_t x = LV_CLAMP(slot_center(0), p.x - area.x1, slot_center(4));
    if (code == LV_EVENT_PRESSED) {
        nav_drag.from = x;
    } else if (code == LV_EVENT_PRESSING) {
        if (!nav_drag.dragging) {
            if (LV_ABS(x - nav_drag.from) < S(8))
                return;
            nav_drag.dragging = true;
            pill_side = 0;
            lv_anim_delete(ui.nav_pill, pill_x);
            lv_obj_set_hidden(ui.nav_pill, false);
            lv_obj_t *b = ui.nav_buttons[0];
            /* The pill swells while it is held. */
            lv_obj_set_size(ui.nav_pill, lv_obj_get_width(b) + S(10), lv_obj_get_height(b) + S(4));
            lv_obj_set_y(ui.nav_pill, lv_obj_get_y(b) - S(2));
        }
        lv_obj_set_x(ui.nav_pill, x - (lv_obj_get_width(ui.nav_buttons[0]) + S(10)) / 2);
        nav_highlight(nav_nearest(x));
    } else if (code == LV_EVENT_RELEASED) {
        nav_drag.dragging = false;
        int page = nav_slots[nav_nearest(x)];
        /* Showing the page moves the pill, after the slide direction is known. */
        navigate(page);
        /* navigate() declines while locked or in a call; put the pill back. */
        if ((int)app.page != page)
            nav_sync();
    } else if (code == LV_EVENT_PRESS_LOST) {
        nav_drag.dragging = false;
        nav_sync();
    }
}

void shell_title(void)
{
    ui_set_text(ui.title, phone_is_open() ? L("电话", "Phone") : page_label(app.page));
    icon_set(ui.back_icon, app.page == PAGE_OVERVIEW && !phone_is_open() ? "home" : "back");
    nav_sync();
}

static void spinner_show(lv_timer_t *t)
{
    lv_timer_pause(spinner_timer);
    lv_obj_set_hidden(ui.spinner, !(loading[app.page] > 0 && !load_failed[app.page]));
}

static void paint_load(void)
{
    if (loading[app.page] > 0 && !load_failed[app.page]) {
        if (lv_obj_is_hidden(ui.spinner)) {
            lv_timer_reset(spinner_timer);
            lv_timer_resume(spinner_timer);
        }
    } else {
        lv_timer_pause(spinner_timer);
        lv_obj_set_hidden(ui.spinner, true);
    }
    for (int i = 0; i < PAGE_COUNT; i++)
        if (ui.errors[i])
            lv_obj_set_hidden(ui.errors[i], !load_failed[i]);
}

void shell_page_loading(enum page page, bool on)
{
    loading[page] += on ? 1 : -1;
    if (loading[page] < 0)
        loading[page] = 0;
    if (on)
        load_failed[page] = false;
    paint_load();
}

void shell_page_error(enum page page, bool failed)
{
    load_failed[page] = failed;
    paint_load();
}

static void slide_x(void *obj, int32_t x)
{
    lv_obj_set_style_translate_x(obj, x, 0);
}

static void slide_done(lv_anim_t *a)
{
    lv_obj_t *obj = a->var;
    lv_obj_set_style_translate_x(obj, 0, 0);
    if (obj != ui.pages[app.page])
        lv_obj_set_hidden(obj, true);
}

static void slide(lv_obj_t *obj, int32_t from, int32_t to)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, slide_x);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, 280);
    lv_anim_set_path_cb(&a, ui_ease_out);
    lv_anim_set_completed_cb(&a, slide_done);
    lv_anim_start(&a);
}

void shell_show(enum page page)
{
    enum page previous = app.page;
    app.page = page;
    if (previous != page) {
        lv_obj_t *old = ui.pages[previous], *now = ui.pages[page];
        if (page_ops[previous]->hidden)
            page_ops[previous]->hidden();
        lv_anim_delete(old, slide_x);
        lv_anim_delete(now, slide_x);
        lv_obj_set_hidden(now, false);
        lv_obj_scroll_to_y(now, 0, LV_ANIM_OFF);
        /* Opening a slotted page from outside the bar slides the pages the way the
         * waiting pill comes in, so both move together. */
        if (step_dir)
            slide_dir = step_dir;
        else if (pill_side && slot_of(page) >= 0)
            slide_dir = pill_side;
        else
            slide_dir = page_position(page) > page_position(previous) ? 1 : -1;
        if (theme.motion) {
            int32_t w = lv_obj_get_width(ui.content);
            slide(old, 0, -slide_dir * w);
            slide(now, slide_dir * w, 0);
        } else {
            lv_obj_set_hidden(old, true);
        }
    }
    input_defocus();
    shell_title();
    paint_load();
    if (app.status && page_ops[page]->status)
        page_ops[page]->status(app.status);
    if (page_ops[page]->shown)
        page_ops[page]->shown();
}

static void menu_show(bool on);

static void navigate(int page)
{
    if (lock_active() || phone_call_active())
        return;
    if (page == (int)app.page && page_ops[page]->top)
        page_ops[page]->top();
    if (phone_is_open())
        phone_close();
    menu_show(false);
    shell_show((enum page)page);
    idle_reset();
}

void shell_step(int delta)
{
    int order[PAGE_COUNT];
    int n = page_order(order), at = page_position(app.page);
    step_dir = delta > 0 ? 1 : -1;
    navigate(order[(at + delta + n) % n]);
    step_dir = 0;
}

static void back(void)
{
    if (phone_is_open()) {
        phone_close();
        return;
    }
    if (!lv_obj_is_hidden(ui.menu)) {
        menu_show(false);
        return;
    }
    if (page_ops[app.page]->back && page_ops[app.page]->back())
        return;
    navigate(PAGE_OVERVIEW);
}

/* ---------- status ---------- */

static int signal_bars(const cJSON *sim)
{
    double rsrp;
    if (!sim || jtrue(sim, "stale") || (jget(sim, "error") && !cJSON_IsNull(jget(sim, "error"))) ||
        !jnum(sim, "signal.rsrp", &rsrp))
        return 0;
    return rsrp >= -90 ? 4 : rsrp >= -100 ? 3 : rsrp >= -110 ? 2 : 1;
}

static const cJSON *sim_slot(const cJSON *st, int slot)
{
    const cJSON *sim;
    cJSON_ArrayForEach(sim, jget(st, "sims"))
        if ((int)jnum_or(sim, "slot", -1) == slot)
            return sim;
    return NULL;
}

static void render_bar(const cJSON *st)
{
    const cJSON *m = jget(st, "modem");
    double card;
    if (jnum(m, "sim_card", &card))
        ui_set_fmt(ui.bar_sim, "%d", (int)card + 1);
    else
        ui_set_text(ui.bar_sim, "");
    const char *op = operator_name(jstr(m, "operator"));
    ui_set_text(ui.bar_op, op ? op
                              : !jtrue(m, "present") ? L("无模组", "No modem")
                              : !jtrue(m, "sim")     ? L("无 SIM 卡", "No SIM")
                                                     : L("搜索网络", "Searching"));
    ui_set_text(ui.bar_tech, jstr(m, "tech") ? tech_name(jstr(m, "tech")) : "--");
    lv_obj_set_style_text_opa(ui.bar_tech, jtrue(st, "wan.up") ? LV_OPA_COVER : LV_OPA_40, 0);

    int inserted = 0;
    const cJSON *sim, *single = NULL;
    cJSON_ArrayForEach(sim, jget(st, "sims"))
        if (jtrue(sim, "present")) {
            inserted++;
            single = sim;
        }
    bool dual = inserted >= 2;
    lv_obj_set_hidden(ui.bar_bars, dual);
    lv_obj_set_hidden(lv_obj_get_parent(ui.bar_dual[0]), !dual);
    if (dual) {
        int primary = (int)jnum_or(m, "sim_card", 0);
        for (int slot = 0; slot < 2; slot++) {
            bars_set(ui.bar_dual[slot], signal_bars(sim_slot(st, slot)));
            lv_obj_set_height(ui.bar_dual[slot], slot == primary ? 10 : 4);
            lv_obj_move_to_index(ui.bar_dual[slot], slot == primary ? 0 : 1);
        }
    } else {
        double q = 0, rsrp;
        if (inserted == 1)
            q = jtrue(single, "stale") || !jnum(single, "signal.rsrp", &rsrp) ? 0 : fmin(100, fmax(0, (rsrp + 120) * 2.5));
        else
            q = jnum_or(m, "quality", 0);
        bars_set(ui.bar_bars, q >= 75 ? 4 : q >= 50 ? 3 : q >= 25 ? 2 : q > 0 ? 1 : 0);
    }

    const cJSON *b = jget(st, "battery");
    double cap;
    bool has_cap = jnum(b, "capacity", &cap);
    int level = has_cap ? (int)fmax(0, fmin(100, cap)) : 0;
    double current = jnum_or(b, "current_ma", 0);
    bool online = jtrue(b, "online");
    const char *status = jstr(b, "status");
    bool charging = online && level < 100 && !(status && !strcmp(status, "Full")) &&
                    ((status && !strcmp(status, "Charging")) || current > 20);
    int flags = level | (charging ? 0x100 : 0) | (online ? 0x400 : 0) | (level <= 15 && !charging ? 0x200 : 0);
    if ((int)(intptr_t)lv_obj_get_user_data(ui.bar_bat) != flags) {
        lv_obj_set_user_data(ui.bar_bat, (void *)(intptr_t)flags);
        lv_obj_invalidate(ui.bar_bat);
    }
    if (has_cap)
        ui_set_fmt(ui.bar_pct, "%d%%", (int)cap);
    else
        ui_set_text(ui.bar_pct, "--");

    lv_obj_set_hidden(ui.bar_sms, cJSON_GetArraySize(jget(st, "sms.unread")) == 0);
    int n = cJSON_GetArraySize(notes.list);
    if (n)
        ui_set_fmt(ui.bar_notes, "• %d", n);
    else
        ui_set_text(ui.bar_notes, "");
}

static void render_status(void)
{
    if (!app.status)
        return;
    render_bar(app.status);
    lock_status(app.status);
    if (app.blank || lock_active() || phone_is_open())
        return;
    if (page_ops[app.page]->status)
        page_ops[app.page]->status(app.status);
}

static void clock_tick(lv_timer_t *t)
{
    if (app.blank) {
        lv_timer_pause(clock_timer);
        return;
    }
    struct local_time now = local_now();
    bool seconds = jtrue(screen_config(), "clock_seconds");
    if (seconds)
        ui_set_fmt(ui.bar_clock, "%02d:%02d:%02d", now.hour, now.minute, now.second);
    else
        ui_set_fmt(ui.bar_clock, "%02d:%02d", now.hour, now.minute);
    lock_clock();
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    long ms = (long)(ts.tv_sec + (time_t)jnum_or(app.status, "tz_offset", 0)) * 1000 + ts.tv_nsec / 1000000;
    long period = seconds ? 1000 : 60000;
    lv_timer_set_period(clock_timer, (uint32_t)(period - ms % period));
    lv_timer_reset(clock_timer);
    lv_timer_resume(clock_timer);
}

/* ---------- plugin notifications ---------- */

static bool listed(char list[][96], int count, const char *key)
{
    for (int i = 0; i < count; i++)
        if (!strcmp(list[i], key))
            return true;
    return false;
}

static void note_key(const cJSON *n, char *out)
{
    const char *plugin = jstr(n, "plugin") ? jstr(n, "plugin") : "";
    const cJSON *id = jget(n, "id");
    if (cJSON_IsNumber(id))
        snprintf(out, 96, "%s/%g", plugin, id->valuedouble);
    else
        snprintf(out, 96, "%s/%s", plugin, cJSON_IsString(id) ? id->valuestring : "");
}

static const cJSON *notice_current(void)
{
    const cJSON *n;
    char key[96];
    cJSON_ArrayForEach(n, notes.list) {
        note_key(n, key);
        if (!listed(notes.dismissed, notes.dismissed_count, key))
            return n;
    }
    return NULL;
}

static void notice_render(void)
{
    const cJSON *n = notice_current();
    bool hidden = !n || app.blank || app.keylock || lock_active() || phone_is_open() ||
                  !lv_obj_is_hidden(ui.picture);
    if (hidden != lv_obj_is_hidden(ui.notice)) {
        lv_obj_set_hidden(ui.notice, hidden);
        if (!hidden)
            input_set_group(ui.notice_group);
        else if (input_group() == ui.notice_group)
            input_set_group(ui.group);
    }
    if (hidden)
        return;
    note_key(n, notes.key);
    const char *title = jlabel(jget(n, "title"));
    ui_set_text(ui.notice_title, *title ? title : L("应用通知", "App notification"));
    ui_set_text(ui.notice_body, jstr(n, "body") ? jstr(n, "body") : "");
}

static void notice_dismiss(lv_event_t *e)
{
    if (notes.dismissed_count < 16)
        snprintf(notes.dismissed[notes.dismissed_count++], 96, "%s", notes.key);
    notice_render();
}

static void notice_open(lv_event_t *e)
{
    const cJSON *n = notice_current();
    const char *plugin = jstr(n, "plugin");
    bool phone = plugin && !strcmp(plugin, "phone");
    notice_dismiss(e);
    if (phone)
        phone_open();
}

static void notifications(const cJSON *list)
{
    cJSON *filtered = cJSON_CreateArray();
    const cJSON *n;
    cJSON_ArrayForEach(n, list) {
        const char *id = jstr(n, "id");
        if (jstr(n, "plugin") && !strcmp(jstr(n, "plugin"), "phone") && id && !strncmp(id, "call-", 5))
            continue;
        cJSON_AddItemToArray(filtered, cJSON_Duplicate(n, true));
    }
    cJSON_Delete(notes.list);
    notes.list = filtered;

    char seen[16][96];
    int seen_count = 0, kept = 0;
    bool fresh_wake = false;
    cJSON_ArrayForEach(n, notes.list) {
        if (seen_count == 16)
            break;
        note_key(n, seen[seen_count]);
        if (!listed(notes.seen, notes.seen_count, seen[seen_count]) && jtrue(n, "wake"))
            fresh_wake = true;
        seen_count++;
    }
    for (int i = 0; i < notes.dismissed_count; i++)
        if (listed(seen, seen_count, notes.dismissed[i]))
            memmove(notes.dismissed[kept++], notes.dismissed[i], 96);
    notes.dismissed_count = kept;
    memcpy(notes.seen, seen, sizeof seen);
    notes.seen_count = seen_count;
    if (fresh_wake && app.blank && !app.keylock)
        screen_set_blank(false);
    notice_render();
}

/* ---------- polling ---------- */

static void reconcile_power(const cJSON *st)
{
    const cJSON *awake = jget(st, "screen.awake");
    if (cJSON_IsBool(awake) && cJSON_IsTrue(awake) == app.blank && lv_tick_elaps(blank_changed_at) > 1000) {
        app.blank = !cJSON_IsTrue(awake);
        lock_power(app.blank);
        clock_tick(NULL);
        idle_reset();
    }
    const cJSON *online = jget(st, "battery.online");
    if (cJSON_IsBool(online)) {
        bool changed = supply_known && supply_online != cJSON_IsTrue(online);
        supply_known = true;
        supply_online = cJSON_IsTrue(online);
        if (changed) {
            if (app.blank)
                screen_set_blank(false);
            else
                idle_reset();
        }
    }
}

/* Language, theme and navigation slots decide how the shell is built. */
static bool apply_screen(void)
{
    const cJSON *screen = screen_config();
    const char *lang = jstr(screen, "lang");
    bool en = lang && !strcmp(lang, "en");
    bool rebuild = en != app.en;
    app.en = en;
    rebuild |= theme_apply(screen);
    for (int i = 0; i < 5; i++) {
        char name[16];
        snprintf(name, sizeof name, "nav_slot%d", i + 1);
        const char *v = jstr(screen, name);
        int slot = v && v[0] >= '0' && v[0] <= '8' && !v[1] ? v[0] - '0' : nav_defaults[i];
        rebuild |= slot != nav_slots[i];
        nav_slots[i] = slot;
    }
    return rebuild;
}

void shell_screen_changed(void)
{
    if (apply_screen())
        shell_rebuild();
    else
        render_status();
    clock_tick(NULL);
    idle_reset();
}

static void status_done(int status, cJSON *st, void *ctx)
{
    poll_busy = false;
    lv_timer_set_period(poll_timer, poll_again ? 1 : app.blank ? POLL_BLANK_MS : POLL_AWAKE_MS);
    lv_timer_reset(poll_timer);
    poll_again = false;
    if (status != 200 || !cJSON_IsObject(jget(st, "screen")))
        return;
    cJSON_Delete(app.status);
    app.status = cJSON_Duplicate(st, true);
    const cJSON *screen = screen_config();
    bool first = first_status;
    if (first) {
        first_status = false;
        app.brightness = (int)jnum_or(screen, "brightness", 120);
        if (app.brightness <= 0)
            app.brightness = 120;
        backlight_send(app.blank ? 0 : app.brightness);
    }
    if (first)
        clock_tick(NULL);
    if (apply_screen())
        shell_rebuild();
    reconcile_power(app.status);
    render_status();
    sms_check(app.status);
    notifications(jget(app.status, "notifications"));
}

static void poll_fire(lv_timer_t *t)
{
    if (poll_busy) {
        poll_again = true;
        return;
    }
    poll_busy = true;
    lv_timer_set_period(poll_timer, 60000);
    api_get("/status", status_done, NULL);
}

void shell_poll_now(void)
{
    if (poll_busy) {
        poll_again = true;
        return;
    }
    lv_timer_ready(poll_timer);
}

/* ---------- picture ---------- */

static void picture_close(lv_event_t *e)
{
    lv_obj_set_hidden(ui.picture, true);
    lv_obj_clean(ui.picture);
    input_set_group(ui.group);
    notice_render();
}

void picture_open(const char *src, const char *caption)
{
    char path[256];
    snprintf(path, sizeof path, "A:/usr/share/e5-infoscreen/www%s%s", src[0] == '/' ? "" : "/", src);
    lv_obj_clean(ui.picture);
    lv_obj_t *image = lv_image_create(ui.picture);
    lv_image_set_src(image, path);
    lv_obj_set_style_radius(image, S(10), 0);
    lv_obj_set_style_clip_corner(image, true, 0);
    int32_t w = lv_display_get_horizontal_resolution(lv_display_get_default()) * 80 / 100;
    lv_image_header_t header;
    if (lv_image_decoder_get_info(path, &header) == LV_RESULT_OK && header.w > 0)
        lv_image_set_scale(image, (uint32_t)(256 * w / header.w));
    lv_image_set_inner_align(image, LV_IMAGE_ALIGN_CENTER);
    lv_obj_t *text = ui_text(ui.picture, 15, false, theme.text);
    lv_obj_set_width(text, LV_PCT(90));
    lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(text, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(text, caption ? caption : "");
    lv_obj_t *hint = ui_text(ui.picture, 12, false, theme.dim);
    lv_label_set_text(hint, L("按任意键或点一下关闭", "Any key or a tap closes this"));
    lv_obj_set_hidden(ui.picture, false);
    lv_obj_move_foreground(ui.picture);
    input_set_group(ui.picture_group);
    notice_render();
}

bool shell_overlay_open(void)
{
    return !lv_obj_is_hidden(ui.picture) || !lv_obj_is_hidden(ui.notice);
}

/* ---------- keys ---------- */

static void power_hold(lv_timer_t *t)
{
    lv_timer_pause(power_key.timer);
    power_key.fired = true;
    power_menu_open();
}

struct focus_scan {
    lv_group_t *group;
    lv_obj_t *current, *best;
    int64_t current_pos, best_pos;
    int dir;
};

static int64_t focus_pos(lv_obj_t *obj)
{
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    return (int64_t)a.y1 * 4096 + a.x1;
}

/* Focus moves in on-screen reading order within the visible page, past what is
 * disabled as LVGL's own group order does. */
static void focus_scan(lv_obj_t *parent, struct focus_scan *scan)
{
    uint32_t count = lv_obj_get_child_count(parent);
    for (uint32_t i = 0; i < count; i++) {
        lv_obj_t *child = lv_obj_get_child(parent, (int32_t)i);
        if (lv_obj_is_hidden(child))
            continue;
        if (lv_obj_get_group(child) == scan->group && child != scan->current &&
            !lv_obj_has_state(child, LV_STATE_DISABLED)) {
            int64_t pos = focus_pos(child);
            bool ahead = scan->current ? (pos - scan->current_pos) * scan->dir > 0 : true;
            if (ahead && (!scan->best || (pos - scan->best_pos) * scan->dir < 0)) {
                scan->best = child;
                scan->best_pos = pos;
            }
        }
        focus_scan(child, scan);
    }
}

/* Arrows walk the focusable objects in reading order. Past the last one the page
 * scrolls, and only a page that cannot scroll further wraps to the other end. */
static void focus_move(int dir)
{
    lv_group_t *group = input_group();
    lv_obj_t *page = ui.pages[app.page];
    struct focus_scan scan = {.group = group, .current = lv_group_get_focused(group), .dir = dir};
    if (scan.current)
        scan.current_pos = focus_pos(scan.current);
    else
        scan.dir = 1;
    focus_scan(page, &scan);
    if (!scan.best) {
        if (dir > 0 ? lv_obj_get_scroll_bottom(page) > 0 : lv_obj_get_scroll_top(page) > 0) {
            lv_obj_scroll_by_bounded(page, 0, -dir * S(80), LV_ANIM_ON);
            return;
        }
        if (!scan.current)
            return;
        struct focus_scan wrap = {.group = group, .dir = dir};
        focus_scan(page, &wrap);
        scan.best = wrap.best;
    }
    if (scan.best) {
        lv_group_focus_obj(scan.best);
        /* LVGL marks the focus state from the group's indev, which it resolves to
         * the touch device whenever one exists, so set it here for the keypad. */
        lv_obj_add_state(scan.best, LV_STATE_FOCUS_KEY);
        lv_obj_scroll_to_view_recursive(scan.best, LV_ANIM_ON);
    }
}

/* The on-screen keyboard belongs to the focused input field. */
static bool field_focused(void)
{
    return lv_obj_check_type(lv_group_get_focused(input_group()), &lv_textarea_class);
}

void shell_key(const struct key_event *key)
{
    if (key->kind == KEY_KIND_VOLUP || key->kind == KEY_KIND_VOLDOWN) {
        int step = key->kind == KEY_KIND_VOLUP ? 1 : -1;
        /* Both keys together: a press takes a screenshot, a hold offers screen
         * recording, and during a recording either one stops it. */
        if (key->down && !key->repeat) {
            if (volume_held == -step) {
                volume_held = 0;
                lv_timer_pause(volume_timer);
                if (record_active()) {
                    record_stop();
                } else {
                    chord = true;
                    lv_timer_reset(chord_timer);
                    lv_timer_resume(chord_timer);
                }
                return;
            }
            volume_held = step;
            volume_repeating = false;
            lv_timer_set_period(volume_timer, 120);
            lv_timer_reset(volume_timer);
            lv_timer_resume(volume_timer);
        } else if (!key->down && chord) {
            chord = false;
            lv_timer_pause(chord_timer);
            screenshot();
        } else if (!key->down && volume_held == step) {
            volume_held = 0;
            lv_timer_pause(volume_timer);
        }
        return;
    }
    if (key->kind == KEY_KIND_POWER) {
        if (key->down && !key->repeat) {
            power_key.last_press = lv_tick_get();
            if (phone_key(key))
                return;
            power_key.fired = false;
            lv_timer_reset(power_key.timer);
            lv_timer_resume(power_key.timer);
        } else if (!key->down) {
            lv_timer_pause(power_key.timer);
            if (power_key.fired || phone_key(key))
                return;
            if (power_menu_is_open())
                power_menu_close();
            else if (!app.keylock)
                screen_set_blank(!app.blank);
        }
        return;
    }
    if (!key->down)
        return;
    if (key->kind == KEY_KIND_CHAR && key->ch == '*' && lv_tick_elaps(power_key.last_press) < 2000 && !key->repeat) {
        power_key.last_press = 0;
        app.keylock = !app.keylock;
        if (app.keylock) {
            screen_set_blank(true);
            cJSON *body = cJSON_CreateObject();
            cJSON_AddNumberToObject(body, "ms", 80);
            api_post("/vibrate", body, NULL, NULL);
        } else {
            screen_set_blank(false);
            toast("%s", L("已解锁", "Unlocked"));
        }
        return;
    }
    if (app.keylock || app.blank)
        return;
    if (key->kind == KEY_KIND_BACK) {
        if (lv_tick_elaps(last_back) < 150)
            return;
        last_back = lv_tick_get();
    }
    idle_reset();
    if (power_menu_is_open()) {
        power_menu_key(key);
        return;
    }
    if (dialog_key(key))
        return;
    if (phone_key(key) || lock_key(key))
        return;
    if (!lv_obj_is_hidden(ui.picture)) {
        picture_close(NULL);
        return;
    }
    if (!lv_obj_is_hidden(ui.notice)) {
        if (key->kind == KEY_KIND_BACK)
            notice_dismiss(NULL);
        else if (key->kind == KEY_KIND_UP || key->kind == KEY_KIND_LEFT)
            input_focus_move(ui.notice_group, -1);
        else if (key->kind == KEY_KIND_DOWN || key->kind == KEY_KIND_RIGHT)
            input_focus_move(ui.notice_group, 1);
        else if (key->kind == KEY_KIND_OK)
            input_focus_key(LV_KEY_ENTER);
        return;
    }
    if (key->kind == KEY_KIND_UP || key->kind == KEY_KIND_DOWN || key->kind == KEY_KIND_LEFT ||
        key->kind == KEY_KIND_RIGHT) {
        bool vertical = key->kind == KEY_KIND_UP || key->kind == KEY_KIND_DOWN;
        uint32_t *last = vertical ? &last_focus : &last_step;
        if (key->repeat && lv_tick_elaps(*last) < (vertical ? NAV_FOCUS_MS : NAV_STEP_MS))
            return;
        *last = lv_tick_get();
    }
    if (page_ops[app.page]->key && page_ops[app.page]->key(key))
        return;
    switch (key->kind) {
    case KEY_KIND_MENU:
        if (!key->repeat)
            navigate(PAGE_OVERVIEW);
        break;
    case KEY_KIND_CALL:
        if (!key->repeat) {
            menu_show(false);
            phone_open();
        }
        break;
    case KEY_KIND_LEFT:
    case KEY_KIND_RIGHT:
        shell_step(key->kind == KEY_KIND_LEFT ? -1 : 1);
        break;
    case KEY_KIND_UP:
    case KEY_KIND_DOWN:
        focus_move(key->kind == KEY_KIND_DOWN ? 1 : -1);
        break;
    case KEY_KIND_OK:
        if (!key->repeat && lv_group_get_focused(input_group()))
            input_focus_key(LV_KEY_ENTER);
        break;
    case KEY_KIND_BACK:
        /* A held Back steps back once. */
        if (key->repeat)
            break;
        if (field_focused())
            input_defocus();
        else
            back();
        break;
    case KEY_KIND_HOTSPOT:
        navigate(PAGE_HOTSPOT);
        break;
    case KEY_KIND_CHAR:
        if (key->ch >= '1' && key->ch <= '9')
            navigate(key->ch - '1');
        break;
    default:
        break;
    }
}

/* A tap ends the key focus, except in a field, where it is a cursor move. */
void shell_touch_press(void)
{
    if (!field_focused())
        input_defocus();
    if (page_ops[app.page]->touch)
        page_ops[app.page]->touch();
}

/* A horizontal swipe changes the page; once a drag goes vertical, it stays a scroll. */
static struct {
    bool active;
    char axis;
    lv_point_t start;
    uint32_t time;
} swipe;

void shell_swipe(bool pressed, lv_point_t p)
{
    if (pressed && !swipe.active) {
        idle_reset();
        lv_area_t nav;
        lv_obj_get_coords(ui.nav, &nav);
        bool in_nav = !lv_obj_is_hidden(ui.nav) && p.y >= nav.y1 && p.y <= nav.y2;
        swipe.active = input_touch_count() == 1 && !in_nav && !lock_active() && !phone_is_open() && !power_menu_is_open() && !dialog_is_open() &&
                       !shell_overlay_open() && lv_obj_is_hidden(ui.menu);
        swipe.axis = 0;
        swipe.start = p;
        swipe.time = lv_tick_get();
        return;
    }
    if (!swipe.active)
        return;
    int32_t dx = p.x - swipe.start.x, dy = p.y - swipe.start.y;
    if (pressed) {
        if (input_touch_count() != 1) {
            swipe.active = false;
        } else if (!swipe.axis && LV_MAX(LV_ABS(dx), LV_ABS(dy)) >= 8) {
            swipe.axis = LV_ABS(dx) > LV_ABS(dy) * 3 / 2 ? 'x' : 'y';
            if (swipe.axis == 'x')
                lv_indev_wait_release(input_touch());
        }
        return;
    }
    swipe.active = false;
    if (swipe.axis == 'x' && LV_ABS(dx) > 50 && LV_ABS(dx) * 2 > LV_ABS(dy) * 3 && lv_tick_elaps(swipe.time) < 800)
        shell_step(dx < 0 ? 1 : -1);
}

/* ---------- construction ---------- */

static void on_back(lv_event_t *e)
{
    back();
    idle_reset();
}

static void on_lock(lv_event_t *e)
{
    lock_engage();
    idle_reset();
}

static void on_menu(lv_event_t *e)
{
    menu_show(lv_obj_is_hidden(ui.menu));
    idle_reset();
}

static void on_page(lv_event_t *e)
{
    navigate((int)(intptr_t)lv_event_get_user_data(e));
}

static void menu_show(bool on)
{
    lv_obj_set_hidden(ui.menu, !on);
    if (on)
        lv_obj_move_foreground(ui.menu);
}

static lv_obj_t *page_button(lv_obj_t *parent, int page, int width)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, width, LV_PCT(100));
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(b, 2, 0);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_bg_color(b, theme.active, 0);
    lv_obj_set_style_text_color(b, theme.dim, 0);
    icon_create(b, page_ops[page]->icon, S(22));
    lv_obj_t *label = lv_label_create(b);
    lv_obj_set_style_text_font(label, F(10), 0);
    lv_label_set_text(label, page_label(page));
    lv_obj_set_clickable(b, true);
    lv_obj_add_event_cb(b, on_page, LV_EVENT_CLICKED, (void *)(intptr_t)page);
    return b;
}

static lv_obj_t *tool_button(lv_obj_t *parent, const char *icon, int width, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, width, 42);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_bg_color(b, theme.button, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_text_color(b, theme.dim, 0);
    lv_obj_t *glyph = icon_create(b, icon, 22);
    lv_obj_center(glyph);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

static void build_bar(void)
{
    ui.bar = lv_obj_create(ui.root);
    lv_obj_remove_style_all(ui.bar);
    lv_obj_set_size(ui.bar, LV_PCT(100), STATUS_H);
    lv_obj_set_style_bg_color(ui.bar, theme.chrome, 0);
    lv_obj_set_style_bg_opa(ui.bar, theme.desktop_wallpaper ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(ui.bar, 12, 0);
    lv_obj_set_flex_flow(ui.bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ui.bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(ui.bar, 5, 0);
    lv_obj_set_scrollable(ui.bar, false);
    lv_obj_set_clickable(ui.bar, false);
    lv_obj_set_hidden(ui.bar, !theme.status_bar);

    ui.bar_sim = ui_text(ui.bar, 9, true, theme.bg);
    lv_obj_set_style_bg_color(ui.bar_sim, theme.text, 0);
    lv_obj_set_style_bg_opa(ui.bar_sim, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ui.bar_sim, 2, 0);
    lv_obj_set_style_pad_hor(ui.bar_sim, 2, 0);
    ui.bar_op = ui_text(ui.bar, 10, false, theme.text);
    lv_obj_set_style_max_width(ui.bar_op, 72, 0);
    lv_label_set_long_mode(ui.bar_op, LV_LABEL_LONG_MODE_DOTS);
    ui.bar_tech = ui_text(ui.bar, 9, true, theme.accent);
    ui.bar_rec = ui_text(ui.bar, 10, true, theme.poor);
    lv_obj_set_hidden(ui.bar_rec, !record_active());
    lv_obj_t *spacer = lv_obj_create(ui.bar);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_flex_grow(spacer, 1);
    ui.bar_notes = ui_text(ui.bar, 10, false, theme.accent);
    ui.bar_sms = icon_create(ui.bar, "sms", 13);
    lv_obj_set_style_text_color(ui.bar_sms, theme.accent, 0);
    ui.bar_bars = bars_create(ui.bar, 15, 11);
    lv_obj_t *dual = lv_obj_create(ui.bar);
    lv_obj_remove_style_all(dual);
    lv_obj_set_size(dual, 15, 15);
    lv_obj_set_flex_flow(dual, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(dual, 1, 0);
    for (int i = 0; i < 2; i++)
        ui.bar_dual[i] = bars_create(dual, 15, 10);
    lv_obj_set_hidden(dual, true);
    ui.bar_bat = lv_obj_create(ui.bar);
    lv_obj_remove_style_all(ui.bar_bat);
    lv_obj_set_size(ui.bar_bat, 22, 15);
    lv_obj_add_event_cb(ui.bar_bat, battery_draw, LV_EVENT_DRAW_MAIN, NULL);
    ui.bar_pct = ui_text(ui.bar, 10, false, theme.text);
    ui.bar_clock = ui_text(ui.bar, 10, false, theme.text);
}

static void build_toolbar(void)
{
    ui.toolbar = lv_obj_create(ui.root);
    lv_obj_remove_style_all(ui.toolbar);
    lv_obj_set_size(ui.toolbar, LV_PCT(100), TOOLBAR_H);
    lv_obj_set_style_bg_color(ui.toolbar, theme.chrome, 0);
    lv_obj_set_style_bg_opa(ui.toolbar, theme.desktop_wallpaper ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(ui.toolbar, 6, 0);
    lv_obj_set_flex_flow(ui.toolbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ui.toolbar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(ui.toolbar, false);
    lv_obj_t *back_button = tool_button(ui.toolbar, "home", 30, on_back);
    ui.back_icon = lv_obj_get_child(back_button, 0);
    ui.title = ui_text(ui.toolbar, 18, true, theme.text);
    lv_obj_set_style_pad_hor(ui.title, 8, 0);
    lv_label_set_long_mode(ui.title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_flex_grow(ui.title, 1);
    ui.spinner = lv_spinner_create(ui.toolbar);
    lv_obj_set_size(ui.spinner, 16, 16);
    lv_obj_set_style_arc_width(ui.spinner, 2, 0);
    lv_obj_set_style_arc_width(ui.spinner, 2, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(ui.spinner, lv_color_mix(theme.accent, theme.chrome, 51), 0);
    lv_obj_set_style_arc_color(ui.spinner, theme.accent, LV_PART_INDICATOR);
    lv_obj_set_style_margin_right(ui.spinner, 8, 0);
    lv_obj_set_hidden(ui.spinner, true);
    tool_button(ui.toolbar, "lock", 40, on_lock);
    tool_button(ui.toolbar, "menu", 40, on_menu);
}

static void build_nav(void)
{
    ui.nav = lv_obj_create(ui.root);
    lv_obj_remove_style_all(ui.nav);
    lv_obj_set_size(ui.nav, LV_PCT(100), NAV_H);
    lv_obj_set_style_bg_color(ui.nav, theme.nav, 0);
    lv_obj_set_style_bg_opa(ui.nav, theme.desktop_wallpaper ? theme.surface_opa : LV_OPA_COVER, 0);
    lv_obj_set_style_pad_top(ui.nav, 3, 0);
    lv_obj_set_style_pad_bottom(ui.nav, 5, 0);
    lv_obj_set_style_pad_hor(ui.nav, 7, 0);
    lv_obj_set_style_pad_gap(ui.nav, 3, 0);
    lv_obj_set_flex_flow(ui.nav, LV_FLEX_FLOW_ROW);
    lv_obj_set_scrollable(ui.nav, false);
    lv_obj_set_clickable(ui.nav, true);
    lv_obj_add_event_cb(ui.nav, on_nav, LV_EVENT_ALL, NULL);

    /* Translucent glass: a lighter top edge fading down, and a thin rim. */
    ui.nav_pill = lv_obj_create(ui.nav);
    lv_obj_remove_style_all(ui.nav_pill);
    lv_obj_set_floating(ui.nav_pill, true);
    lv_obj_set_clickable(ui.nav_pill, false);
    lv_obj_set_scrollable(ui.nav_pill, false);
    lv_obj_set_style_radius(ui.nav_pill, S(16), 0);
    lv_obj_set_style_bg_opa(ui.nav_pill, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(ui.nav_pill, lv_color_white(), 0);
    lv_obj_set_style_bg_grad_color(ui.nav_pill, lv_color_white(), 0);
    lv_obj_set_style_bg_grad_dir(ui.nav_pill, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_main_opa(ui.nav_pill, theme.light ? LV_OPA_80 : 46, 0);
    lv_obj_set_style_bg_grad_opa(ui.nav_pill, theme.light ? LV_OPA_50 : 14, 0);
    lv_obj_set_style_border_width(ui.nav_pill, 1, 0);
    lv_obj_set_style_border_color(ui.nav_pill, theme.light ? theme.line : lv_color_white(), 0);
    lv_obj_set_style_border_opa(ui.nav_pill, theme.light ? LV_OPA_COVER : 60, 0);
    lv_obj_set_hidden(ui.nav_pill, true);

    for (int i = 0; i < 5; i++) {
        ui.nav_buttons[i] = page_button(ui.nav, nav_slots[i], 0);
        lv_obj_set_flex_grow(ui.nav_buttons[i], 1);
        lv_obj_set_clickable(ui.nav_buttons[i], false);
    }
    lv_obj_set_hidden(ui.nav, !theme.nav_bar);
}

static void build_menu(void)
{
    ui.menu = ui_box(ui.content, LV_FLEX_FLOW_COLUMN, 12);
    lv_obj_set_size(ui.menu, LV_PCT(100), LV_PCT(100));
    lv_obj_set_floating(ui.menu, true);
    lv_obj_set_clickable(ui.menu, true);
    lv_obj_set_style_bg_color(ui.menu, theme.bg, 0);
    lv_obj_set_style_bg_opa(ui.menu, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(ui.menu, 14, 0);
    /* Three rows of large cells outgrow the screen at the bigger sizes. */
    lv_obj_set_scrollable(ui.menu, true);
    lv_obj_set_scroll_dir(ui.menu, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(ui.menu, LV_SCROLLBAR_MODE_OFF);
    lv_obj_t *head = ui_box(ui.menu, LV_FLEX_FLOW_ROW, 8);
    lv_obj_t *title = ui_text(head, 16, true, theme.text);
    lv_label_set_text(title, L("全部页面", "All pages"));
    lv_obj_set_flex_grow(title, 1);
    tool_button(head, "close", 40, on_menu);
    lv_obj_t *grid = ui_box(ui.menu, LV_FLEX_FLOW_ROW_WRAP, 8);
    int32_t cell = (lv_display_get_horizontal_resolution(lv_display_get_default()) - 2 * 14 - 2 * S(8)) / 3;
    /* In page order, the same order the number keys select. */
    for (int page = 0; page < PAGE_COUNT; page++) {
        lv_obj_t *b = page_button(grid, page, cell);
        lv_obj_set_height(b, S(70));
        lv_obj_set_style_bg_opa(b, theme.surface_opa, 0);
        lv_obj_set_style_bg_color(b, theme.surface, 0);
        lv_obj_set_style_text_color(b, page == (int)app.page ? theme.accent : theme.text, 0);
    }
    lv_obj_set_hidden(ui.menu, true);
}

static void build_overlays(void)
{
    lv_obj_t *sys = lv_layer_sys();
    ui.toast = ui_text(sys, 13, false, theme.text);
    lv_obj_set_style_bg_color(ui.toast, theme.button, 0);
    lv_obj_set_style_bg_opa(ui.toast, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ui.toast, 18, 0);
    lv_obj_set_style_pad_ver(ui.toast, 8, 0);
    lv_obj_set_style_pad_hor(ui.toast, 14, 0);
    /* A pixel cap for the same reason as a row's value: a wrapping label measures
     * itself against the raw value, so a percentage would not wrap. */
    lv_obj_set_style_max_width(ui.toast, S(288), 0);
    lv_label_set_long_mode(ui.toast, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(ui.toast, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(ui.toast, LV_ALIGN_BOTTOM_MID, 0, -((theme.nav_bar ? NAV_H : 0) + 8));
    lv_obj_set_hidden(ui.toast, true);

    ui.osd = ui_box(sys, LV_FLEX_FLOW_ROW, 10);
    lv_obj_set_width(ui.osd, LV_PCT(92));
    lv_obj_align(ui.osd, LV_ALIGN_TOP_MID, 0, (theme.status_bar ? STATUS_H : 0) + TOOLBAR_H + 8);
    lv_obj_set_style_bg_color(ui.osd, theme.surface, 0);
    lv_obj_set_style_bg_opa(ui.osd, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ui.osd, 14, 0);
    lv_obj_set_style_pad_all(ui.osd, 12, 0);
    ui.osd_label = ui_text(ui.osd, 13, false, theme.text);
    ui.osd_bar = lv_bar_create(ui.osd);
    lv_obj_set_flex_grow(ui.osd_bar, 1);
    lv_obj_set_height(ui.osd_bar, 6);
    lv_obj_set_style_bg_color(ui.osd_bar, theme.line, 0);
    lv_obj_set_style_bg_opa(ui.osd_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(ui.osd_bar, theme.accent, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(ui.osd_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    ui.osd_value = ui_text(ui.osd, 13, true, theme.text);
    lv_obj_set_hidden(ui.osd, true);

    lv_group_set_default(ui.notice_group);
    ui.notice = ui_box(ui.root, LV_FLEX_FLOW_COLUMN, 8);
    lv_obj_set_floating(ui.notice, true);
    lv_obj_set_clickable(ui.notice, true);
    lv_obj_set_width(ui.notice, LV_PCT(95));
    lv_obj_align(ui.notice, LV_ALIGN_TOP_MID, 0, (theme.status_bar ? STATUS_H : 0) + 12);
    lv_obj_set_style_bg_color(ui.notice, theme.surface, 0);
    lv_obj_set_style_bg_opa(ui.notice, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(ui.notice, theme.accent, 0);
    lv_obj_set_style_border_width(ui.notice, 1, 0);
    lv_obj_set_style_radius(ui.notice, 12, 0);
    lv_obj_set_style_pad_all(ui.notice, 12, 0);
    lv_obj_set_style_shadow_width(ui.notice, 24, 0);
    lv_obj_set_style_shadow_opa(ui.notice, LV_OPA_60, 0);
    ui.notice_title = ui_text(ui.notice, 14, true, theme.text);
    ui.notice_body = ui_text(ui.notice, 13, false, theme.text);
    lv_obj_set_width(ui.notice_body, LV_PCT(100));
    lv_label_set_long_mode(ui.notice_body, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_t *actions = ui_box(ui.notice, LV_FLEX_FLOW_ROW, 8);
    lv_obj_t *open = ui_button(actions, L("查看", "Open"), notice_open, NULL);
    ui_primary(open);
    lv_obj_set_flex_grow(open, 1);
    lv_obj_set_flex_grow(ui_button(actions, L("稍后", "Later"), notice_dismiss, NULL), 1);
    lv_obj_set_hidden(ui.notice, true);

    lv_group_set_default(ui.picture_group);
    ui.picture = ui_box(ui.root, LV_FLEX_FLOW_COLUMN, 12);
    lv_obj_set_floating(ui.picture, true);
    lv_obj_set_clickable(ui.picture, true);
    lv_obj_set_size(ui.picture, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_align(ui.picture, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(ui.picture, theme.bg, 0);
    lv_obj_set_style_bg_opa(ui.picture, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(ui.picture, picture_close, LV_EVENT_CLICKED, NULL);
    lv_obj_set_hidden(ui.picture, true);
    lv_group_set_default(ui.group);
}

static void build(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, theme.bg, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    ui.root = lv_obj_create(screen);
    lv_obj_remove_style_all(ui.root);
    lv_obj_set_size(ui.root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(ui.root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(ui.root, false);
    lv_obj_set_style_text_color(ui.root, theme.text, 0);
    if (theme.desktop_wallpaper) {
        static const char path[] = "A:/etc/e5-infoscreen/wallpapers/desktop.png";
        lv_image_cache_drop(path);
        ui.wallpaper = lv_image_create(ui.root);
        lv_image_set_src(ui.wallpaper, path);
        lv_obj_set_floating(ui.wallpaper, true);
        lv_obj_set_size(ui.wallpaper, LV_PCT(100), LV_PCT(100));
        lv_image_set_inner_align(ui.wallpaper, LV_IMAGE_ALIGN_COVER);
    }

    ui.group = lv_group_create();
    ui.notice_group = lv_group_create();
    ui.picture_group = lv_group_create();
    lv_group_set_wrap(ui.group, false);
    lv_group_set_default(ui.group);
    input_set_group(ui.group);

    /* The chrome keeps the panel's fixed bar heights, as the web page's does: the
     * status bar and nav at their standard size, the title at most 120 %. */
    float scale = theme_cap(1);
    build_bar();
    theme.scale = scale;
    theme_cap(1.2f);
    build_toolbar();
    theme.scale = scale;
    ui.content = lv_obj_create(ui.root);
    lv_obj_remove_style_all(ui.content);
    lv_obj_set_width(ui.content, LV_PCT(100));
    lv_obj_set_flex_grow(ui.content, 1);
    lv_obj_set_scrollable(ui.content, false);
    for (int i = 0; i < PAGE_COUNT; i++) {
        ui.pages[i] = ui_page(ui.content);
        lv_obj_set_floating(ui.pages[i], true);
        ui.errors[i] = ui_text(ui.pages[i], 12, false, theme.text);
        lv_obj_set_width(ui.errors[i], LV_PCT(100));
        lv_obj_set_style_bg_color(ui.errors[i], theme.surface, 0);
        lv_obj_set_style_bg_opa(ui.errors[i], theme.surface_opa, 0);
        lv_obj_set_style_radius(ui.errors[i], 10, 0);
        lv_obj_set_style_pad_all(ui.errors[i], 8, 0);
        lv_label_set_long_mode(ui.errors[i], LV_LABEL_LONG_MODE_WRAP);
        lv_label_set_text(ui.errors[i], L("加载失败，请重新进入页面重试", "Could not load. Reopen this page to retry."));
        lv_obj_set_hidden(ui.errors[i], !load_failed[i]);
        page_ops[i]->create(ui.pages[i]);
        lv_obj_set_hidden(ui.pages[i], i != (int)app.page);
    }
    build_menu();
    theme_cap(1);
    build_nav();
    theme.scale = scale;
    build_overlays();
    lock_create();
    phone_init();
    shell_title();
}

void shell_rebuild(void)
{
    keyboard_close();
    power_menu_close();
    dialog_close();
    lv_obj_clean(lv_layer_sys());
    lv_obj_clean(lv_layer_top());
    lv_obj_delete(ui.root);
    lv_group_delete(ui.group);
    lv_group_delete(ui.notice_group);
    lv_group_delete(ui.picture_group);
    memset(&ui, 0, sizeof ui);
    build();
    shell_show(app.page);
    clock_tick(NULL);
}

void shell_init(void)
{
    memcpy(nav_slots, nav_defaults, sizeof nav_slots);
    theme_apply(NULL);
    poll_timer = lv_timer_create(poll_fire, 1, NULL);
    idle_timer = lv_timer_create(idle_fire, 60000, NULL);
    lv_timer_pause(idle_timer);
    clock_timer = lv_timer_create(clock_tick, 1000, NULL);
    toast_timer = lv_timer_create(toast_hide, 2000, NULL);
    lv_timer_pause(toast_timer);
    osd_timer = lv_timer_create(osd_hide, 1500, NULL);
    lv_timer_pause(osd_timer);
    spinner_timer = lv_timer_create(spinner_show, 150, NULL);
    lv_timer_pause(spinner_timer);
    volume_timer = lv_timer_create(volume_repeat, 120, NULL);
    lv_timer_pause(volume_timer);
    chord_timer = lv_timer_create(chord_hold, 800, NULL);
    lv_timer_pause(chord_timer);
    power_key.timer = lv_timer_create(power_hold, 1200, NULL);
    lv_timer_pause(power_key.timer);
    build();
    lock_engage();
    shell_show(PAGE_OVERVIEW);
    clock_tick(NULL);
    api_call_events(phone_calls);
}
