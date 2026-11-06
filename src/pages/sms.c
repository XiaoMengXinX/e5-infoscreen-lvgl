#include <stdio.h>
#include <string.h>

#include "api.h"
#include "app.h"
#include "theme.h"
#include "ui.h"
#include "util.h"

static struct {
    lv_obj_t *list, *view, *from, *time, *text, *delete;
} ui;

static cJSON *messages;
static double open_id = -1;
static bool reading, has_unread_snapshot;
static cJSON *unread_snapshot;
static char hidden_revision[64];
static lv_timer_t *disarm_timer;

static const cJSON *find(double id)
{
    const cJSON *m;
    cJSON_ArrayForEach(m, messages)
        if (jnum_or(m, "id", -2) == id)
            return m;
    return NULL;
}

static bool id_in(const cJSON *array, double id)
{
    const cJSON *item;
    cJSON_ArrayForEach(item, array)
        if (cJSON_IsNumber(item) && item->valuedouble == id)
            return true;
    return false;
}

static void sms_time(const char *ts, char *out, size_t size)
{
    int y, mo, d, h, mi;
    if (!ts || sscanf(ts, "%4d-%2d-%2dT%2d:%2d", &y, &mo, &d, &h, &mi) != 5) {
        snprintf(out, size, "%s", "");
        return;
    }
    struct local_time now = local_now();
    if (now.year == y && now.month == mo && now.day == d)
        snprintf(out, size, "%02d:%02d", h, mi);
    else
        snprintf(out, size, "%02d-%02d %02d:%02d", mo, d, h, mi);
}

static void meta(const cJSON *m, char *out, size_t size)
{
    char when[24];
    sms_time(jstr(m, "time"), when, sizeof when);
    const char *sim = jstr(m, "sim");
    snprintf(out, size, "%s%s%s", sim ? sim : "", sim && *when ? " · " : "", when);
}

static void on_open(lv_event_t *e);

static void render_list(void)
{
    lv_obj_clean(ui.list);
    if (!cJSON_GetArraySize(messages)) {
        lv_obj_t *card = ui_card(ui.list);
        lv_label_set_text(ui_text(card, 13, false, theme.dim), L("没有短信", "No messages"));
        return;
    }
    const cJSON *unread = jget(app.status, "sms.unread"), *m;
    cJSON_ArrayForEach(m, messages) {
        double id = jnum_or(m, "id", -1);
        bool fresh = jtrue(m, "unread") || id_in(unread, id);
        lv_obj_t *row = ui_box(ui.list, LV_FLEX_FLOW_COLUMN, 4);
        lv_obj_set_clickable(row, true);
        lv_obj_set_style_bg_color(row, theme.surface, 0);
        lv_obj_set_style_bg_opa(row, theme.surface_opa, 0);
        lv_obj_set_style_bg_color(row, lv_color_mix(lv_color_white(), theme.surface, 30), LV_STATE_PRESSED);
        lv_obj_set_style_radius(row, S(12), 0);
        lv_obj_set_style_pad_ver(row, S(theme.compact ? 9 : 12), 0);
        lv_obj_set_style_pad_hor(row, S(theme.compact ? 11 : 12), 0);
        if (fresh) {
            lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, 0);
            lv_obj_set_style_border_width(row, 3, 0);
            lv_obj_set_style_border_color(row, theme.accent, 0);
        }
        lv_obj_t *top = ui_box(row, LV_FLEX_FLOW_ROW, 8);
        lv_obj_t *from = ui_text(top, 13, true, theme.text);
        lv_label_set_text(from, jstr(m, "number") ? jstr(m, "number") : L("未知号码", "Unknown"));
        lv_obj_set_flex_grow(from, 1);
        char text[64];
        meta(m, text, sizeof text);
        lv_label_set_text(ui_text(top, 11, false, theme.dim), text);
        /* One line, as the web list shows it; the message opens in full. */
        lv_obj_t *preview = ui_text(row, 13, false, theme.dim);
        lv_obj_set_size(preview, LV_PCT(100), lv_font_get_line_height(F(13)));
        lv_label_set_long_mode(preview, LV_LABEL_LONG_MODE_DOTS);
        char line[160];
        snprintf(line, sizeof line, "%s", jstr(m, "text") ? jstr(m, "text") : "");
        for (char *c = line; *c; c++)
            if (*c == '\n' || *c == '\r' || *c == '\t')
                *c = ' ';
        lv_label_set_text(preview, line);
        lv_obj_add_event_cb(row, on_open, LV_EVENT_CLICKED, NULL);
        lv_obj_set_user_data(row, (void *)(intptr_t)id);
        ui_focusable(row);
    }
}

