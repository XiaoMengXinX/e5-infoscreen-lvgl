/* The swipe lock prevents accidental operation; it is not authentication. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/misc/cache/instance/lv_image_cache.h"

#include "api.h"
#include "app.h"
#include "icons.h"
#include "input.h"
#include "theme.h"
#include "ui.h"
#include "util.h"

#define SAMPLES 32

static struct {
    lv_obj_t *root, *time, *date, *network, *dot, *hint;
    lv_obj_t *summary, *usage, *day, *month, *rates;
    lv_obj_t *day_value[2], *month_value[2], *rx[2], *tx[2];
} ui;

static bool locked = true, dark, animating, first_config = true, lock_was_enabled = true;
static uint32_t wake_at, block_until, key_armed;
static int usage_slot = -1;
static uint32_t usage_at;
static bool usage_busy, usage_fresh;
static unsigned usage_generation;
static cJSON *usage;

static struct {
    bool active;
    int32_t x, y, last_x, last_y, peak_y, base;
    uint32_t time;
    struct {
        int32_t y;
        uint32_t t;
    } samples[SAMPLES];
    int count;
} gesture;

bool lock_active(void)
{
    return locked || dark;
}

bool lock_touch_blocked(void)
{
    return !locked && lv_tick_get() < block_until;
}

static int32_t screen_height(void)
{
    return lv_display_get_vertical_resolution(lv_display_get_default());
}

static void set_metric(lv_obj_t *labels[2], const char *text)
{
    char number[32];
    snprintf(number, sizeof number, "%s", text);
    char *space = strchr(number, ' ');
    if (space)
        *space = 0;
    ui_set_text(labels[0], number);
    ui_set_text(labels[1], space ? space + 1 : "");
}

static void paint_usage(void)
{
    static const char *const keys[2] = {"today", "month"};
    lv_obj_t **targets[2] = {ui.day_value, ui.month_value};
    for (int i = 0; i < 2; i++) {
        char text[32] = "—";
        char used[32];
        snprintf(used, sizeof used, "%s_used", keys[i]);
        const cJSON *span = jget(usage, keys[i]);
        if (jtrue(usage, "available") && span) {
            double total;
            if (!jnum(usage, used, &total))
                total = jnum_or(span, "rx", 0) + jnum_or(span, "tx", 0);
            fmt_amount(total, text, sizeof text);
        }
        set_metric(targets[i], text);
    }
}

static void usage_done(int status, cJSON *reply, void *ctx)
{
    usage_busy = false;
    if ((unsigned)(uintptr_t)ctx == usage_generation) {
        cJSON_Delete(usage);
        const cJSON *source = jget(reply, "wan") ? jget(reply, "wan") : reply;
        usage = status == 200 && source ? cJSON_Duplicate(source, true) : NULL;
    }
    if (ui.root)
        paint_usage();
    if ((unsigned)(uintptr_t)ctx != usage_generation && !dark && locked)
        lock_refresh_usage();
}

static void fetch_usage(void)
{
    if (usage_busy || dark || (usage_fresh && lv_tick_elaps(usage_at) < 10000))
        return;
    usage_busy = true;
    usage_fresh = true;
    usage_at = lv_tick_get();
    char path[32] = "/traffic";
    if (usage_slot >= 0)
        snprintf(path, sizeof path, "/traffic?slot=%d", usage_slot);
    api_get(path, usage_done, (void *)(uintptr_t)usage_generation);
}

void lock_refresh_usage(void)
{
    usage_generation++;
    usage_fresh = false;
    cJSON_Delete(usage);
    usage = NULL;
    if (ui.root && !dark)
        paint_usage();
    if (locked && !dark)
        fetch_usage();
}

static void anim_y(void *obj, int32_t v)
{
    lv_obj_set_style_translate_y(obj, v, 0);
}

/* A drag that does not unlock springs back rather than snapping. */
static void reset_gesture(void)
{
    gesture.active = false;
    if (animating || !ui.root)
        return;
    int32_t current = lv_obj_get_style_translate_y(ui.root, 0);
    lv_anim_delete(ui.root, anim_y);
    if (!current || !theme.motion) {
        lv_obj_set_style_translate_y(ui.root, 0, 0);
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, ui.root);
    lv_anim_set_exec_cb(&a, anim_y);
    lv_anim_set_values(&a, current, 0);
    lv_anim_set_duration(&a, 280);
    lv_anim_set_path_cb(&a, ui_ease_out);
    lv_anim_start(&a);
}

static void finish_unlock(void)
{
    locked = false;
    animating = false;
    gesture.active = false;
    block_until = lv_tick_get() + 450;
    if (ui.root) {
        lv_obj_set_hidden(ui.root, true);
        lv_obj_set_style_translate_y(ui.root, 0, 0);
    }
    idle_reset();
    shell_show(app.page);
}

