/* Battery runtime, the estimate and the charge history, from the web page's
 * unisoc-battery-history service. */
#include "battery.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "api.h"
#include "app.h"
#include "theme.h"
#include "ui.h"
#include "util.h"

#define POINTS 96
#define REFRESH_MS 10000

static const struct {
    const char *id, *zh, *en, *short_zh, *short_en;
} ranges[] = {
    {"full", "自上次充满", "Since last full", "充满后", "Full"},
    {"charge", "自上次充电", "Since last charge", "充电后", "Charge"},
    {"24h", "24 小时", "24 hours", "24 小时", "24 h"},
    {"7d", "7 天", "7 days", "7 天", "7 d"},
};
#define RANGES ((int)(sizeof ranges / sizeof *ranges))

static struct {
    lv_obj_t *view, *home, *notice;
    lv_obj_t *pct, *state, *detail;
    struct ui_meter meter;
    lv_obj_t *time, *time_note, *estimate, *estimate_note;
    lv_obj_t *ranges, *tip, *chart, *start, *end;
} ui;

static struct {
    int range, n, selected;
    unsigned generation;
    /* Enter has handed the arrow keys to the range choices. */
    bool busy, keyed;
    double t0, step;
    float pct[POINTS], mw[POINTS], max_mw;
    char st[POINTS];
} hist = {.selected = -1};

static lv_timer_t *refresher;

static const char *state_text(char st)
{
    switch (st) {
    case 'D': return L("使用电池", "On battery");
    case 'C': return L("正在充电", "Charging");
    case 'F': return L("已充满", "Full");
    case 'P': return L("已连接电源，未充电", "Plugged in, not charging");
    default: return L("暂无记录", "No record");
    }
}

static void duration(const cJSON *seconds, lv_obj_t *label)
{
    if (!cJSON_IsNumber(seconds) || seconds->valuedouble < 0) {
        ui_set_text(label, "—");
        return;
    }
    int s = (int)seconds->valuedouble;
    ui_set_fmt(label, L("%d 小时 %d 分钟", "%d h %d min"), s / 3600, s % 3600 / 60);
}

static void stamp(double t, char *out, size_t size)
{
    time_t at = (time_t)t;
    struct tm tm;
    localtime_r(&at, &tm);
    strftime(out, size, "%m-%d %H:%M", &tm);
}

static void paint_tip(void)
{
    char from[16], to[16];
    if (!hist.n) {
        ui_set_text(ui.tip, L("暂无历史记录，采集后自动显示", "No history yet; it appears once recorded"));
        return;
    }
    if (hist.selected < 0) {
        stamp(hist.t0, from, sizeof from);
        stamp(hist.t0 + hist.step * (hist.n - 1), to, sizeof to);
        ui_set_fmt(ui.tip, "%s — %s", from, to);
        return;
    }
    int i = hist.selected;
    char pct[8] = "—", power[16] = "";
    stamp(hist.t0 + hist.step * i, from, sizeof from);
    if (!isnan(hist.pct[i]))
        snprintf(pct, sizeof pct, "%.0f%%", hist.pct[i]);
    if (!isnan(hist.mw[i]))
        snprintf(power, sizeof power, " · %.2f W", hist.mw[i] / 1000);
    ui_set_fmt(ui.tip, "%s · %s · %s%s", from, pct, state_text(hist.st[i]), power);
}

static void read_series(const cJSON *r, const char *key, float *out)
{
    const cJSON *a = jget(r, key);
    for (int i = 0; i < hist.n; i++) {
        const cJSON *v = cJSON_GetArrayItem(a, i);
        out[i] = cJSON_IsNumber(v) ? (float)v->valuedouble : NAN;
    }
}