static void disarm(void)
{
    lv_timer_pause(disarm_timer);
    lv_obj_set_style_bg_color(ui.delete, theme.danger, 0);
    ui_button_text(ui.delete, L("删除", "Delete"));
    lv_obj_set_user_data(ui.delete, NULL);
}

static void disarm_tick(lv_timer_t *t)
{
    disarm();
}

static void show_view(bool on)
{
    lv_obj_set_hidden(ui.list, on);
    lv_obj_set_hidden(ui.view, !on);
    lv_obj_scroll_to_y(lv_obj_get_parent(ui.list), 0, LV_ANIM_OFF);
}

static void paint_view(const cJSON *m)
{
    char text[64];
    meta(m, text, sizeof text);
    ui_set_text(ui.time, text);
    ui_set_text(ui.text, jstr(m, "text") ? jstr(m, "text") : "");
}

static void open_message(double id)
{
    const cJSON *m = find(id);
    if (!m)
        return;
    open_id = id;
    ui_set_text(ui.from, jstr(m, "number") ? jstr(m, "number") : L("未知号码", "Unknown"));
    paint_view(m);
    disarm();
    show_view(true);
}

static void close_message(void)
{
    open_id = -1;
    disarm();
    show_view(false);
    render_list();
}

static void on_open(lv_event_t *e)
{
    open_message((double)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target_obj(e)));
}

static void on_back(lv_event_t *e)
{
    close_message();
}

static void read_done(int status, cJSON *r, void *ctx)
{
    reading = false;
    cJSON *acknowledged = ctx;
    if (!api_reply_ok(r)) {
        cJSON_Delete(acknowledged);
        return;
    }
    cJSON *unread = (cJSON *)jget(app.status, "sms.unread");
    for (int i = cJSON_GetArraySize(unread) - 1; i >= 0; i--)
        if (id_in(acknowledged, cJSON_GetArrayItem(unread, i)->valuedouble))
            cJSON_DeleteItemFromArray(unread, i);
    cJSON *m;
    cJSON_ArrayForEach(m, messages)
        if (id_in(acknowledged, jnum_or(m, "id", -1)))
            cJSON_ReplaceItemInObject(m, "unread", cJSON_CreateFalse());
    cJSON_Delete(acknowledged);
}

static void mark_read(void)
{
    const cJSON *unread = jget(app.status, "sms.unread");
    if (app.blank || lock_active() || reading || !cJSON_GetArraySize(unread))
        return;
    reading = true;
    api_post("/sms-read", NULL, read_done, cJSON_Duplicate(unread, true));
}

static void loaded(int status, cJSON *r, void *ctx)
{
    shell_page_loading(PAGE_SMS, false);
    bool ok = api_read_ok(status, r);
    shell_page_error(PAGE_SMS, !ok);
    if (ok) {
        cJSON_Delete(messages);
        messages = cJSON_Duplicate(jget(r, "messages"), true);
        if (!messages)
            messages = cJSON_CreateArray();
        if (open_id < 0)
            render_list();
    }
    if (open_id >= 0) {
        const cJSON *current = find(open_id);
        if (current)
            paint_view(current);
        else
            close_message();
    }
    double fresh = (double)(intptr_t)ctx;
    if (fresh >= 0 && find(fresh)) {
        open_message(fresh);
        mark_read();
    }
}

static void load(double open_after)
{
    shell_page_loading(PAGE_SMS, true);
    api_get("/sms", loaded, (void *)(intptr_t)open_after);
}

static void deleted(int status, cJSON *r, void *ctx)
{
    if (!api_reply_ok(r)) {
        toast("✗");
        return;
    }
    toast("%s", L("已删除", "Deleted"));
    double id = (double)(intptr_t)ctx;
    for (int i = cJSON_GetArraySize(messages) - 1; i >= 0; i--)
        if (jnum_or(cJSON_GetArrayItem(messages, i), "id", -2) == id)
            cJSON_DeleteItemFromArray(messages, i);
    close_message();
    load(-1);
}