static void unlock_done(lv_anim_t *a)
{
    if (animating)
        finish_unlock();
}

static void unlock(float velocity)
{
    if (animating)
        return;
    int32_t h = screen_height();
    int32_t current = lv_obj_get_style_translate_y(ui.root, 0);
    float remaining = (float)(h + current);
    /* The ease-out starts at 3 × remaining / duration: begin at the finger's speed. */
    uint32_t ms = theme.motion ? (uint32_t)fminf(360, fmaxf(160, 3 * remaining / fmaxf(0.8f, velocity))) : 0;
    if (!ms) {
        finish_unlock();
        return;
    }
    animating = true;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, ui.root);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_path_cb(&a, ui_ease_out);
    lv_anim_set_exec_cb(&a, anim_y);
    lv_anim_set_values(&a, current, -h * 105 / 100);
    lv_anim_set_completed_cb(&a, unlock_done);
    lv_anim_start(&a);
}

static void gesture_start(lv_point_t p)
{
    if (input_touch_count() != 1 || animating || p.y < screen_height() / 4 || dark || jfalse(screen_config(), "touch")) {
        reset_gesture();
        return;
    }
    /* Catching the lock while it springs back carries on from where it is. */
    lv_anim_delete(ui.root, anim_y);
    gesture.base = lv_obj_get_style_translate_y(ui.root, 0);
    gesture.active = true;
    gesture.x = gesture.last_x = p.x;
    gesture.y = gesture.last_y = gesture.peak_y = p.y;
    gesture.time = lv_tick_get();
    gesture.samples[0].y = p.y;
    gesture.samples[0].t = gesture.time;
    gesture.count = 1;
    idle_reset();
}

static void gesture_move(lv_point_t p)
{
    if (!gesture.active)
        return;
    int32_t distance = gesture.y - p.y > 0 ? gesture.y - p.y : 0;
    if (input_touch_count() != 1 || abs(p.x - gesture.x) > fmaxf(72, distance * 0.8f) || p.y > gesture.y + 20) {
        reset_gesture();
        return;
    }
    gesture.last_x = p.x;
    gesture.last_y = p.y;
    if (p.y < gesture.peak_y)
        gesture.peak_y = p.y;
    uint32_t now = lv_tick_get();
    if (gesture.count == SAMPLES) {
        memmove(gesture.samples, gesture.samples + 1, sizeof gesture.samples[0] * (SAMPLES - 1));
        gesture.count--;
    }
    gesture.samples[gesture.count].y = p.y;
    gesture.samples[gesture.count++].t = now;
    while (gesture.count > 2 && now - gesture.samples[1].t > 120) {
        memmove(gesture.samples, gesture.samples + 1, sizeof gesture.samples[0] * (size_t)(gesture.count - 1));
        gesture.count--;
    }
    lv_obj_set_style_translate_y(ui.root, gesture.base - distance, 0);
    idle_reset();
}

static void gesture_end(lv_point_t p)
{
    if (!gesture.active)
        return;
    int32_t h = screen_height();
    uint32_t now = lv_tick_get();
    int32_t dy = gesture.y - p.y - gesture.base, dx = abs(gesture.x - p.x);
    uint32_t elapsed = now - gesture.time;
    const char *distance = jstr(screen_config(), "unlock_distance");
    bool longer = distance && !strcmp(distance, "long");
    float threshold = longer ? (float)h * 0.5f : fmaxf(88, (float)h * 0.20f);
    int first = -1;
    for (int i = 0; i < gesture.count; i++)
        if (now - gesture.samples[i].t <= 140) {
            first = i;
            break;
        }
    float velocity = 0;
    if (first >= 0 && now - gesture.samples[gesture.count - 1].t < 100) {
        float span = fmaxf(16, (float)(now - gesture.samples[first].t));
        velocity = fmaxf(0, (float)(gesture.samples[first].y - p.y) / span);
    }
    bool reversed = p.y - gesture.peak_y > fmaxf(16, (float)h * 0.035f);
    float projected = (float)dy + fminf(160, velocity * velocity / (2 * 0.0045f));
    bool flick = !longer && dy >= 48 && velocity >= 0.5f && projected >= threshold;
    if (!reversed && (dy >= threshold || flick) && dy > dx * 1.35f && elapsed >= 24 && now - wake_at >= 180)
        unlock(velocity);
    else
        reset_gesture();
}

static void on_touch(lv_event_t *e)
{
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
        gesture_start(p);
        break;
    case LV_EVENT_PRESSING:
        gesture_move(p);
        break;
    case LV_EVENT_RELEASED:
        gesture_end(p);
        break;
    case LV_EVENT_PRESS_LOST:
        reset_gesture();
        break;
    default:
        break;
    }
}