static void paint(const cJSON *r)
{
    const cJSON *c = jget(r, "current");
    double v;
    int pct = jnum(c, "pct", &v) ? (int)v : -1;
    if (pct >= 0)
        ui_set_fmt(ui.pct, "%d%%", pct);
    else
        ui_set_text(ui.pct, "—");
    ui_meter_set(&ui.meter, "", LV_CLAMP(0, pct, 100), theme.good);
    const char *st = jstr(c, "st");
    ui_set_text(ui.state, st ? state_text(*st) : "—");
    char detail[96];
    size_t len = 0;
    if (jnum(c, "mw", &v))
        len += (size_t)snprintf(detail + len, sizeof detail - len, "%.2f W", fabs(v) / 1000);
    if (jnum(c, "ma", &v))
        len += (size_t)snprintf(detail + len, sizeof detail - len, "%s%s%.0f mA", len ? " · " : "", v > 0 ? "+" : "", v);
    if (jnum(c, "mv", &v))
        len += (size_t)snprintf(detail + len, sizeof detail - len, "%s%.2f V", len ? " · " : "", v / 1000);
    if (jnum(c, "temp", &v))
        snprintf(detail + len, sizeof detail - len, "%s%.1f °C", len ? " · " : "", v / 10);
    ui_set_text(ui.detail, len ? detail : "—");

    duration(jget(r, "stats.on_battery_s"), ui.time);
    ui_set_text(ui.time_note, L(ranges[hist.range].zh, ranges[hist.range].en));
    const cJSON *estimate = jget(r, "estimate_s");
    duration(estimate, ui.estimate);
    ui_set_text(ui.estimate_note, jtrue(c, "online")       ? L("连接电源时不估算", "Not estimated on power")
                                  : cJSON_IsNull(estimate) ? L("放电记录不足", "Not enough discharge yet")
                                                           : L("按最近放电速度", "From the recent drain"));

    hist.n = cJSON_IsNumber(jget(r, "first")) ? LV_MIN(cJSON_GetArraySize(jget(r, "pct")), POINTS) : 0;
    hist.t0 = jnum_or(r, "t0", 0);
    hist.step = jnum_or(r, "step", 0);
    read_series(r, "pct", hist.pct);
    read_series(r, "mw", hist.mw);
    hist.max_mw = 1000;
    const cJSON *states = jget(r, "st");
    for (int i = 0; i < hist.n; i++) {
        const cJSON *item = cJSON_GetArrayItem(states, i);
        hist.st[i] = cJSON_IsString(item) ? *item->valuestring : 0;
        if (!isnan(hist.mw[i]))
            hist.max_mw = fmaxf(hist.max_mw, ceilf(hist.mw[i] / 1000) * 1000);
    }
    if (hist.selected >= hist.n)
        hist.selected = -1;
    char at[16];
    stamp(hist.t0, at, sizeof at);
    ui_set_text(ui.start, hist.n ? at : "");
    stamp(hist.t0 + hist.step * (hist.n ? hist.n - 1 : 0), at, sizeof at);
    ui_set_text(ui.end, hist.n ? at : "");
    paint_tip();
    lv_obj_invalidate(ui.chart);
}

static void loaded(int status, cJSON *r, void *ctx)
{
    if ((unsigned)(uintptr_t)ctx != hist.generation)
        return;
    hist.busy = false;
    if (status != 200 || !cJSON_IsObject(r) || jget(r, "error")) {
        ui_set_text(ui.notice, L("电池记录读取失败，请稍后重试", "The battery record could not be read; try again later"));
        lv_obj_set_hidden(ui.notice, false);
        return;
    }
    bool present = jtrue(r, "present");
    lv_obj_set_hidden(ui.notice, present);
    if (!present) {
        ui_set_text(ui.notice, L("未检测到电池", "No battery found"));
        return;
    }
    paint(r);
}

static void fetch(void)
{
    if (hist.busy)
        return;
    hist.busy = true;
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "range", ranges[hist.range].id);
    cJSON_AddNumberToObject(args, "points", POINTS);
    api_ubus("unisoc-battery-history", "history", args, loaded, (void *)(uintptr_t)hist.generation);
}

static void refresh(lv_timer_t *t)
{
    if (!app.blank)
        fetch();
}

static void paint_ranges(void)
{
    ui_choices_paint(ui.ranges, hist.range, hist.keyed);
}

static void choose(int range)
{
    if (range == hist.range)
        return;
    hist.range = range;
    hist.selected = -1;
    /* A reply for the range before is dropped. */
    hist.generation++;
    hist.busy = false;
    fetch();
}

static void on_range(lv_event_t *e)
{
    hist.keyed = false;
    choose((int)(intptr_t)lv_event_get_user_data(e));
    paint_ranges();
}

bool battery_key(const struct key_event *key)
{
    if (!battery_is_open())
        return false;
    if (key->kind == KEY_KIND_OK) {
        if (!key->repeat) {
            hist.keyed = !hist.keyed;
            paint_ranges();
        }
        return true;
    }
    int dir = key->kind == KEY_KIND_LEFT ? -1 : key->kind == KEY_KIND_RIGHT ? 1 : 0;
    if (!hist.keyed || !dir)
        return false;
    choose((hist.range + dir + RANGES) % RANGES);
    paint_ranges();
    return true;
}

/* A tap works the choices directly, so the keys go back to scrolling. */
void battery_touch(void)
{
    if (!hist.keyed)
        return;
    hist.keyed = false;
    paint_ranges();
}