static void on_delete(lv_event_t *e)
{
    if (!lv_obj_get_user_data(ui.delete)) {
        lv_obj_set_user_data(ui.delete, (void *)1);
        ui_button_text(ui.delete, L("再按一次删除", "Press again to delete"));
        lv_obj_set_style_bg_color(ui.delete, theme.poor, 0);
        lv_timer_reset(disarm_timer);
        lv_timer_resume(disarm_timer);
        return;
    }
    double id = open_id;
    disarm();
    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "id", id);
    api_post("/sms-delete", body, deleted, (void *)(intptr_t)id);
}

/* A new unread message lights the screen (if enabled) and opens it; a wake
 * may light the lock, but cannot expose the inbox. */
void sms_check(const cJSON *st)
{
    const char *revision = jstr(st, "sms.hidden_revision");
    bool hidden_changed = revision && *hidden_revision && strcmp(revision, hidden_revision);
    snprintf(hidden_revision, sizeof hidden_revision, "%s", revision ? revision : "");
    bool here = app.page == PAGE_SMS && !app.blank;
    if (hidden_changed && here)
        load(-1);

    const cJSON *now = jget(st, "sms.unread");
    cJSON *before = unread_snapshot;
    bool had = has_unread_snapshot;
    unread_snapshot = now ? cJSON_Duplicate(now, true) : cJSON_CreateArray();
    has_unread_snapshot = true;
    if (!had) {
        cJSON_Delete(before);
        return;
    }
    double fresh = -1;
    const cJSON *id;
    cJSON_ArrayForEach(id, now)
        if (cJSON_IsNumber(id) && !id_in(before, id->valuedouble))
            fresh = id->valuedouble;
    int before_count = cJSON_GetArraySize(before);
    cJSON_Delete(before);
    if (fresh < 0) {
        const cJSON *current = find(open_id);
        const char *state = jstr(current, "state");
        if (here && ((open_id < 0 && cJSON_GetArraySize(now) != before_count) || (state && !strcmp(state, "receiving"))))
            load(-1);
        return;
    }
    if (!jtrue(st, "sms.screen"))
        return;
    if (app.blank)
        screen_set_blank(false);
    if (!lock_active() && app.page != PAGE_SMS)
        shell_show(PAGE_SMS);
    toast("%s", L("新短信", "New message"));
    load(fresh);
}

static void create(lv_obj_t *page)
{
    ui.list = ui_box(page, LV_FLEX_FLOW_COLUMN, ui_stack_gap());
    ui.view = ui_box(page, LV_FLEX_FLOW_COLUMN, ui_stack_gap());
    lv_obj_t *card = ui_card(ui.view);
    lv_obj_t *head = ui_box(card, LV_FLEX_FLOW_ROW, 8);
    ui.from = ui_text(head, 15, true, theme.text);
    lv_obj_set_flex_grow(ui.from, 1);
    ui.time = ui_text(head, 11, false, theme.dim);
    ui.text = ui_text(card, 14, false, theme.text);
    lv_obj_set_width(ui.text, LV_PCT(100));
    lv_obj_set_style_text_line_space(ui.text, S(4), 0);
    lv_label_set_long_mode(ui.text, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_t *actions = ui_card(ui.view);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(actions, S(12), 0);
    lv_obj_set_flex_grow(ui_button(actions, L("返回", "Back"), on_back, NULL), 1);
    ui.delete = ui_button(actions, "", on_delete, NULL);
    lv_obj_set_flex_grow(ui.delete, 1);
    ui_note(ui.view, L("删除本地记录，与网页同步；SIM 原件保留。", "Deletes the shared local record; keeps the SIM copy."));
    if (!disarm_timer) {
        disarm_timer = lv_timer_create(disarm_tick, 3000, NULL);
        lv_timer_pause(disarm_timer);
    }
    disarm();
    if (messages)
        render_list();
    if (open_id >= 0 && find(open_id))
        open_message(open_id);
    else
        show_view(false);
}

static void shown(void)
{
    load(-1);
    mark_read();
}

static void hidden(void)
{
    if (open_id >= 0)
        close_message();
}

static bool back(void)
{
    if (open_id < 0)
        return false;
    close_message();
    return true;
}

const struct page_ops sms_page = {
    .icon = "sms", .zh = "短信", .en = "Messages",
    .create = create, .shown = shown, .hidden = hidden, .back = back, .top = hidden,
};
