#include <math.h>
#include <stdio.h>
#include <string.h>

#include "api.h"
#include "app.h"
#include "theme.h"
#include "ui.h"
#include "util.h"

struct span {
    lv_obj_t *used, *rx, *tx, *since;
};

static struct {
    lv_obj_t *choices[3], *row, *state;
    struct span day, month;
    lv_obj_t *boot_rx, *boot_tx, *days;
} ui;

static int slot = -1;
static bool locked;
static unsigned generation;
static lv_timer_t *refresh;

static void amount(lv_obj_t *label, const cJSON *object, const char *path)
{
    double v;
    char text[32] = "—";
    if (jnum(object, path, &v))
        fmt_amount(v, text, sizeof text);
    ui_set_text(label, text);
}

static void clear_display(void)
{
    struct span *spans[2] = {&ui.day, &ui.month};
    for (int i = 0; i < 2; i++) {
        ui_set_text(spans[i]->used, "—");
        ui_set_text(spans[i]->rx, "—");
        ui_set_text(spans[i]->tx, "—");
        ui_set_text(spans[i]->since, "");
        lv_obj_set_hidden(spans[i]->since, true);
    }
    ui_set_text(ui.boot_rx, "—");
    ui_set_text(ui.boot_tx, "—");
    lv_obj_clean(ui.days);
}

static void paint_choices(void)
{
    for (int i = 0; i < 3; i++) {
        bool on = i == slot + 1;
        lv_obj_set_style_bg_color(ui.choices[i], on ? theme.active : theme.button, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(ui.choices[i], 0), on ? theme.accent : theme.text, 0);
    }
    /* The outline marks that the arrow keys drive the selector. */
    lv_obj_set_style_outline_width(ui.row, locked ? 2 : 0, 0);
}

static void since_text(const cJSON *u, struct span *day, struct span *month)
{
    double since;
    if (!jnum(u, "since", &since))
        return;
    struct local_time s = local_at(since), now = local_now();
    char text[64];
    snprintf(text, sizeof text, "%s %02d-%02d %02d:%02d", L("开始统计于", "Counting since"), s.month, s.day, s.hour,
             s.minute);
    bool same_month = s.year == now.year && s.month == now.month, same_day = same_month && s.day == now.day;
    ui_set_text(month->since, text);
    lv_obj_set_hidden(month->since, !same_month);
    ui_set_text(day->since, text);
    lv_obj_set_hidden(day->since, !same_day);
}

