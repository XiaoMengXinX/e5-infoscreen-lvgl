#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api.h"
#include "app.h"
#include "theme.h"
#include "ui.h"
#include "util.h"

#define QR_SIZE 168

static struct {
    lv_obj_t *ssid, *toggle, *qr, *qr_off, *key, *show_key, *count, *clients;
} ui;

static lv_draw_buf_t *qr_buf;
static char qr_for[256];
static int wifi_pending = -1;
static bool show_key;
static char *clients_signature;

/* qrencode's SVG: a viewBox of N modules and one 1×1 rect per dark module. */
static lv_draw_buf_t *qr_from_svg(const char *svg)
{
    int n = 0, tx = 0, ty = 0;
    const char *p = strstr(svg, "viewBox=\"0 0 ");
    if (!p || sscanf(p, "viewBox=\"0 0 %d", &n) != 1 || n <= 0 || n > 200)
        return NULL;
    p = strstr(svg, "translate(");
    if (p)
        sscanf(p, "translate(%d,%d)", &tx, &ty);
    int module = QR_SIZE / n > 0 ? QR_SIZE / n : 1, size = module * n;
    lv_draw_buf_t *buf = lv_draw_buf_create((uint32_t)size, (uint32_t)size, LV_COLOR_FORMAT_XRGB8888, 0);
    if (!buf)
        return NULL;
    for (int y = 0; y < size; y++)
        memset(buf->data + (size_t)y * buf->header.stride, 0xff, (size_t)size * 4);
    const char *pattern = p ? p : svg;
    while ((p = strstr(pattern, "<rect "))) {
        pattern = p + 6;
        int x, y;
        const char *fill = strstr(p, "fill=\"#000000\"");
        const char *end = strchr(p, '>');
        if (!fill || !end || fill > end || sscanf(p, "<rect x=\"%d\" y=\"%d\"", &x, &y) != 2)
            continue;
        x += tx;
        y += ty;
        if (x < 0 || y < 0 || x >= n || y >= n)
            continue;
        for (int row = 0; row < module; row++) {
            uint32_t *line = (uint32_t *)(buf->data + (size_t)(y * module + row) * buf->header.stride);
            for (int col = 0; col < module; col++)
                line[x * module + col] = 0xff000000;
        }
    }
    return buf;
}

static void qr_loaded(int status, const char *body, void *ctx)
{
    shell_page_loading(PAGE_HOTSPOT, false);
    lv_draw_buf_t *buf = status == 200 && body ? qr_from_svg(body) : NULL;
    if (!buf) {
        qr_for[0] = 0;
        return;
    }
    lv_image_set_src(ui.qr, buf);
    if (qr_buf)
        lv_draw_buf_destroy(qr_buf);
    qr_buf = buf;
}

static void key_loaded(int status, cJSON *r, void *ctx)
{
    const char *key = show_key ? jstr(r, "key") : NULL;
    ui_set_text(ui.key, key ? key : "");
    lv_obj_set_hidden(ui.key, !key || !*key);
}

static void on_show_key(lv_event_t *e)
{
    show_key = !show_key;
    ui_button_text(ui.show_key, show_key ? L("隐藏密码", "Hide key") : L("显示密码", "Show key"));
    lv_obj_set_hidden(ui.key, true);
    if (show_key)
        api_get("/wifi-key", key_loaded, NULL);
}

static void poll_soon(lv_timer_t *t)
{
    shell_poll_now();
}

static void toggled(int status, cJSON *r, void *ctx)
{
    if (!api_reply_ok(r)) {
        wifi_pending = -1;
        char error[256] = "";
        if (jstr(r, "error"))
            error_text(jstr(r, "error"), error, sizeof error);
        toast("%s%s%s", L("失败", "Failed"), *error ? ": " : "", error);
    }
    lv_timer_set_repeat_count(lv_timer_create(poll_soon, 1500, NULL), 1);
}

static void status(const cJSON *st);

static void on_toggle(lv_event_t *e)
{
    if (!app.status)
        return;
    bool on = !(wifi_pending >= 0 ? wifi_pending : jtrue(app.status, "wifi.enabled"));
    wifi_pending = on;
    toast("%s", on ? L("正在开启热点…", "Turning the hotspot on…") : L("正在关闭热点…", "Turning the hotspot off…"));
    status(app.status);
    cJSON *body = cJSON_CreateObject();
    cJSON_AddBoolToObject(body, "on", on);
    api_post("/wifi", body, toggled, NULL);
}

