#include <math.h>
#include <stdio.h>
#include <string.h>

#include "api.h"
#include "app.h"
#include "theme.h"
#include "ui.h"
#include "util.h"

static struct {
    lv_obj_t *battery, *uptime, *wan_uptime, *load, *memory, *usb, *lan, *v4, *v6, *level;
} ui;

static void usb_text(const cJSON *u, char *out, size_t size)
{
    const char *role = jstr(u, "role"), *link = jstr(u, "link");
    if (role && !strcmp(role, "host")) {
        size_t len = 0;
        const cJSON *d;
        cJSON_ArrayForEach(d, jget(u, "devices")) {
            char name[96];
            usb_device_name(d, name, sizeof name);
            len += (size_t)snprintf(out + len, size - len, "%s%s", len ? " · " : "", name);
            if (len >= size)
                return;
        }
        if (!len)
            snprintf(out, size, "%s", L("OTG 主机，等待设备", "OTG host, waiting for device"));
        return;
    }
    if (!u || !link) {
        snprintf(out, size, "--");
        return;
    }
    static const char *const links[][3] = {
        {"none", "未连接", "Not connected"}, {"charger", "仅充电（充电器）", "Charging only (a charger)"},
        {"host", "USB 供电，未建立数据连接", "USB power, no data connection"},
        {"enumerated", "已连接电脑", "Connected to a computer"}, {"lease", "电脑已获取地址", "The computer has an address"},
        {"online", "电脑正通过 E5 上网", "The computer is online through the E5"},
    };
    const char *word = link;
    for (size_t i = 0; i < sizeof links / sizeof *links; i++)
        if (!strcmp(link, links[i][0]))
            word = L(links[i][1], links[i][2]);
    size_t len = (size_t)snprintf(out, size, "%s", word);
    bool leased = !strcmp(link, "lease") || !strcmp(link, "online");
    if (leased && jstr(u, "ip") && len < size)
        len += (size_t)snprintf(out + len, size - len, " · %s", jstr(u, "ip"));
    if (!strcmp(link, "online") && len < size) {
        char down[24], up[24];
        fmt_bytes(jnum_or(u, "tx_rate", 0), down, sizeof down);
        fmt_bytes(jnum_or(u, "rx_rate", 0), up, sizeof up);
        snprintf(out + len, size - len, " · ↓%s/s ↑%s/s", down, up);
    }
}

static void paint_level(void)
{
    ui_set_fmt(ui.level, "%d%%", (int)lround(app.brightness / 255.0 * 100));
}

static void status(const cJSON *st)
{
    char text[256];
    battery_text(jget(st, "battery"), text, sizeof text);
    ui_set_text(ui.battery, text);
    double v;
    if (jnum(st, "system.uptime", &v)) {
        fmt_duration(v, text, sizeof text);
        ui_set_text(ui.uptime, text);
    }
    if (jtrue(st, "wan.up") && jnum(st, "wan.uptime", &v)) {
        fmt_duration(v, text, sizeof text);
        ui_set_text(ui.wan_uptime, text);
    } else {
        ui_set_text(ui.wan_uptime, "--");
    }
    if (jnum(st, "system.load", &v))
        ui_set_fmt(ui.load, "%.2f", v);
    double total, available;
    if (jnum(st, "system.mem_total", &total) && jnum(st, "system.mem_available", &available)) {
        char used[24], all[24];
        fmt_bytes(total - available, used, sizeof used);
        fmt_bytes(total, all, sizeof all);
        ui_set_fmt(ui.memory, "%s / %s", used, all);
    }
    usb_text(jget(st, "usb"), text, sizeof text);
    ui_set_text(ui.usb, text);
    ui_set_text(ui.lan, jstr_or(st, "system.lan_ip", "--"));
    ui_set_text(ui.v4, jstr_or(st, "wan.ipv4", "--"));
    const char *v6 = jstr_or(st, "wan.ipv6_prefix", jstr(st, "wan.ipv6"));
    ui_set_text(ui.v6, v6 ? v6 : "--");
    paint_level();
}

static void on_brightness(lv_event_t *e)
{
    int step = (int)(intptr_t)lv_event_get_user_data(e);
    app.brightness = app.brightness + step < 10 ? 10 : app.brightness + step > 255 ? 255 : app.brightness + step;
    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "level", app.brightness);
    cJSON_AddBoolToObject(body, "save", true);
    api_post("/backlight", body, NULL, NULL);
    paint_level();
}

static void on_reconnect(lv_event_t *e)
{
    toast("%s", L("正在重新连接…", "Reconnecting…"));
    api_post("/wan-reconnect", NULL, NULL, NULL);
    shell_poll_after(3000);
}

static void create(lv_obj_t *page)
{
    lv_obj_t *system = ui_card(page);
    ui.battery = ui_kv(system, L("电池", "Battery"));
    ui.uptime = ui_kv(system, L("开机时长", "Uptime"));
    ui.wan_uptime = ui_kv(system, L("联网时长", "Online"));
    ui.load = ui_kv(system, L("负载", "Load"));
    ui.memory = ui_kv(system, L("内存", "Memory"));
    lv_obj_t *network = ui_card(page);
    ui.usb = ui_kv(network, "USB");
    ui.lan = ui_kv(network, "LAN");
    ui.v4 = ui_kv(network, "IPv4");
    ui.v6 = ui_kv(network, "IPv6");
    lv_obj_set_style_text_font(ui.v6, F(11), 0);
    lv_obj_t *light = ui_card(page);
    lv_obj_set_flex_flow(light, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(light, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *label = ui_text(light, 13, false, theme.dim);
    lv_label_set_text(label, L("亮度", "Brightness"));
    lv_obj_set_flex_grow(label, 1);
    ui_button(light, "−", on_brightness, (void *)(intptr_t)-25);
    ui.level = ui_text(light, 14, false, theme.text);
    lv_obj_set_width(ui.level, S(48));
    lv_obj_set_style_text_align(ui.level, LV_TEXT_ALIGN_CENTER, 0);
    ui_button(light, "+", on_brightness, (void *)(intptr_t)25);
    lv_obj_t *reconnect = ui_button(page, L("重新连接网络", "Reconnect"), on_reconnect, NULL);
    lv_obj_set_width(reconnect, LV_PCT(100));
    paint_level();
}

const struct page_ops device_page = {
    .icon = "device", .zh = "设备", .en = "Device",
    .create = create, .status = status,
};
