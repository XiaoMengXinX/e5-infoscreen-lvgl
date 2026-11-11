#include <math.h>
#include <stdio.h>
#include <string.h>

#include "src/misc/lv_text_private.h"

#include "api.h"
#include "app.h"
#include "battery.h"
#include "icons.h"
#include "theme.h"
#include "ui.h"
#include "util.h"

static const char *const temp_fields[] = {"cpu", "gpu", "soc", "lte", "nr", "mm", "board", "pa"};
static const char *const temp_names[][2] = {
    {"CPU", "CPU"}, {"GPU", "GPU"}, {"SoC", "SoC"}, {"4G 基带", "4G modem"},
    {"5G 基带", "5G modem"}, {"多媒体", "Media"}, {"主板", "Board"}, {"射频功放", "RF PA"},
};

static struct {
    lv_obj_t *uptime, *rx, *rx_unit, *tx, *tx_unit, *total;
    lv_obj_t *boot, *boot_note, *boot_button;
    lv_obj_t *usb, *usb_title, *usb_detail;
    lv_obj_t *update, *update_version, *update_notes, *update_toggle, *update_error, *update_now, *update_later;
    lv_obj_t *wan_card, *wan_word, *wan, *wifi_word, *wifi, *power_word, *power, *clients, *battery, *temps[8];
    struct ui_meter memory, storage;
} ui;

/* When the first of a double press on the network card came. */
static uint32_t wan_pressed;

static struct {
    cJSON *state;
    bool expanded, busy;
    uint32_t polled;
    bool polled_once;
} update;

static uint32_t boot_confirm_until;
static bool boot_busy;

static void boot_done(int status, cJSON *r, void *ctx)
{
    boot_busy = false;
    if (api_reply_ok(r)) {
        lv_obj_set_hidden(ui.boot, true);
        toast("%s", L("已设为默认启动", "Default boot saved"));
    } else {
        lv_obj_remove_state(ui.boot_button, LV_STATE_DISABLED);
        toast("%s", L("设置失败，请重试", "Failed to save; try again"));
    }
}

static void on_boot_default(lv_event_t *e)
{
    if (lock_active() || boot_busy || !jtrue(app.status, "boot.can_set_default"))
        return;
    if (lv_tick_get() > boot_confirm_until) {
        boot_confirm_until = lv_tick_get() + 5000;
        toast("%s", L("再按一次设为默认启动", "Press again to make OpenWrt the default"));
        return;
    }
    boot_confirm_until = 0;
    boot_busy = true;
    lv_obj_add_state(ui.boot_button, LV_STATE_DISABLED);
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "id", "default_boot");
    cJSON_AddStringToObject(body, "value", "linux");
    api_post("/settings/system", body, boot_done, NULL);
}

static void render_update(void)
{
    const cJSON *u = update.state;
    bool busy = jtrue(u, "busy");
    bool hidden = !u || (!busy && (!jtrue(u, "available") || jtrue(u, "deferred")));
    lv_obj_set_hidden(ui.update, hidden);
    if (hidden)
        return;
    ui_set_fmt(ui.update_version, "%s → %s", jstr_or(u, "current", ""),
               jstr_or(u, "latest.version", ""));
    char notes[1024];
    label_text(jget(u, "latest.notes"), notes, sizeof notes);
    ui_set_text(ui.update_notes, *notes ? notes : L("只更新信息屏，设置和应用保留", "Settings and apps stay; the screen only"));
    lv_obj_set_hidden(ui.update_notes, !update.expanded);
    ui_button_text(ui.update_toggle, update.expanded ? L("收起更新日志", "Hide release notes") : L("查看更新日志", "Release notes"));
    const char *action = jstr(u, "action"), *result = jstr(u, "result");
    const char *error = action && !strcmp(action, "apply") && result && !strcmp(result, "failed") ? jstr(u, "error") : NULL;
    char error_buf[256] = "";
    if (error)
        error_text(error, error_buf, sizeof error_buf);
    ui_set_text(ui.update_error, error_buf);
    lv_obj_set_hidden(ui.update_error, !error);
    ui_button_text(ui.update_now, busy ? L("正在更新…", "Updating…") : error ? L("重试更新", "Retry update") : L("立即更新", "Update now"));
    bool disabled = busy || jtrue(u, "checking") || update.busy;
    lv_obj_set_state(ui.update_now, LV_STATE_DISABLED, disabled);
    lv_obj_set_state(ui.update_later, LV_STATE_DISABLED, disabled);
}

