/* Clash's external controller, with the address, port and secret kept as the
 * screen's options clash_host (empty for this device), clash_port and clash_secret.
 * The rates come from the totals in /connections read each second; a core on
 * this device also shows its process's memory and uptime. */
#include "clash.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "api.h"
#include "app.h"
#include "clash_find.h"
#include "input.h"
#include "keyboard.h"
#include "theme.h"
#include "ui.h"
#include "util.h"

#define LOOPBACK "127.0.0.1"
#define POLL_MS 1000
/* Polls between reads of the mode, which the dashboard may change too. */
#define MODE_EVERY 5

static const struct {
    const char *id, *zh, *en;
} modes[] = {
    {"rule", "规则", "Rule"},
    {"global", "全局", "Global"},
    {"direct", "直连", "Direct"},
};
#define MODES ((int)(sizeof modes / sizeof *modes))

#define SUBSCRIPTIONS 16

static struct {
    lv_obj_t *page, *main, *setup, *subs;
    lv_obj_t *notice, *up, *down, *up_total, *down_total, *conns, *core, *memory, *uptime, *modes;
    lv_obj_t *subs_row, *drop_row, *subs_list, *host, *port, *secret;
} ui;

static struct {
    char host[64];
    /* 0 until a port is saved. */
    int port;
    char secret[128];
    /* Replies to an earlier connection are dropped. */
    unsigned generation;
    int mode, polls;
    bool open, busy, reached, switching, sampled;
    double up, down;
    uint32_t at;
    /* The core's process when it runs on this device, else 0. */
    int pid;
} clash;

/* The subscriptions shown, by card, and the one updating (-1 for none). */
static struct {
    char names[SUBSCRIPTIONS][64];
    int updating;
} subs = {.updating = -1};

static lv_timer_t *poller;

static void request(const char *method, const char *path, cJSON *body, api_cb cb)
{
    api_http(*clash.host ? clash.host : LOOPBACK, clash.port, method, path, clash.secret, body, cb,
             (void *)(uintptr_t)clash.generation);
}

static bool current(void *ctx)
{
    return (unsigned)(uintptr_t)ctx == clash.generation;
}

static bool succeeded(int status)
{
    return status >= 200 && status < 300;
}

static void paint_modes(void)
{
    ui_choices_paint(ui.modes, clash.mode, false);
}

/* sing-box offers the modes its route rules name. */
static void offer_modes(const cJSON *list)
{
    for (int i = 0; i < MODES; i++) {
        bool offered = !list;
        const cJSON *item;
        cJSON_ArrayForEach(item, list) offered |= cJSON_IsString(item) && !strcasecmp(item->valuestring, modes[i].id);
        lv_obj_set_state(lv_obj_get_child(ui.modes, i), LV_STATE_DISABLED, !offered);
    }
}

static void clear(void)
{
    ui_set_text(ui.up, "—");
    ui_set_text(ui.down, "—");
    ui_set_text(ui.up_total, "");
    ui_set_text(ui.down_total, "");
    ui_set_text(ui.conns, "—");
    ui_set_text(ui.core, "—");
    lv_obj_set_hidden(lv_obj_get_parent(ui.memory), true);
    lv_obj_set_hidden(lv_obj_get_parent(ui.uptime), true);
    lv_obj_set_hidden(ui.subs_row, true);
    lv_obj_set_hidden(ui.drop_row, true);
    clash.mode = -1;
    clash.sampled = false;
    offer_modes(NULL);
    paint_modes();
}

/* Shows why a read failed; false when it did. */
static bool answered(int status)
{
    if (succeeded(status)) {
        lv_obj_set_hidden(ui.notice, true);
        return true;
    }
    if (status == 401 && !*clash.secret)
        ui_set_text(ui.notice, L("未设置控制密码，请在「连接设置」中填写", "No secret is set; enter it in Connection"));
    else if (status == 401)
        ui_set_text(ui.notice, L("控制密码不正确", "The secret is wrong"));
    else if (status)
        ui_set_fmt(ui.notice, L("控制接口返回 HTTP %d", "The controller answered HTTP %d"), status);
    else
        ui_set_fmt(ui.notice, L("无法连接 %s 端口 %d，请确认 Clash 已运行且地址和端口正确",
                                "Nothing answers at %s port %d; check that Clash runs there"),
                   *clash.host ? clash.host : LOOPBACK, clash.port);
    lv_obj_set_hidden(ui.notice, false);
    if (clash.reached)
        clear();
    clash.reached = false;
    return false;
}