static void status(const cJSON *st)
{
    const cJSON *w = jget(st, "wifi");
    ui_set_text(ui.ssid, jstr(w, "ssid") ? jstr(w, "ssid") : "--");
    bool enabled = jtrue(w, "enabled"), up = jtrue(w, "up");
    bool on = wifi_pending >= 0 ? wifi_pending : enabled;
    ui_button_text(ui.toggle, on ? L("开", "On") : L("关", "Off"));
    lv_obj_set_style_bg_color(ui.toggle, on ? theme.accent : theme.button, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(ui.toggle, 0), on ? theme.bg : theme.text, 0);
    const char *state = jstr(w, "state");
    if (wifi_pending >= 0 && wifi_pending == enabled && (!enabled || up || (state && !strcmp(state, "failed"))))
        wifi_pending = -1;

    bool live = enabled && up;
    lv_obj_set_style_opa(ui.qr, live ? LV_OPA_COVER : LV_OPA_20, 0);
    lv_obj_set_hidden(ui.qr_off, live);
    char error[256];
    const char *raw = jstr(w, "error");
    if (raw)
        error_text(raw, error, sizeof error);
    ui_set_text(ui.qr_off, raw ? error : enabled ? L("热点未启动", "Hotspot not up") : L("热点已关闭", "Hotspot off"));

    char key[256];
    snprintf(key, sizeof key, "%s|%d|%d|%g", jstr(w, "ssid") ? jstr(w, "ssid") : "", jtrue(w, "secured"),
             jtrue(w, "hidden"), jnum_or(w, "qr_revision", 0));
    if (jstr(w, "ssid") && strcmp(key, qr_for)) {
        snprintf(qr_for, sizeof qr_for, "%s", key);
        shell_page_loading(PAGE_HOTSPOT, true);
        api_get_text("/qr", qr_loaded, NULL);
    }

    const cJSON *clients = jget(st, "clients"), *c;
    ui_set_fmt(ui.count, "(%d)", cJSON_GetArraySize(clients));
    char *signature = cJSON_PrintUnformatted(clients);
    bool same = signature && clients_signature && !strcmp(signature, clients_signature);
    free(clients_signature);
    clients_signature = signature;
    if (same)
        return;
    lv_obj_clean(ui.clients);
    if (!cJSON_GetArraySize(clients))
        lv_label_set_text(ui_text(ui.clients, 12, false, theme.dim), L("无", "None"));
    cJSON_ArrayForEach(c, clients) {
        lv_obj_t *row = ui_box(ui.clients, LV_FLEX_FLOW_ROW, 8);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        const char *name = jstr(c, "name") ? jstr(c, "name") : jstr(c, "ip") ? jstr(c, "ip") : jstr(c, "mac");
        lv_obj_t *label = ui_text(row, 13, false, theme.text);
        lv_obj_set_flex_grow(label, 1);
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
        lv_label_set_text(label, name ? name : L("未命名", "unnamed"));
        const char *via = jstr(c, "via");
        char detail[96];
        int n = snprintf(detail, sizeof detail, "%s · %s", jstr(c, "ip") ? jstr(c, "ip") : "",
                         via && !strcmp(via, "usb") ? "USB" : "Wi-Fi");
        double signal;
        if (jnum(c, "signal", &signal) && n > 0 && (size_t)n < sizeof detail)
            snprintf(detail + n, sizeof detail - (size_t)n, " %g dBm", signal);
        lv_label_set_text(ui_text(row, 11, false, theme.dim), detail);
    }
}

static void create(lv_obj_t *page)
{
    lv_obj_t *head = ui_card(page);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ui.ssid = ui_text(head, 15, true, theme.text);
    lv_obj_set_flex_grow(ui.ssid, 1);
    lv_label_set_long_mode(ui.ssid, LV_LABEL_LONG_MODE_DOTS);
    ui.toggle = ui_button(head, "", on_toggle, NULL);
    lv_obj_set_style_min_width(ui.toggle, S(64), 0);
    /* When on, the switch is filled with the accent; ring it in the text colour. */
    lv_obj_set_style_outline_color(ui.toggle, theme.text, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_pad(ui.toggle, 2, LV_STATE_FOCUS_KEY);

    lv_obj_t *qr = ui_card(page);
    lv_obj_set_flex_align(qr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_label_set_text(ui_text(qr, 14, true, theme.text), L("扫码即连", "Scan to connect"));
    lv_label_set_text(ui_text(qr, 11, false, theme.dim), L("用手机相机扫描，无需输入密码", "Scan with your camera to join"));
    lv_obj_t *frame = lv_obj_create(qr);
    lv_obj_remove_style_all(frame);
    lv_obj_set_size(frame, QR_SIZE + 16, QR_SIZE + 16);
    lv_obj_set_style_bg_color(frame, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(frame, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(frame, 10, 0);
    lv_obj_set_scrollable(frame, false);
    ui.qr = lv_image_create(frame);
    lv_obj_center(ui.qr);
    if (qr_buf)
        lv_image_set_src(ui.qr, qr_buf);
    ui.qr_off = ui_text(frame, 12, false, lv_color_black());
    lv_obj_set_width(ui.qr_off, LV_PCT(90));
    lv_obj_set_style_text_align(ui.qr_off, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(ui.qr_off, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_center(ui.qr_off);
    /* Takes no room until the key is shown. */
    ui.key = ui_text(qr, 13, false, theme.text);
    lv_obj_set_hidden(ui.key, true);
    ui.show_key = ui_button(qr, show_key ? L("隐藏密码", "Hide key") : L("显示密码", "Show key"), on_show_key, NULL);

    lv_obj_t *list = ui_card(page);
    lv_obj_t *title = ui_box(list, LV_FLEX_FLOW_ROW, 4);
    lv_label_set_text(ui_text(title, 12, false, theme.dim), L("在线设备", "Clients"));
    ui.count = ui_text(title, 12, false, theme.dim);
    ui.clients = ui_box(list, LV_FLEX_FLOW_COLUMN, 8);
    free(clients_signature);
    clients_signature = NULL;
    if (show_key)
        api_get("/wifi-key", key_loaded, NULL);
}

static void shown(void)
{
    qr_for[0] = 0;
    if (app.status)
        status(app.status);
}

const struct page_ops hotspot_page = {
    .icon = "wifi", .zh = "热点", .en = "Hotspot",
    .create = create, .shown = shown, .status = status,
};