static void update_read(int status, cJSON *r, void *ctx)
{
    update.busy = false;
    if (status == 200 && r) {
        const char *old = jstr(update.state, "latest.version"), *now = jstr(r, "latest.version");
        if (!old || !now || strcmp(old, now))
            update.expanded = false;
        cJSON_Delete(update.state);
        update.state = cJSON_Duplicate(r, true);
    }
    render_update();
}

static void poll_update(bool force)
{
    bool fast = jtrue(update.state, "checking") || jtrue(update.state, "busy");
    if (update.busy || (!force && update.polled_once && lv_tick_elaps(update.polled) < (fast ? 2000u : 30000u)))
        return;
    update.polled = lv_tick_get();
    update.polled_once = true;
    update.busy = true;
    api_get(jtrue(app.status, "wan.up") ? "/update?check=1" : "/update", update_read, NULL);
}

static void force_poll(lv_timer_t *t)
{
    poll_update(true);
}

static void update_acted(int status, cJSON *r, void *ctx)
{
    update.busy = false;
    bool apply = (bool)(intptr_t)ctx;
    if (api_reply_ok(r)) {
        cJSON_Delete(update.state);
        update.state = cJSON_Duplicate(r, true);
        if (apply)
            cJSON_ReplaceItemInObject(update.state, "busy", cJSON_CreateTrue());
        else
            toast("%s", L("已推迟更新", "Update postponed"));
    } else {
        char error[256] = "";
        if (jstr(r, "error"))
            error_text(jstr(r, "error"), error, sizeof error);
        toast("%s%s%s", L("失败", "Failed"), *error ? ": " : "", error);
    }
    render_update();
    lv_timer_set_repeat_count(lv_timer_create(force_poll, 500, NULL), 1);
}

static void on_update_action(lv_event_t *e)
{
    bool apply = (bool)(intptr_t)lv_event_get_user_data(e);
    const cJSON *u = update.state;
    if (!u || update.busy || jtrue(u, "busy") || jtrue(u, "checking"))
        return;
    update.busy = true;
    render_update();
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "action", apply ? "apply" : "defer");
    if (jstr(u, "latest.version"))
        cJSON_AddStringToObject(body, "version", jstr(u, "latest.version"));
    api_post("/update", body, update_acted, (void *)(intptr_t)apply);
}

static void on_update_toggle(lv_event_t *e)
{
    update.expanded = !update.expanded;
    render_update();
}

static lv_obj_t *row(lv_obj_t *page, const char *label)
{
    lv_obj_t *card = ui_card(page);
    return ui_kv(card, label);
}

/* A value whose leading word carries the state colour, as the web page's span does.
 * Returns the label for the rest of the value. */
static lv_obj_t *word_row(lv_obj_t *page, const char *label, lv_obj_t **word)
{
    lv_obj_t *rest = row(page, label);
    lv_obj_t *box = ui_box(lv_obj_get_parent(rest), LV_FLEX_FLOW_ROW_WRAP, 0);
    lv_obj_set_width(box, 0);
    lv_obj_set_flex_grow(box, 1);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    *word = ui_text(box, 13, false, theme.text);
    lv_obj_set_parent(rest, box);
    lv_obj_set_flex_grow(rest, 0);
    /* A pixel cap so that a long SSID wraps; see ui_row. */
    lv_obj_set_style_max_width(rest, S(180), 0);
    lv_label_set_text_static(rest, "");
    return rest;
}