static void render_days(const cJSON *days)
{
    lv_obj_clean(ui.days);
    int count = cJSON_GetArraySize(days);
    if (!count) {
        lv_label_set_text(ui_text(ui.days, 12, false, theme.dim), L("无", "None"));
        return;
    }
    double max = 1;
    const cJSON *d;
    cJSON_ArrayForEach(d, days)
        max = fmax(max, jnum_or(d, "rx", 0) + jnum_or(d, "tx", 0));
    for (int i = count - 1; i >= 0; i--) {
        d = cJSON_GetArrayItem(days, i);
        double rx = jnum_or(d, "rx", 0), tx = jnum_or(d, "tx", 0);
        lv_obj_t *row = ui_box(ui.days, LV_FLEX_FLOW_ROW, 8);
        lv_obj_t *date = ui_text(row, 11, false, theme.dim);
        lv_label_set_text(date, jstr_or(d, "date", ""));
        lv_obj_set_width(date, S(70));
        lv_obj_t *bar = ui_box(row, LV_FLEX_FLOW_ROW, 0);
        lv_obj_set_flex_grow(bar, 1);
        lv_obj_set_height(bar, S(8));
        lv_obj_set_style_bg_color(bar, theme.line, 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(bar, S(4), 0);
        lv_obj_set_style_clip_corner(bar, true, 0);
        double parts[2] = {rx, tx};
        for (int p = 0; p < 2; p++) {
            lv_obj_t *fill = lv_obj_create(bar);
            lv_obj_remove_style_all(fill);
            lv_obj_set_size(fill, LV_PCT((int)lround(parts[p] / max * 100)), LV_PCT(100));
            lv_obj_set_style_bg_color(fill, theme.accent, 0);
            lv_obj_set_style_bg_opa(fill, p ? LV_OPA_50 : LV_OPA_COVER, 0);
        }
        char sum[32];
        fmt_amount(rx + tx, sum, sizeof sum);
        lv_obj_t *total = ui_text(row, 11, false, theme.text);
        lv_label_set_text(total, sum);
        lv_obj_set_width(total, S(64));
        lv_obj_set_style_text_align(total, LV_TEXT_ALIGN_RIGHT, 0);
    }
}

static void loaded(int status, cJSON *u, void *ctx)
{
    shell_page_loading(PAGE_TRAFFIC, false);
    if ((unsigned)(uintptr_t)ctx != generation)
        return;
    bool ok = api_read_ok(status, u);
    shell_page_error(PAGE_TRAFFIC, !ok);
    clear_display();
    const char *warning = !ok || jhas(u, "error") ? L("流量统计暂不可用", "Usage unavailable")
                        : !jtrue(u, "available")       ? L("系统时间尚未同步", "Clock not synchronized")
                        : jtrue(u, "storage_error")    ? L("流量记录保存失败", "Could not save usage")
                        : jfalse(u, "interface_available") ? L("网卡离线 · 历史记录仍可查看", "Interface offline · History is available")
                                                           : "";
    double s;
    if (jnum(u, "slot", &s))
        ui_set_fmt(ui.state, "SIM %d%s%s", (int)s + 1, *warning ? " · " : "", warning);
    else
        ui_set_text(ui.state, warning);
    amount(ui.boot_rx, u, "session.rx");
    amount(ui.boot_tx, u, "session.tx");
    if (!jtrue(u, "available"))
        return;
    amount(ui.day.used, u, "today_used");
    amount(ui.month.used, u, "month_used");
    amount(ui.day.rx, u, "today.rx");
    amount(ui.day.tx, u, "today.tx");
    amount(ui.month.rx, u, "month.rx");
    amount(ui.month.tx, u, "month.tx");
    since_text(u, &ui.day, &ui.month);
    render_days(jget(u, "days"));
}

static void load(void)
{
    char path[32] = "/traffic";
    if (slot >= 0)
        snprintf(path, sizeof path, "/traffic?slot=%d", slot);
    shell_page_loading(PAGE_TRAFFIC, true);
    api_get(path, loaded, (void *)(uintptr_t)++generation);
}

static void choose(int choice)
{
    if (choice == slot)
        return;
    slot = choice;
    paint_choices();
    clear_display();
    load();
}

static void on_choice(lv_event_t *e)
{
    locked = false;
    choose((int)(intptr_t)lv_event_get_user_data(e) - 1);
}

/* The arrow keys scroll this page; Enter hands the selector the left/right ones,
 * which match the row it is drawn around. */
static bool key(const struct key_event *key)
{
    if (key->kind == KEY_KIND_OK) {
        if (key->repeat)
            return true;
        locked = !locked;
        paint_choices();
        return true;
    }
    int dir = key->kind == KEY_KIND_LEFT ? -1 : key->kind == KEY_KIND_RIGHT ? 1 : 0;
    if (!locked || !dir)
        return false;
    choose((slot + 1 + dir + 3) % 3 - 1);
    return true;
}

/* A tap works the selector directly, so the keys go back to scrolling. */
static void touch(void)
{
    if (!locked)
        return;
    locked = false;
    paint_choices();
}

void traffic_invalidate(void)
{
    generation++;
    clear_display();
    lock_refresh_usage();
}

static lv_obj_t *span_card(lv_obj_t *page, const char *title, struct span *span)
{
    lv_obj_t *card = ui_card(page);
    lv_label_set_text(ui_text(card, 12, false, theme.dim), title);
    span->used = ui_text(card, 24, true, theme.text);
    lv_obj_t *line = ui_box(card, LV_FLEX_FLOW_ROW, 6);
    lv_label_set_text(ui_text(line, 12, false, theme.dim), L("外网", "WAN"));
    lv_label_set_text(ui_text(line, 13, false, theme.accent), "↓");
    span->rx = ui_text(line, 13, false, theme.text);
    lv_label_set_text(ui_text(line, 13, false, theme.accent), "↑");
    span->tx = ui_text(line, 13, false, theme.text);
    span->since = ui_text(card, 10, false, theme.dim);
    return card;
}

static void create(lv_obj_t *page)
{
    lv_obj_t *scope = ui_card(page);
    lv_label_set_text(ui_text(scope, 12, false, theme.dim), L("查看流量", "View usage"));
    lv_obj_t *choices = ui_box(scope, LV_FLEX_FLOW_ROW, 6);
    const char *labels[3] = {L("跟随数据卡", "Follow data SIM"), "SIM 1", "SIM 2"};
    for (int i = 0; i < 3; i++) {
        ui.choices[i] = ui_button(choices, labels[i], on_choice, (void *)(intptr_t)i);
        lv_obj_set_flex_grow(ui.choices[i], 1);
        lv_obj_set_style_pad_hor(ui.choices[i], S(4), 0);
        lv_obj_set_style_text_font(lv_obj_get_child(ui.choices[i], 0), F(12), 0);
        /* Touch-only until Enter hands the arrow keys to the selector. */
        lv_group_remove_obj(ui.choices[i]);
    }
    /* A label too wide for a third of the row (large sizes, English) gets a line
     * of its own above the SIM choices. */
    lv_obj_update_layout(choices);
    lv_obj_t *first = lv_obj_get_child(ui.choices[0], 0);
    if (lv_obj_get_width(first) > lv_obj_get_content_width(ui.choices[0])) {
        lv_obj_set_flex_flow(choices, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_row(choices, S(6), 0);
        lv_obj_set_flex_grow(ui.choices[0], 0);
        lv_obj_set_width(ui.choices[0], LV_PCT(100));
    }
    ui.row = choices;
    lv_obj_set_style_radius(choices, S(11), 0);
    lv_obj_set_style_outline_color(choices, theme.accent, 0);
    lv_obj_set_style_outline_pad(choices, S(4), 0);
    ui.state = ui_text(scope, 11, false, theme.dim);
    span_card(page, L("今日", "Today"), &ui.day);
    span_card(page, L("本月", "This month"), &ui.month);
    lv_obj_t *boot = ui_card(page);
    lv_label_set_text_fmt(ui_text(boot, 12, false, theme.dim), "%s · %s", L("本次开机", "Since boot"), L("外网", "WAN"));
    lv_obj_t *line = ui_box(boot, LV_FLEX_FLOW_ROW, 6);
    lv_label_set_text(ui_text(line, 13, false, theme.accent), "↓");
    ui.boot_rx = ui_text(line, 15, false, theme.text);
    lv_label_set_text(ui_text(line, 13, false, theme.accent), "↑");
    ui.boot_tx = ui_text(line, 15, false, theme.text);
    lv_label_set_text(ui_text(boot, 10, false, theme.dim),
                      L("网卡累计独立计数，清空历史不会重置网卡", "Live interface counters are separate from usage history"));
    lv_obj_t *days = ui_card(page);
    lv_label_set_text_fmt(ui_text(days, 12, false, theme.dim), "%s · %s", L("最近 7 天", "Last 7 days"), L("外网", "WAN"));
    ui.days = ui_box(days, LV_FLEX_FLOW_COLUMN, 6);
    paint_choices();
    clear_display();
}

static void tick(lv_timer_t *t)
{
    if (!app.blank && app.page == PAGE_TRAFFIC)
        load();
}

static void shown(void)
{
    locked = false;
    paint_choices();
    load();
    if (!refresh)
        refresh = lv_timer_create(tick, 30000, NULL);
    lv_timer_resume(refresh);
}

static void hidden(void)
{
    if (refresh)
        lv_timer_pause(refresh);
}

const struct page_ops traffic_page = {
    .icon = "traffic", .zh = "流量", .en = "Usage",
    .create = create, .shown = shown, .hidden = hidden, .key = key, .touch = touch,
};