static void on_version(int status, cJSON *r, void *ctx)
{
    if (!current(ctx) || !answered(status))
        return;
    /* sing-box names itself in the version; Clash gives the number alone. */
    const char *version = jstr_or(r, "version", "");
    bool singbox = isalpha((unsigned char)version[0]) && !(version[0] == 'v' && isdigit((unsigned char)version[1]));
    if (singbox)
        ui_set_text(ui.core, version);
    else
        ui_set_fmt(ui.core, "%s %s", jtrue(r, "meta") ? "Mihomo" : "Clash", version);
    /* sing-box has no providers. */
    lv_obj_set_hidden(ui.subs_row, singbox);
}

static void on_configs(int status, cJSON *r, void *ctx)
{
    if (!current(ctx) || clash.switching || !answered(status))
        return;
    const char *mode = jstr_or(r, "mode", "");
    clash.mode = -1;
    for (int i = 0; i < MODES; i++)
        if (!strcasecmp(mode, modes[i].id))
            clash.mode = i;
    offer_modes(jget(r, "mode-list"));
    paint_modes();
}

static void rate(lv_obj_t *label, double bytes_per_s)
{
    char num[16], unit[16];
    fmt_speed(bytes_per_s, jstr_is(app.status, "screen.speed_unit", "bytes"), 2, num, unit, sizeof num);
    ui_set_fmt(label, "%s %s", num, unit);
}

static void total(lv_obj_t *label, double bytes)
{
    char amount[32];
    fmt_amount(bytes, amount, sizeof amount);
    ui_set_fmt(label, L("累计 %s", "%s total"), amount);
}

static bool local(void)
{
    return !*clash.host || !strcmp(clash.host, LOOPBACK) || !strcmp(clash.host, "::1");
}

/* The running core that serves this port. */
static int core_pid(void)
{
    struct clash_core cores[4];
    int n = clash_find(cores, (int)(sizeof cores / sizeof *cores));
    for (int i = 0; i < n; i++)
        if (cores[i].port == clash.port)
            return cores[i].pid;
    return 0;
}

static void paint_process(void)
{
    double memory, uptime;
    bool known = clash.pid && clash_process(clash.pid, &memory, &uptime);
    lv_obj_set_hidden(lv_obj_get_parent(ui.memory), !known);
    lv_obj_set_hidden(lv_obj_get_parent(ui.uptime), !known);
    if (!known)
        return;
    char text[32];
    fmt_amount(memory, text, sizeof text);
    ui_set_text(ui.memory, text);
    fmt_duration(uptime, text, sizeof text);
    ui_set_text(ui.uptime, text);
}

static void on_connections(int status, cJSON *r, void *ctx)
{
    if (!current(ctx))
        return;
    clash.busy = false;
    if (!answered(status))
        return;
    if (!clash.reached) {
        clash.reached = true;
        lv_obj_set_hidden(ui.drop_row, false);
        /* A restarted core is found again once it answers. */
        clash.pid = local() ? core_pid() : 0;
        request("GET", "/version", NULL, on_version);
        request("GET", "/configs", NULL, on_configs);
    }
    paint_process();
    double up = jnum_or(r, "uploadTotal", NAN), down = jnum_or(r, "downloadTotal", NAN);
    uint32_t now = lv_tick_get();
    if (clash.sampled && now != clash.at) {
        double seconds = lv_tick_diff(now, clash.at) / 1000.0;
        rate(ui.up, (up - clash.up) / seconds);
        rate(ui.down, (down - clash.down) / seconds);
    }
    clash.sampled = true;
    clash.up = up;
    clash.down = down;
    clash.at = now;
    total(ui.up_total, up);
    total(ui.down_total, down);
    ui_set_fmt(ui.conns, "%d", cJSON_GetArraySize(jget(r, "connections")));
}

static void refresh(lv_timer_t *t)
{
    if (app.blank || !clash.port)
        return;
    if (!clash.busy) {
        clash.busy = true;
        request("GET", "/connections", NULL, on_connections);
    }
    if (clash.reached && ++clash.polls % MODE_EVERY == 0)
        request("GET", "/configs", NULL, on_configs);
}