static int32_t chart_x(const lv_area_t *a, int i)
{
    return a->x1 + (hist.n > 1 ? (int32_t)((int64_t)i * (lv_area_get_width(a) - 1) / (hist.n - 1)) : 0);
}

static void line(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2, lv_color_t color, int32_t width,
                 lv_opa_t opa)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = color;
    d.width = width;
    d.opa = opa;
    d.round_start = d.round_end = 1;
    d.p1 = (lv_point_precise_t){x1, y1};
    d.p2 = (lv_point_precise_t){x2, y2};
    lv_draw_line(layer, &d);
}

/* Charge in the upper band over green for charging and grey for power without
 * charging; the drain's power in the strip below. */
static void draw_chart(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_content_coords(lv_event_get_target_obj(e), &a);
    int32_t strip = S(26), top = a.y1 + S(4), base = a.y2 - strip - S(8), floor = a.y2;
    for (int v = 0; v <= 100; v += 50) {
        int32_t y = base - v * (base - top) / 100;
        line(layer, a.x1, y, a.x2, y, theme.line, 1, LV_OPA_COVER);
    }
    line(layer, a.x1, floor, a.x2, floor, theme.line, 1, LV_OPA_COVER);
    for (int i = 0, end; i < hist.n; i = end) {
        bool plugged = hist.st[i] == 'P', charging = hist.st[i] == 'C' || hist.st[i] == 'F';
        for (end = i + 1; end < hist.n; end++)
            if ((hist.st[end] == 'P') != plugged || (hist.st[end] == 'C' || hist.st[end] == 'F') != charging)
                break;
        if (!plugged && !charging)
            continue;
        lv_draw_rect_dsc_t band;
        lv_draw_rect_dsc_init(&band);
        band.bg_color = plugged ? theme.dim : theme.good;
        band.bg_opa = LV_OPA_20;
        lv_area_t at = {chart_x(&a, i), top, end < hist.n ? chart_x(&a, end) : a.x2, base};
        lv_draw_rect(layer, &band, &at);
    }
    lv_color_t drain = lv_color_hex(0xc28a22);
    for (int i = 1; i < hist.n; i++) {
        int32_t x0 = chart_x(&a, i - 1), x1 = chart_x(&a, i);
        if (!isnan(hist.pct[i - 1]) && !isnan(hist.pct[i]))
            line(layer, x0, base - (int32_t)(hist.pct[i - 1] * (base - top) / 100), x1,
                 base - (int32_t)(hist.pct[i] * (base - top) / 100), theme.good, S(2), LV_OPA_COVER);
        if (!isnan(hist.mw[i - 1]) && !isnan(hist.mw[i]))
            line(layer, x0, floor - (int32_t)(hist.mw[i - 1] * strip / hist.max_mw), x1,
                 floor - (int32_t)(hist.mw[i] * strip / hist.max_mw), drain, 1, LV_OPA_COVER);
    }
    if (hist.selected >= 0) {
        int32_t x = chart_x(&a, hist.selected);
        line(layer, x, top, x, floor, theme.text, 1, LV_OPA_60);
    }
    lv_draw_label_dsc_t text;
    lv_draw_label_dsc_init(&text);
    text.color = theme.dim;
    text.font = F(9);
    /* The labels are drawn after this returns, by then from a copy. */
    text.text_local = 1;
    char label[16];
    for (int v = 0; v <= 100; v += 50) {
        snprintf(label, sizeof label, "%d%%", v);
        text.text = label;
        int32_t y = base - v * (base - top) / 100, h = lv_font_get_line_height(text.font);
        /* Inside the chart: under the top line, over the others. */
        lv_area_t at = {a.x1 + S(2), v == 100 ? y + 1 : y - h, a.x1 + S(40), v == 100 ? y + h : y - 1};
        lv_draw_label(layer, &text, &at);
    }
    snprintf(label, sizeof label, "%.1f W", hist.max_mw / 1000);
    text.text = label;
    text.color = drain;
    lv_area_t at = {a.x1 + S(2), floor - strip, a.x1 + S(60), floor - strip + S(12)};
    lv_draw_label(layer, &text, &at);
}

static void on_chart_touch(lv_event_t *e)
{
    if (!hist.n)
        return;
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    lv_area_t a;
    lv_obj_get_content_coords(ui.chart, &a);
    int i = (int)lroundf((float)(p.x - a.x1) * (hist.n - 1) / LV_MAX(1, lv_area_get_width(&a) - 1));
    i = LV_CLAMP(0, i, hist.n - 1);
    if (i == hist.selected)
        return;
    hist.selected = i;
    paint_tip();
    lv_obj_invalidate(ui.chart);
}