static lv_obj_t *metric(lv_obj_t *parent, const char *label, lv_obj_t *value[2], int size)
{
    lv_obj_t *box = ui_box(parent, LV_FLEX_FLOW_COLUMN, 2);
    lv_obj_set_flex_grow(box, 1);
    lv_obj_set_width(box, 0);
    lv_label_set_text(ui_text(box, 10, false, theme.dim), label);
    lv_obj_t *line = ui_box(box, LV_FLEX_FLOW_ROW, 3);
    lv_obj_set_flex_align(line, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    value[0] = ui_text(line, size, true, theme.text);
    value[1] = ui_text(line, 11, false, theme.dim);
    lv_label_set_text_static(value[0], "—");
    return box;
}

static lv_obj_t *panel(lv_obj_t *parent)
{
    lv_obj_t *p = ui_box(parent, LV_FLEX_FLOW_ROW, 12);
    lv_obj_set_style_bg_color(p, theme.surface, 0);
    lv_obj_set_style_bg_opa(p, theme.lock_wallpaper ? LV_OPA_60 : LV_OPA_80, 0);
    lv_obj_set_style_radius(p, S(16), 0);
    lv_obj_set_style_pad_all(p, S(14), 0);
    return p;
}

static void apply_config(void)
{
    const cJSON *c = screen_config();
    bool daily = !jfalse(c, "lock_daily"), monthly = !jfalse(c, "lock_monthly"), speed = !jfalse(c, "lock_speed");
    lv_obj_set_hidden(ui.day, !daily);
    lv_obj_set_hidden(ui.month, !monthly);
    lv_obj_set_hidden(ui.usage, !daily && !monthly);
    lv_obj_set_hidden(ui.rates, !speed);
    if (!key_armed)
        ui_set_text(ui.hint, jfalse(c, "touch") ? L("触控已关闭 · 按两次确认键解锁", "Touch off · press OK twice")
                                                : L("从下方向上滑动解锁", "Swipe up to unlock"));
}

void lock_create(void)
{
    /* The lock keeps its layout at the standard size so it always fits the panel. */
    float scale = theme.scale;
    theme.scale = scale < 1 ? scale : 1;
    int32_t height = screen_height() - shell_status_bar_height();
    /* The lock slides up inside this window, which keeps the status bar clear. */
    lv_obj_t *window = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(window);
    lv_obj_set_size(window, LV_PCT(100), height);
    lv_obj_align(window, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_scrollable(window, false);
    lv_obj_set_clickable(window, false);
    ui.root = lv_obj_create(window);
    lv_obj_remove_style_all(ui.root);
    lv_obj_set_size(ui.root, LV_PCT(100), height);
    lv_obj_set_style_bg_opa(ui.root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(ui.root, theme.hero_start, 0);
    lv_obj_set_style_bg_grad_color(ui.root, theme.bg, 0);
    lv_obj_set_style_bg_grad_dir(ui.root, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_pad_hor(ui.root, S(18), 0);
    lv_obj_set_style_pad_top(ui.root, S(28), 0);
    lv_obj_set_style_pad_bottom(ui.root, S(18), 0);
    lv_obj_set_flex_flow(ui.root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ui.root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(ui.root, S(8), 0);
    lv_obj_set_scrollable(ui.root, false);
    lv_obj_set_gesture_bubble(ui.root, false);
    lv_obj_set_clickable(ui.root, true);
    lv_obj_set_press_lock(ui.root, true);
    lv_obj_add_event_cb(ui.root, on_touch, LV_EVENT_ALL, NULL);
    if (theme.lock_wallpaper) {
        static const char path[] = "A:/etc/e5-infoscreen/wallpapers/lock.png";
        lv_image_cache_drop(path);
        lv_obj_t *wallpaper = lv_image_create(ui.root);
        lv_image_set_src(wallpaper, path);
        lv_obj_set_floating(wallpaper, true);
        lv_obj_set_size(wallpaper, lv_display_get_horizontal_resolution(lv_display_get_default()), height);
        lv_obj_align(wallpaper, LV_ALIGN_TOP_LEFT, -S(18), -S(28));
        lv_image_set_inner_align(wallpaper, LV_IMAGE_ALIGN_COVER);
    }

    lv_obj_set_style_text_color(icon_create(ui.root, "lock", S(22)), theme.accent, 0);
    ui.date = ui_text(ui.root, 15, false, theme.text);
    ui.time = ui_text(ui.root, 56, false, theme.text);
    lv_obj_set_style_text_letter_space(ui.time, -2, 0);
    lv_obj_t *online = ui_box(ui.root, LV_FLEX_FLOW_ROW, 6);
    lv_obj_set_width(online, LV_SIZE_CONTENT);
    ui.dot = lv_obj_create(online);
    lv_obj_remove_style_all(ui.dot);
    lv_obj_set_size(ui.dot, S(7), S(7));
    lv_obj_set_style_radius(ui.dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(ui.dot, LV_OPA_COVER, 0);
    ui.network = ui_text(online, 12, false, theme.dim);

    lv_obj_t *spacer = lv_obj_create(ui.root);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_flex_grow(spacer, 1);

    ui.summary = ui_box(ui.root, LV_FLEX_FLOW_COLUMN, 10);
    ui.usage = panel(ui.summary);
    ui.day = metric(ui.usage, L("今日已用", "TODAY"), ui.day_value, 22);
    ui.month = metric(ui.usage, L("本月已用", "THIS MONTH"), ui.month_value, 22);
    ui.rates = panel(ui.summary);
    metric(ui.rates, L("↓ 下载", "↓ DOWN"), ui.rx, 18);
    metric(ui.rates, L("↑ 上传", "↑ UP"), ui.tx, 18);

    lv_obj_t *swipe = ui_box(ui.root, LV_FLEX_FLOW_COLUMN, 4);
    lv_obj_set_flex_align(swipe, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_text_color(icon_create(swipe, "up", S(22)), theme.dim, 0);
    ui.hint = ui_text(swipe, 12, false, theme.dim);
    key_armed = 0;

    apply_config();
    paint_usage();
    lock_clock();
    if (app.status)
        lock_status(app.status);
    lv_obj_set_hidden(ui.root, !locked);
    theme.scale = scale;
}

void lock_clock(void)
{
    if (!ui.root)
        return;
    struct local_time now = local_now();
    ui_set_fmt(ui.time, "%02d:%02d", now.hour, now.minute);
    if (app.en)
        ui_set_fmt(ui.date, "%02d / %02d", now.month, now.day);
    else
        ui_set_fmt(ui.date, "%d月%d日", now.month, now.day);
}

void lock_engage(void)
{
    lv_anim_delete(ui.root, NULL);
    animating = false;
    locked = true;
    wake_at = lv_tick_get();
    key_armed = 0;
    /* The keyboard is drawn above the lock, so an open field must give it up. */
    input_defocus();
    if (phone_is_open() && !phone_call_active())
        phone_close();
    gesture.active = false;
    lv_obj_set_style_translate_y(ui.root, 0, 0);
    lv_obj_set_hidden(ui.root, false);
    apply_config();
    lock_clock();
    usage_fresh = false;
    fetch_usage();
}

void lock_power(bool blank)
{
    dark = blank;
    lock_engage();
    if (!blank && jfalse(screen_config(), "lock_enabled"))
        finish_unlock();
}

void lock_status(const cJSON *st)
{
    const cJSON *c = jget(st, "screen");
    bool enabled = !jfalse(c, "lock_enabled");
    double slot;
    if (jnum(st, "modem.sim_card", &slot) && (slot == 0 || slot == 1) && (int)slot != usage_slot) {
        usage_slot = (int)slot;
        lock_refresh_usage();
    }
    if (!enabled && locked && !dark && (first_config || lock_was_enabled))
        finish_unlock();
    first_config = false;
    lock_was_enabled = enabled;
    if (dark || !ui.root)
        return;
    apply_config();
    bool bytes = jstr(c, "speed_unit") && !strcmp(jstr(c, "speed_unit"), "bytes");
    lv_obj_t **targets[2] = {ui.rx, ui.tx};
    static const char *const rates[2] = {"traffic.rx_rate", "traffic.tx_rate"};
    for (int i = 0; i < 2; i++) {
        char num[24], unit[24];
        double rate;
        fmt_speed(jnum(st, rates[i], &rate) ? rate : NAN, bytes, num, unit, sizeof num);
        ui_set_text(targets[i][0], num);
        ui_set_text(targets[i][1], unit);
    }
    bool up = jtrue(st, "wan.up");
    ui_set_text(ui.network, up ? L("蜂窝网络已连接", "Mobile network connected") : L("等待网络连接", "Connecting"));
    lv_obj_set_style_bg_color(ui.dot, up ? theme.accent : theme.fair, 0);
    if (locked)
        fetch_usage();
}

bool lock_key(const struct key_event *key)
{
    if (!locked || dark)
        return false;
    if (jfalse(screen_config(), "touch") && key->kind == KEY_KIND_OK && !key->repeat && lv_tick_elaps(wake_at) >= 400) {
        if (key_armed && lv_tick_elaps(key_armed) < 1800) {
            unlock(0);
        } else {
            key_armed = lv_tick_get();
            ui_set_text(ui.hint, L("再按一次确认键解锁", "Press OK again"));
        }
    }
    return true;
}