static void reconnect(void)
{
    char port[8];
    screen_option("clash_port", port, sizeof port);
    clash.port = atoi(port);
    screen_option("clash_secret", clash.secret, sizeof clash.secret);
    screen_option("clash_host", clash.host, sizeof clash.host);
    clash.generation++;
    clash.busy = clash.reached = clash.switching = false;
    /* An update's reply is dropped with the rest. */
    subs.updating = -1;
    clear();
    lv_obj_set_hidden(ui.notice, false);
    if (!clash.port) {
        ui_set_text(ui.notice, *clash.secret ? L("未设置 API 端口，请在「连接设置」中填写或自动识别",
                                                 "No API port is set; enter or detect it in Connection")
                                             : L("未设置 API 端口和控制密码，请在「连接设置」中填写或自动识别",
                                                 "No API port or secret is set; enter or detect them in Connection"));
        return;
    }
    ui_set_text(ui.notice, L("正在连接…", "Connecting…"));
    refresh(NULL);
}

static void load_subscriptions(void);

/* A page over the status, which stops reading meanwhile. */
static void show(lv_obj_t *view)
{
    keyboard_close();
    input_defocus();
    lv_obj_set_hidden(ui.main, view != ui.main);
    lv_obj_set_hidden(ui.setup, view != ui.setup);
    lv_obj_set_hidden(ui.subs, view != ui.subs);
    lv_obj_scroll_to_y(ui.page, 0, LV_ANIM_OFF);
    if (view == ui.main) {
        lv_timer_resume(poller);
        return;
    }
    lv_timer_pause(poller);
    if (view == ui.subs) {
        load_subscriptions();
        return;
    }
    char port[8] = "";
    if (clash.port)
        snprintf(port, sizeof port, "%d", clash.port);
    lv_textarea_set_text(ui.host, clash.host);
    lv_textarea_set_text(ui.port, port);
    lv_textarea_set_text(ui.secret, clash.secret);
}

static void on_setup(lv_event_t *e)
{
    show(ui.setup);
}

static void on_subscriptions(lv_event_t *e)
{
    show(ui.subs);
}

static void on_dropped(int status, cJSON *r, void *ctx)
{
    if (!current(ctx))
        return;
    if (succeeded(status))
        toast("%s", L("已断开全部连接", "All connections closed"));
    else
        toast("%s", L("断开连接失败", "Could not close the connections"));
}

static void on_drop(lv_event_t *e)
{
    request("DELETE", "/connections", NULL, on_dropped);
}

static void subscription_note(const char *text)
{
    lv_obj_t *note = ui_note(ui.subs_list, text);
    lv_obj_set_style_pad_hor(note, S(4), 0);
}

/* Unreserved characters stay; the rest of a name's UTF-8 is percent-encoded. */
static void path_escape(const char *in, char *out, size_t size)
{
    size_t len = 0;
    for (; *in && len + 4 < size; in++) {
        unsigned char c = (unsigned char)*in;
        if (isalnum(c) || strchr("-._~", c))
            out[len++] = (char)c;
        else
            len += (size_t)snprintf(out + len, size - len, "%%%02X", c);
    }
    out[len] = 0;
}

static void stamp(double epoch, bool date_only, char *out, size_t size)
{
    struct local_time t = local_at(epoch);
    if (date_only)
        snprintf(out, size, "%04d-%02d-%02d", t.year, t.month, t.day);
    else
        snprintf(out, size, "%02d-%02d %02d:%02d", t.month, t.day, t.hour, t.minute);
}

static void on_update(lv_event_t *e);

static void subscription_card(const cJSON *provider, int index)
{
    snprintf(subs.names[index], sizeof subs.names[index], "%s", jstr_or(provider, "name", ""));
    lv_obj_t *card = ui_card(ui.subs_list);
    lv_obj_t *head = ui_box(card, LV_FLEX_FLOW_ROW, 8);
    lv_obj_t *name = ui_text(head, 13, true, theme.text);
    lv_obj_set_flex_grow(name, 1);
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(name, subs.names[index]);
    lv_obj_t *update = ui_button(head, subs.updating == index ? L("更新中…", "Updating…") : L("更新", "Update"),
                                 on_update, (void *)(intptr_t)index);
    lv_obj_set_style_pad_ver(update, S(5), 0);
    lv_obj_set_style_text_font(lv_obj_get_child(update, 0), F(12), 0);
    lv_obj_set_state(update, LV_STATE_DISABLED, subs.updating >= 0);

    const cJSON *info = jget(provider, "subscriptionInfo");
    double used = jnum_or(info, "Upload", 0) + jnum_or(info, "Download", 0), all = jnum_or(info, "Total", 0);
    char text[64], a[24], b[24];
    if (all > 0) {
        struct ui_meter meter = ui_meter(card, L("已用流量", "Used"));
        fmt_amount(used, a, sizeof a);
        fmt_amount(all, b, sizeof b);
        snprintf(text, sizeof text, "%s / %s", a, b);
        int percent = (int)LV_MIN(100, used * 100 / all);
        ui_meter_set(&meter, text, percent, percent >= 90 ? theme.danger : theme.accent);
    }
    double at;
    if (jnum(info, "Expire", &at) && at > 0) {
        stamp(at, true, text, sizeof text);
        lv_label_set_text(ui_kv(card, L("到期时间", "Expires")), text);
    }
    snprintf(text, sizeof text, "%d", cJSON_GetArraySize(jget(provider, "proxies")));
    lv_label_set_text(ui_kv(card, L("节点数", "Nodes")), text);
    if (iso_epoch(jstr(provider, "updatedAt"), &at)) {
        stamp(at, false, text, sizeof text);
        lv_label_set_text(ui_kv(card, L("更新时间", "Updated")), text);
    }
}