static void figure(lv_obj_t *parent, const char *title, lv_obj_t **value, lv_obj_t **note)
{
    lv_obj_t *col = ui_box(parent, LV_FLEX_FLOW_COLUMN, 2);
    lv_obj_set_width(col, 0);
    lv_obj_set_flex_grow(col, 1);
    lv_label_set_text(ui_text(col, 11, false, theme.dim), title);
    *value = ui_text(col, 16, true, theme.text);
    lv_label_set_text_static(*value, "—");
    *note = ui_text(col, 10, false, theme.dim);
}

void battery_create(lv_obj_t *parent, lv_obj_t *home)
{
    memset(&ui, 0, sizeof ui);
    ui.home = home;
    ui.view = ui_box(parent, LV_FLEX_FLOW_COLUMN, ui_stack_gap());
    lv_obj_set_hidden(ui.view, true);
    ui_heading(ui.view, L("电池与电量", "Battery"));
    ui.notice = ui_note(ui.view, "");
    lv_obj_set_hidden(ui.notice, true);

    lv_obj_t *now = ui_card(ui.view);
    lv_obj_t *head = ui_box(now, LV_FLEX_FLOW_ROW, 8);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    ui.pct = ui_text(head, 28, true, theme.text);
    lv_label_set_text_static(ui.pct, "—");
    ui.state = ui_text(head, 13, false, theme.good);
    ui.meter = ui_meter(now, "");
    lv_obj_set_hidden(lv_obj_get_parent(ui.meter.value), true);
    ui.detail = ui_text(now, 11, false, theme.dim);

    lv_obj_t *figures = ui_card(ui.view);
    lv_obj_set_flex_flow(figures, LV_FLEX_FLOW_ROW);
    figure(figures, L("电池使用时间", "On battery"), &ui.time, &ui.time_note);
    figure(figures, L("预估续航", "Estimated"), &ui.estimate, &ui.estimate_note);

    lv_obj_t *history = ui_card(ui.view);
    lv_label_set_text(ui_text(history, 13, true, theme.text), L("电池历史", "History"));
    ui.ranges = ui_choices(history);
    for (int i = 0; i < RANGES; i++)
        ui_choice(ui.ranges, L(ranges[i].short_zh, ranges[i].short_en), on_range, (void *)(intptr_t)i);
    hist.keyed = false;
    paint_ranges();
    ui.tip = ui_text(history, 11, false, theme.dim);
    lv_obj_set_width(ui.tip, LV_PCT(100));
    lv_label_set_long_mode(ui.tip, LV_LABEL_LONG_MODE_WRAP);
    ui.chart = lv_obj_create(history);
    lv_obj_remove_style_all(ui.chart);
    lv_obj_set_size(ui.chart, LV_PCT(100), S(150));
    lv_obj_add_event_cb(ui.chart, draw_chart, LV_EVENT_DRAW_MAIN_END, NULL);
    lv_obj_add_event_cb(ui.chart, on_chart_touch, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(ui.chart, on_chart_touch, LV_EVENT_PRESSING, NULL);
    lv_obj_t *axis = ui_box(history, LV_FLEX_FLOW_ROW, 0);
    lv_obj_set_flex_align(axis, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ui.start = ui_text(axis, 10, false, theme.dim);
    ui.end = ui_text(axis, 10, false, theme.dim);
    lv_obj_t *note = ui_note(history, L("绿底为充电或已充满，灰底为接电未充电，橙线为电池放电功率；按住图表查看某一时刻。每分钟记录，保留最近 10 天。",
                                        "Green: charging or full; grey: plugged in, not charging; orange: the battery's drain. Hold the chart to read a moment. Recorded each minute for 10 days."));
    lv_obj_set_style_pad_hor(note, 0, 0);
    hist.n = 0;
    paint_tip();
}

void battery_open(void)
{
    lv_obj_set_hidden(ui.home, true);
    lv_obj_set_hidden(ui.view, false);
    lv_obj_scroll_to_y(lv_obj_get_parent(ui.view), 0, LV_ANIM_OFF);
    hist.selected = -1;
    fetch();
    if (!refresher)
        refresher = lv_timer_create(refresh, REFRESH_MS, NULL);
    lv_timer_resume(refresher);
}

bool battery_close(void)
{
    if (!battery_is_open())
        return false;
    lv_obj_set_hidden(ui.view, true);
    lv_obj_set_hidden(ui.home, false);
    lv_timer_pause(refresher);
    hist.keyed = false;
    paint_ranges();
    return true;
}

bool battery_is_open(void)
{
    return ui.view && !lv_obj_is_hidden(ui.view);
}