static void on_usb(lv_event_t *e)
{
    settings_open_category("usb", "USB", "usb_role");
}

static void poll_status(lv_timer_t *t)
{
    shell_poll_now();
}

/* Two presses within the double-click time, by touch or by OK; the card takes
 * them only while the link is offline. */
static void on_wan(lv_event_t *e)
{
    uint32_t now = lv_tick_get();
    bool twice = wan_pressed && lv_tick_diff(now, wan_pressed) <= LV_INDEV_DEF_DOUBLE_CLICK_TIME;
    wan_pressed = twice ? 0 : now;
    if (!twice)
        return;
    api_post("/wan-reconnect", NULL, NULL, NULL);
    toast("%s", L("正在重新连接", "Reconnecting"));
    /* As the web page does, the status is read again once the link had time to come up. */
    lv_timer_set_repeat_count(lv_timer_create(poll_status, 3000, NULL), 1);
}

static void on_wifi(lv_event_t *e)
{
    shell_show(PAGE_HOTSPOT);
}

static void on_clients(lv_event_t *e)
{
    settings_open_devices();
}

/* A card that opens something on a tap, with the › that says so. */
static void card_action(lv_obj_t *card, lv_obj_t *row, lv_event_cb_t cb)
{
    if (row)
        lv_label_set_text_static(ui_text(row, 13, false, theme.dim), "›");
    lv_obj_set_clickable(card, true);
    lv_obj_set_style_bg_color(card, theme.active, LV_STATE_PRESSED);
    lv_obj_add_event_cb(card, cb, LV_EVENT_CLICKED, NULL);
    ui_focusable(card);
}

static void on_battery(lv_event_t *e)
{
    battery_open();
}