/* Subscriptions are the providers fetched over HTTP. */
static void on_providers(int status, cJSON *r, void *ctx)
{
    if (!current(ctx))
        return;
    /* A refresh after an update keeps the place on the page. */
    int32_t y = lv_obj_get_scroll_y(ui.page);
    lv_obj_clean(ui.subs_list);
    if (!succeeded(status)) {
        subscription_note(L("读取订阅失败", "Could not read the subscriptions"));
        return;
    }
    int n = 0;
    const cJSON *provider;
    cJSON_ArrayForEach(provider, jget(r, "providers")) {
        if (n < SUBSCRIPTIONS && jstr_is(provider, "vehicleType", "HTTP"))
            subscription_card(provider, n++);
    }
    if (!n)
        subscription_note(L("没有订阅", "No subscriptions"));
    lv_obj_update_layout(ui.page);
    lv_obj_scroll_to_y(ui.page, y, LV_ANIM_OFF);
}

static void load_subscriptions(void)
{
    lv_obj_clean(ui.subs_list);
    subscription_note(L("正在读取…", "Loading…"));
    request("GET", "/providers/proxies", NULL, on_providers);
}

static void on_updated(int status, cJSON *r, void *ctx)
{
    if (!current(ctx))
        return;
    const char *name = subs.names[subs.updating];
    if (succeeded(status))
        toast(L("%s 已更新", "%s updated"), name);
    else
        toast(L("%s 更新失败", "Could not update %s"), name);
    subs.updating = -1;
    request("GET", "/providers/proxies", NULL, on_providers);
}

/* One at a time: the core downloads the subscription before it answers. */
static void on_update(lv_event_t *e)
{
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    char name[200], path[240];
    path_escape(subs.names[index], name, sizeof name);
    snprintf(path, sizeof path, "/providers/proxies/%s", name);
    subs.updating = index;
    /* Each card's first child is its head: the name, then the button. */
    for (uint32_t i = 0; i < lv_obj_get_child_count(ui.subs_list); i++)
        lv_obj_add_state(lv_obj_get_child(lv_obj_get_child(lv_obj_get_child(ui.subs_list, (int32_t)i), 0), 1),
                         LV_STATE_DISABLED);
    ui_button_text(lv_event_get_target_obj(e), L("更新中…", "Updating…"));
    request("PUT", path, NULL, on_updated);
}

static void on_switched(int status, cJSON *r, void *ctx)
{
    if (!current(ctx))
        return;
    clash.switching = false;
    if (!succeeded(status))
        toast("%s", L("切换模式失败", "Could not switch the mode"));
    request("GET", "/configs", NULL, on_configs);
}

static void choose(int mode)
{
    if (!clash.reached || clash.switching || mode == clash.mode)
        return;
    clash.mode = mode;
    clash.switching = true;
    paint_modes();
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "mode", modes[mode].id);
    request("PATCH", "/configs", body, on_switched);
}

static void on_mode(lv_event_t *e)
{
    choose((int)(intptr_t)lv_event_get_user_data(e));
}

static bool numeric_ip(const char *text)
{
    unsigned char addr[16];
    return inet_pton(AF_INET, text, addr) == 1 || inet_pton(AF_INET6, text, addr) == 1;
}

static void on_save(lv_event_t *e)
{
    const char *host = lv_textarea_get_text(ui.host);
    if (*host && !numeric_ip(host)) {
        toast("%s", L("IP 地址格式不正确", "The IP address is not valid"));
        return;
    }
    const char *text = lv_textarea_get_text(ui.port);
    if (!*text) {
        toast("%s", L("请填写 API 端口", "Enter the API port"));
        return;
    }
    int port = atoi(text);
    if (port < 1 || port > 65535) {
        toast("%s", L("端口应在 1 到 65535 之间", "The port is from 1 to 65535"));
        return;
    }
    char value[8];
    snprintf(value, sizeof value, "%d", port);
    if (!screen_option_set("clash_host", host) || !screen_option_set("clash_port", value) ||
        !screen_option_set("clash_secret", lv_textarea_get_text(ui.secret))) {
        toast("%s", L("保存失败", "Could not save"));
        return;
    }
    show(ui.main);
    reconnect();
    toast("%s", L("已保存", "Saved"));
}

static struct clash_core found[4];

static void fill(const struct clash_core *core)
{
    char port[8];
    snprintf(port, sizeof port, "%d", core->port);
    lv_textarea_set_text(ui.host, core->host);
    lv_textarea_set_text(ui.port, port);
    lv_textarea_set_text(ui.secret, core->secret);
    toast(L("已填入 %s 的连接设置", "Filled in from %s"), core->name);
}

static void on_pick(lv_event_t *e)
{
    dialog_close();
    fill(&found[(intptr_t)lv_event_get_user_data(e)]);
}

/* Several cores running: the one to use is the user's pick. */
static void on_detect(lv_event_t *e)
{
    int n = clash_find(found, (int)(sizeof found / sizeof *found)), usable = 0, last = 0;
    for (int i = 0; i < n; i++)
        if (found[i].port)
            usable++, last = i;
    if (!n) {
        toast("%s", L("未找到运行中的 Clash 或 sing-box", "No running Clash or sing-box"));
    } else if (!usable) {
        toast(L("%s 未开启外部控制接口", "%s has no external controller"), found[0].name);
    } else if (usable == 1) {
        fill(&found[last]);
    } else {
        dialog_open(L("选择核心", "Choose a core"), NULL);
        char label[64];
        for (int i = 0; i < n; i++) {
            if (!found[i].port)
                continue;
            snprintf(label, sizeof label, L("%s · 端口 %d", "%s · port %d"), found[i].name, found[i].port);
            dialog_button(label, on_pick, (void *)(intptr_t)i);
        }
        dialog_cancel();
    }
}

static void on_reset(lv_event_t *e)
{
    dialog_close();
    if (!screen_option_clear("clash_host") || !screen_option_clear("clash_port") ||
        !screen_option_clear("clash_secret")) {
        toast("%s", L("重置失败", "Could not reset"));
        return;
    }
    show(ui.main);
    reconnect();
    toast("%s", L("已重置", "Reset"));
}

static void on_confirm_reset(lv_event_t *e)
{
    dialog_open(L("重置连接设置", "Reset the connection"),
                L("清除保存的 IP 地址、API 端口和控制密码", "Clears the saved address, API port and secret"));
    ui_danger(dialog_button(L("重置", "Reset"), on_reset, NULL));
    dialog_cancel();
}