static void create(lv_obj_t *parent)
{
    lv_obj_t *page = ui_box(parent, LV_FLEX_FLOW_COLUMN, ui_stack_gap());
    battery_create(parent, page);
    lv_obj_t *hero = ui_card(page);
    lv_obj_set_style_bg_color(hero, theme.hero_start, 0);
    lv_obj_set_style_bg_grad_color(hero, theme.hero_end, 0);
    lv_obj_set_style_bg_grad_dir(hero, LV_GRAD_DIR_HOR, 0);
    lv_obj_set_style_pad_all(hero, S(14), 0);
    lv_obj_t *head = ui_box(hero, LV_FLEX_FLOW_ROW, 8);
    lv_label_set_text(ui_text(head, 9, false, theme.dim), L("本次运行时长", "Uptime"));
    ui.uptime = ui_text(head, 9, false, theme.accent);
    lv_obj_t *rates = ui_box(hero, LV_FLEX_FLOW_ROW, 8);
    for (int i = 0; i < 2; i++) {
        lv_obj_t *col = ui_box(rates, LV_FLEX_FLOW_COLUMN, 0);
        lv_obj_set_flex_grow(col, 1);
        lv_obj_set_width(col, 0);
        lv_obj_t *line = ui_box(col, LV_FLEX_FLOW_ROW, 4);
        lv_label_set_text(ui_text(line, 20, false, i ? theme.accent : theme.good), i ? "↑" : "↓");
        lv_obj_t *big = ui_text(line, 28, false, theme.text);
        lv_obj_t *unit = ui_text(col, 10, false, theme.dim);
        lv_obj_set_style_pad_left(unit, S(20), 0);
        *(i ? &ui.tx : &ui.rx) = big;
        *(i ? &ui.tx_unit : &ui.rx_unit) = unit;
    }
    lv_obj_t *divider = lv_obj_create(hero);
    lv_obj_remove_style_all(divider);
    lv_obj_set_size(divider, LV_PCT(100), 1);
    lv_obj_set_style_bg_color(divider, theme.text, 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_10, 0);
    lv_obj_t *since = ui_box(hero, LV_FLEX_FLOW_ROW, 6);
    lv_label_set_text(ui_text(since, 10, false, theme.dim), L("本次开机", "Since boot"));
    ui.total = ui_text(since, 10, false, theme.text);

    ui.update = ui_card(page);
    lv_obj_set_style_border_color(ui.update, theme.accent, 0);
    lv_obj_set_style_border_width(ui.update, 1, 0);
    lv_obj_t *uh = ui_box(ui.update, LV_FLEX_FLOW_ROW, 8);
    lv_label_set_text(ui_text(uh, 14, true, theme.text), L("信息屏有更新", "Info screen update"));
    ui.update_version = ui_text(uh, 12, false, theme.dim);
    ui.update_toggle = ui_button(ui.update, "", on_update_toggle, NULL);
    lv_obj_set_style_bg_opa(ui.update_toggle, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_hor(ui.update_toggle, 0, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(ui.update_toggle, 0), theme.accent, 0);
    ui.update_notes = ui_note(ui.update, "");
    lv_obj_set_style_text_font(ui.update_notes, F(12), 0);
    ui.update_error = ui_note(ui.update, "");
    lv_obj_set_style_text_color(ui.update_error, theme.poor, 0);
    lv_obj_t *actions = ui_box(ui.update, LV_FLEX_FLOW_ROW, 8);
    ui.update_now = ui_button(actions, "", on_update_action, (void *)(intptr_t)true);
    ui_primary(ui.update_now);
    ui.update_later = ui_button(actions, L("推迟更新", "Later"), on_update_action, (void *)(intptr_t)false);
    lv_obj_set_flex_grow(ui.update_now, 1);
    lv_obj_set_flex_grow(ui.update_later, 1);
    ui_note(ui.update, L("推迟后 24 小时再提醒", "Remind me in 24 hours"));
    lv_obj_set_hidden(ui.update, true);

    ui.boot = ui_card(page);
    lv_label_set_text(ui_text(ui.boot, 12, true, theme.text), L("当前为临时启动 OpenWrt", "OpenWrt was booted temporarily"));
    ui.boot_note = ui_note(ui.boot, "");
    lv_obj_set_style_pad_hor(ui.boot_note, 0, 0);
    lv_obj_set_style_text_font(ui.boot_note, F(11), 0);
    ui.boot_button = ui_button(ui.boot, L("设为默认启动", "Make default boot"), on_boot_default, NULL);
    lv_obj_set_width(ui.boot_button, LV_PCT(100));
    lv_obj_set_hidden(ui.boot, true);

    ui.usb = ui_card(page);
    lv_obj_set_flex_flow(ui.usb, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ui.usb, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(ui.usb, S(10), 0);
    lv_obj_set_style_text_color(icon_create(ui.usb, "usb", S(22)), theme.accent, 0);
    lv_obj_t *usb_text = ui_box(ui.usb, LV_FLEX_FLOW_COLUMN, 3);
    lv_obj_set_flex_grow(usb_text, 1);
    ui.usb_title = ui_text(usb_text, 12, true, theme.text);
    lv_obj_set_width(ui.usb_title, LV_PCT(100));
    lv_label_set_long_mode(ui.usb_title, LV_LABEL_LONG_MODE_WRAP);
    ui.usb_detail = ui_text(usb_text, 10, false, theme.dim);
    lv_obj_set_hidden(ui.usb, true);
    lv_obj_set_clickable(ui.usb, true);
    lv_obj_set_style_bg_color(ui.usb, theme.active, LV_STATE_PRESSED);
    lv_obj_add_event_cb(ui.usb, on_usb, LV_EVENT_CLICKED, NULL);
    ui_focusable(ui.usb);

    /* A word row's value sits in a box in the card's row. */
    ui.wan = word_row(page, L("网络", "Network"), &ui.wan_word);
    ui.wan_card = lv_obj_get_parent(lv_obj_get_parent(lv_obj_get_parent(ui.wan)));
    card_action(ui.wan_card, NULL, on_wan);
    ui.wifi = word_row(page, L("热点", "Hotspot"), &ui.wifi_word);
    lv_obj_t *wifi_row = lv_obj_get_parent(lv_obj_get_parent(ui.wifi));
    card_action(lv_obj_get_parent(wifi_row), wifi_row, on_wifi);
    ui.clients = row(page, L("在线设备", "Clients"));
    card_action(lv_obj_get_parent(lv_obj_get_parent(ui.clients)), lv_obj_get_parent(ui.clients), on_clients);
    ui.battery = row(page, L("电池", "Battery"));
    card_action(lv_obj_get_parent(lv_obj_get_parent(ui.battery)), lv_obj_get_parent(ui.battery), on_battery);
    ui.power = word_row(page, L("电流 / 电压", "Current / voltage"), &ui.power_word);
    lv_obj_t *usage = ui_card(page);
    ui.memory = ui_meter(usage, L("内存", "Memory"));
    ui.storage = ui_meter(usage, L("存储", "Storage"));
    lv_obj_t *temps = ui_card(page);
    lv_obj_set_flex_flow(temps, LV_FLEX_FLOW_ROW_WRAP);
    for (int i = 0; i < 8; i++) {
        lv_obj_t *cell = ui_box(temps, LV_FLEX_FLOW_ROW, 4);
        lv_obj_set_width(cell, LV_PCT(48));
        lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_label_set_text(ui_text(cell, 12, false, theme.dim), L(temp_names[i][0], temp_names[i][1]));
        ui.temps[i] = ui_text(cell, 12, false, theme.text);
    }
    render_update();
}

static void usage_meter(struct ui_meter *m, double used, double total)
{
    if (!(total > 0) || used < 0) {
        ui_meter_set(m, "--", 0, theme.accent);
        return;
    }
    int pct = (int)fmin(100, lround(used / total * 100));
    char u[24], t[24];
    fmt_bytes(used, u, sizeof u);
    fmt_bytes(total, t, sizeof t);
    char text[64];
    snprintf(text, sizeof text, "%s / %s · %d%%", u, t, pct);
    ui_meter_set(m, text, pct, pct >= 90 ? theme.poor : pct >= 75 ? theme.fair : theme.good);
}

/* True for the plain offline state, the one a reconnect can help. */
static bool wan_word(const cJSON *st, const char **word, lv_color_t *color)
{
    const cJSON *m = jget(st, "modem"), *w = jget(st, "wan");
    const char *state = jstr(m, "state"), *reg = jstr(m, "registration");
    if (!jtrue(m, "present"))
        *word = L("无模组", "No modem"), *color = theme.poor;
    else if (!jtrue(m, "sim"))
        *word = L("无 SIM 卡", "No SIM"), *color = theme.poor;
    else if (jtrue(w, "up"))
        *word = L("已连接", "Connected"), *color = theme.good;
    else if (jtrue(w, "pending") || (state && !strcmp(state, "connecting")))
        *word = L("连接中", "Connecting"), *color = theme.fair;
    else if (reg && !strcmp(reg, "searching"))
        *word = L("搜索网络", "Searching"), *color = theme.fair;
    else {
        *word = L("未连接", "Offline"), *color = theme.poor;
        return true;
    }
    return false;
}

static void render_usb(const cJSON *st)
{
    const cJSON *usb = jget(st, "usb");
    const char *role = jstr(usb, "role"), *link = jstr(usb, "link");
    bool host = role && !strcmp(role, "host");
    int attached = cJSON_GetArraySize(jget(usb, "devices"));
    bool computer = !host && link && (!strcmp(link, "enumerated") || !strcmp(link, "lease") || !strcmp(link, "online"));
    lv_obj_set_hidden(ui.usb, !((host && attached) || computer));
    if (host) {
        char title[256] = "";
        size_t len = 0;
        const cJSON *d;
        cJSON_ArrayForEach(d, jget(usb, "devices")) {
            char name[96];
            usb_device_name(d, name, sizeof name);
            len += (size_t)snprintf(title + len, sizeof title - len, "%s%s%s", len ? "\n" : "", name,
                                    L(" 已连接", " connected"));
            if (len >= sizeof title)
                break;
        }
        ui_set_text(ui.usb_title, title);
        ui_set_fmt(ui.usb_detail, app.en ? "OTG host · %d device(s)" : "OTG 主机 · %d 台设备", attached);
    } else if (computer) {
        ui_set_text(ui.usb_title, L("USB 已连接", "USB connected"));
        ui_set_text(ui.usb_detail, L("设备模式 · 已连接电脑", "Device mode · Computer connected"));
    }
}

/* As many decimals as the column has room for at the display size. */
static void set_rate(lv_obj_t *big, lv_obj_t *unit, double rate, bool bytes)
{
    char num[24], u[24];
    int32_t room = lv_obj_get_content_width(lv_obj_get_parent(lv_obj_get_parent(big))) - lv_obj_get_x(big);
    const lv_font_t *font = lv_obj_get_style_text_font(big, 0);
    lv_text_attributes_t attributes = {.letter_space = lv_obj_get_style_text_letter_space(big, 0)};
    for (int decimals = 2;; decimals--) {
        fmt_speed(rate, bytes, decimals, num, u, sizeof num);
        if (!decimals || room <= 0 || lv_text_get_width(num, strlen(num), font, &attributes) <= room)
            break;
    }
    ui_set_text(big, num);
    ui_set_text(unit, u);
}

static void status(const cJSON *st)
{
    char a[48], b[48], text[256];
    bool bytes = jstr_is(st, "screen.speed_unit", "bytes");
    double v;
    set_rate(ui.rx, ui.rx_unit, jnum(st, "traffic.rx_rate", &v) ? v : NAN, bytes);
    set_rate(ui.tx, ui.tx_unit, jnum(st, "traffic.tx_rate", &v) ? v : NAN, bytes);
    fmt_bytes(jnum_or(st, "traffic.rx_total", 0), a, sizeof a);
    fmt_bytes(jnum_or(st, "traffic.tx_total", 0), b, sizeof b);
    ui_set_fmt(ui.total, "↓ %s  ↑ %s", a, b);
    if (jnum(st, "system.uptime", &v))
        fmt_duration(v, text, sizeof text);
    else
        snprintf(text, sizeof text, "—");
    ui_set_text(ui.uptime, text);

    const cJSON *boot = jget(st, "boot");
    lv_obj_set_hidden(ui.boot, !jtrue(boot, "temporary"));
    ui_set_text(ui.boot_note, jtrue(boot, "can_set_default")
                                  ? L("是否设为默认启动？不会立即重启或断网。", "Make it the default? No reboot or network interruption.")
                                  : L("当前启动镜像不支持设置默认启动", "This boot image cannot set a default boot"));
    lv_obj_set_state(ui.boot_button, LV_STATE_DISABLED, boot_busy || !jtrue(boot, "can_set_default"));
    render_usb(st);

    const char *word;
    lv_color_t color;
    bool offline = wan_word(st, &word, &color);
    ui_set_text(ui.wan_word, word);
    lv_obj_set_style_text_color(ui.wan_word, color, 0);
    /* Disabled, the card takes neither presses nor the key focus. */
    lv_obj_set_state(ui.wan_card, LV_STATE_DISABLED, !offline);
    const char *hint = offline ? L(" · 双击重连", " · Double-tap to reconnect") : "";
    size_t len = (size_t)snprintf(text, sizeof text, "%s", hint);
    if (jstr(st, "modem.tech") && jtrue(st, "wan.up"))
        len += (size_t)snprintf(text + len, sizeof text - len, " · %s", tech_name(jstr(st, "modem.tech")));
    if (jtrue(st, "wan.up") && (jstr(st, "wan.ipv4") || jstr(st, "wan.ipv6")))
        snprintf(text + len, sizeof text - len, " · %s%s%s", jstr(st, "wan.ipv4") ? "IPv4" : "",
                 jstr(st, "wan.ipv4") && jstr(st, "wan.ipv6") ? " " : "", jstr(st, "wan.ipv6") ? "IPv6" : "");
    ui_set_text(ui.wan, text);

    const cJSON *w = jget(st, "wifi");
    const char *state = jstr(w, "state");
    bool enabled = jtrue(w, "enabled"), up = jtrue(w, "up");
    const char *ws = !enabled ? L("已关闭", "Off") : up ? L("已开启", "On")
                   : state && !strcmp(state, "failed") ? L("未启动", "Not running") : L("启动中", "Starting");
    ui_set_text(ui.wifi_word, ws);
    lv_obj_set_style_text_color(ui.wifi_word, enabled && up ? theme.good : enabled ? theme.fair : theme.text, 0);
    if (jstr(w, "ssid"))
        ui_set_fmt(ui.wifi, " · %s", jstr(w, "ssid"));
    else
        ui_set_text(ui.wifi, "");

    int total = 0, wifi = 0, usb = 0;
    const cJSON *c;
    cJSON_ArrayForEach(c, jget(st, "clients")) {
        const char *via = jstr(c, "via");
        total++;
        wifi += via && !strcmp(via, "wifi");
        usb += via && !strcmp(via, "usb");
    }
    if (!total) {
        ui_set_text(ui.clients, L("无", "None"));
    } else {
        len = (size_t)snprintf(text, sizeof text, "%d%s", total, L("台", ""));
        if (wifi)
            len += (size_t)snprintf(text + len, sizeof text - len, " · Wi-Fi %d", wifi);
        if (usb)
            snprintf(text + len, sizeof text - len, " · USB");
        ui_set_text(ui.clients, text);
    }

    const cJSON *bat = jget(st, "battery");
    battery_text(bat, text, sizeof text);
    if (jnum(st, "system.temperatures.battery", &v)) {
        len = strlen(text);
        snprintf(text + len, sizeof text - len, " · %.1f°C", v);
    }
    ui_set_text(ui.battery, text);
    double ma, mv;
    bool has_ma = jnum(bat, "current_ma", &ma);
    /* Within ±20 mA it is the gauge's idle offset: no colour, no sign. */
    if (has_ma)
        ui_set_fmt(ui.power_word, "%s%g mA", ma >= 20 ? "+" : "", ma);
    else
        ui_set_text(ui.power_word, "--");
    lv_obj_set_style_text_color(ui.power_word, has_ma && ma >= 20 ? theme.good : has_ma && ma <= -20 ? theme.fair : theme.text, 0);
    if (jnum(bat, "voltage_mv", &mv))
        ui_set_fmt(ui.power, " · %.2f V", mv / 1000);
    else
        ui_set_text(ui.power, "");

    double mem_total, mem_avail;
    if (jnum(st, "system.mem_total", &mem_total) && jnum(st, "system.mem_available", &mem_avail))
        usage_meter(&ui.memory, mem_total - mem_avail, mem_total);
    else
        usage_meter(&ui.memory, -1, 0);
    usage_meter(&ui.storage, jnum_or(st, "system.disk_used", -1), jnum_or(st, "system.disk_total", 0));
    for (int i = 0; i < 8; i++) {
        char key[48];
        snprintf(key, sizeof key, "system.temperatures.%s", temp_fields[i]);
        fmt_temp(jget(st, key), text, sizeof text);
        ui_set_text(ui.temps[i], text);
    }
    poll_update(false);
}

static void hidden(void)
{
    battery_close();
}

const struct page_ops overview_page = {
    .icon = "home", .zh = "概览", .en = "Home",
    .create = create, .status = status, .hidden = hidden, .top = hidden, .back = battery_close,
    .held = battery_is_open, .key = battery_key, .touch = battery_touch,
};