void clash_create(lv_obj_t *parent)
{
    memset(&ui, 0, sizeof ui);
    ui.page = lv_obj_get_parent(parent);
    lv_obj_t *root = ui.main = ui_box(parent, LV_FLEX_FLOW_COLUMN, ui_stack_gap());
    ui.notice = ui_note(root, "");
    lv_obj_set_hidden(ui.notice, true);

    lv_obj_t *traffic = ui_card(root);
    lv_obj_t *rates = ui_box(traffic, LV_FLEX_FLOW_ROW, 8);
    ui_figure(rates, L("上传", "Upload"), &ui.up, &ui.up_total);
    ui_figure(rates, L("下载", "Download"), &ui.down, &ui.down_total);
    ui.conns = ui_kv(traffic, L("连接数", "Connections"));
    ui.core = ui_kv(traffic, L("核心", "Core"));
    ui.memory = ui_kv(traffic, L("内存", "Memory"));
    ui.uptime = ui_kv(traffic, L("运行时长", "Uptime"));

    lv_obj_t *mode = ui_card(root);
    lv_label_set_text(ui_text(mode, 12, false, theme.dim), L("代理模式", "Proxy mode"));
    ui.modes = ui_choices(mode);
    for (int i = 0; i < MODES; i++)
        ui_choice(ui.modes, L(modes[i].zh, modes[i].en), on_mode, (void *)(intptr_t)i);
    ui_focusable(ui.modes);
    lv_obj_set_style_outline_pad(ui.modes, S(4), LV_STATE_FOCUS_KEY);

    lv_obj_t *rows = ui_box(root, LV_FLEX_FLOW_COLUMN, ui_stack_gap());
    ui.subs_row = ui_row(rows, "download", L("订阅信息", "Subscriptions"), NULL, ROW_CHEVRON, on_subscriptions, NULL);
    ui.drop_row = ui_row(rows, "close", L("断开全部连接", "Close all connections"), NULL, 0, on_drop, NULL);
    ui_row(rows, "settings", L("连接设置", "Connection"), NULL, ROW_CHEVRON, on_setup, NULL);

    ui.subs = ui_box(parent, LV_FLEX_FLOW_COLUMN, ui_stack_gap());
    lv_obj_set_hidden(ui.subs, true);
    ui_heading(ui.subs, L("订阅信息", "Subscriptions"));
    ui.subs_list = ui_box(ui.subs, LV_FLEX_FLOW_COLUMN, ui_stack_gap());

    ui.setup = ui_box(parent, LV_FLEX_FLOW_COLUMN, ui_stack_gap());
    lv_obj_set_hidden(ui.setup, true);
    ui_heading(ui.setup, L("连接设置", "Connection"));
    lv_obj_t *setup = ui_card(ui.setup);
    ui.host = ui_labeled_field(setup, L("IP 地址", "IP address"), sizeof clash.host - 1);
    lv_textarea_set_accepted_chars(ui.host, "0123456789abcdefABCDEF.:");
    lv_textarea_set_placeholder_text(ui.host, L("127.0.0.1（本机）", "127.0.0.1 (this device)"));
    ui.port = ui_labeled_field(setup, L("API 端口", "API port"), 5);
    lv_textarea_set_accepted_chars(ui.port, "0123456789");
    lv_textarea_set_placeholder_text(ui.port, "9090");
    ui.secret = ui_labeled_field(setup, L("控制密码", "Secret"), sizeof clash.secret - 1);
    lv_textarea_set_placeholder_text(ui.secret, L("未设置", "None"));
    lv_obj_t *actions = ui_box(setup, LV_FLEX_FLOW_ROW, 8);
    lv_obj_set_flex_grow(ui_button(actions, L("自动识别", "Detect"), on_detect, NULL), 1);
    lv_obj_t *save = ui_button(actions, L("保存并连接", "Save and connect"), on_save, NULL);
    ui_primary(save);
    lv_obj_set_flex_grow(save, 1);
    lv_obj_t *note = ui_note(setup, L("连接 Clash 或 sing-box 的外部控制接口，IP 地址留空即为本机。"
                                      "自动识别从本机运行中的核心及其配置读取。",
                                      "Connects to a Clash or sing-box controller; an empty address is this "
                                      "device. Detect reads the running core and its configuration."));
    lv_obj_set_style_pad_hor(note, 0, 0);
    lv_obj_t *reset = ui_button(ui.setup, L("重置设置", "Reset"), on_confirm_reset, NULL);
    ui_danger(reset);
    lv_obj_set_width(reset, LV_PCT(100));
    clear();
}

void clash_open(void)
{
    clash.open = true;
    if (!poller)
        poller = lv_timer_create(refresh, POLL_MS, NULL);
    show(ui.main);
    reconnect();
}

void clash_close(void)
{
    if (!clash.open)
        return;
    clash.open = false;
    lv_timer_pause(poller);
    keyboard_close();
}

bool clash_back(void)
{
    if (!lv_obj_is_hidden(ui.main))
        return false;
    show(ui.main);
    return true;
}

bool clash_key(const struct key_event *key)
{
    if (key->kind != KEY_KIND_LEFT && key->kind != KEY_KIND_RIGHT)
        return false;
    if (input_key_focus(input_group()) != ui.modes || clash.mode < 0)
        return true;
    /* Past the modes the core does not offer. */
    int dir = key->kind == KEY_KIND_RIGHT ? 1 : -1;
    for (int mode = clash.mode + dir; mode >= 0 && mode < MODES; mode += dir)
        if (!lv_obj_has_state(lv_obj_get_child(ui.modes, mode), LV_STATE_DISABLED)) {
            choose(mode);
            break;
        }
    return true;
}
