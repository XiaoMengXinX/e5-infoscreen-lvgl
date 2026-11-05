#include <math.h>
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "theme.h"
#include "ui.h"
#include "util.h"

static struct {
    lv_obj_t *op, *tech, *band;
    struct {
        lv_obj_t *slot, *op, *role, *state, *radio, *signal, *note;
    } sims[2];
    struct ui_meter meters[3];
    lv_obj_t *pci, *arfcn, *bw, *nb, *qos;
} ui;

static const struct {
    const char *kind, *path, *unit;
    double excellent, good, fair, weak, min, max;
} grades[3] = {
    {"RSRP", "modem.signal.rsrp", " dBm", -80, -90, -100, -110, -140, -44},
    {"SINR", "modem.signal.snr", " dB", 20, 13, 5, 0, -10, 30},
    {"RSRQ", "modem.signal.rsrq", " dB", -10, -12, -15, -18, -25, -3},
};

static void create(lv_obj_t *page)
{
    lv_obj_t *head = ui_card(page);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_gap(head, S(10), 0);
    ui.op = ui_text(head, 19, true, theme.text);
    lv_label_set_long_mode(ui.op, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_flex_grow(ui.op, 1);
    ui.tech = ui_text(head, 19, true, theme.text);
    ui.band = ui_text(head, 16, false, theme.text);

    lv_obj_t *sims = ui_card(page);
    lv_obj_set_style_pad_ver(sims, S(4), 0);
    for (int slot = 0; slot < 2; slot++) {
        lv_obj_t *line = ui_box(sims, LV_FLEX_FLOW_COLUMN, 6);
        lv_obj_set_style_pad_ver(line, S(8), 0);
        if (slot) {
            lv_obj_set_style_border_side(line, LV_BORDER_SIDE_TOP, 0);
            lv_obj_set_style_border_width(line, 1, 0);
            lv_obj_set_style_border_color(line, theme.line, 0);
        }
        lv_obj_t *heading = ui_box(line, LV_FLEX_FLOW_ROW, 8);
        ui.sims[slot].slot = ui_text(heading, 10, false, theme.text);
        lv_obj_set_style_bg_color(ui.sims[slot].slot, theme.button, 0);
        lv_obj_set_style_bg_opa(ui.sims[slot].slot, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(ui.sims[slot].slot, 5, 0);
        lv_obj_set_style_pad_hor(ui.sims[slot].slot, 5, 0);
        lv_obj_set_style_pad_ver(ui.sims[slot].slot, 2, 0);
        lv_label_set_text_fmt(ui.sims[slot].slot, "SIM %d", slot + 1);
        ui.sims[slot].op = ui_text(heading, 14, true, theme.text);
        lv_obj_set_flex_grow(ui.sims[slot].op, 1);
        lv_label_set_long_mode(ui.sims[slot].op, LV_LABEL_LONG_MODE_DOTS);
        ui.sims[slot].role = ui_text(heading, 10, false, theme.dim);
        lv_obj_t *reading = ui_box(line, LV_FLEX_FLOW_ROW, 8);
        ui.sims[slot].state = ui_text(reading, 12, false, theme.text);
        ui.sims[slot].radio = ui_text(reading, 12, true, theme.text);
        lv_obj_set_flex_grow(ui.sims[slot].radio, 1);
        lv_obj_set_style_text_align(ui.sims[slot].radio, LV_TEXT_ALIGN_RIGHT, 0);
        ui.sims[slot].signal = ui_text(reading, 12, false, theme.dim);
        ui.sims[slot].note = ui_text(line, 10, false, theme.dim);
    }

    for (int i = 0; i < 3; i++)
        ui.meters[i] = ui_meter(ui_card(page), grades[i].kind);

    lv_obj_t *grid = ui_card(page);
    ui.pci = ui_kv(grid, "PCI");
    ui.arfcn = ui_kv(grid, "ARFCN");
    ui.bw = ui_kv(grid, L("带宽", "Bandwidth"));
    ui.nb = ui_kv(grid, L("邻区", "Neighbours"));
    ui.qos = ui_kv(grid, L("签约速率", "Subscribed rate"));
}

static const char *registration(const char *state)
{
    static const char *const states[][3] = {
        {"home", "已注册", "Registered"}, {"roaming", "漫游", "Roaming"}, {"searching", "搜网中", "Searching"},
        {"denied", "注册被拒绝", "Registration denied"}, {"idle", "未注册", "Not registered"},
        {"emergency", "仅紧急服务", "Emergency only"}, {"absent", "未插卡", "No SIM"},
        {"locked", "SIM 已锁定", "SIM locked"}, {"disabled", "射频关闭", "Radio off"}, {"camped", "已驻网", "Camped"},
    };
    for (size_t i = 0; state && i < sizeof states / sizeof *states; i++)
        if (!strcmp(state, states[i][0]))
            return L(states[i][1], states[i][2]);
    return L("状态未知", "Unknown");
}

static void kbps(double k, char *out, size_t size)
{
    char n[24];
    if (k >= 1000000) {
        fmt_trim(k / 1000000, 1, n, sizeof n);
        snprintf(out, size, "%s Gbps", n);
    } else if (k >= 1000) {
        fmt_trim(k / 1000, 1, n, sizeof n);
        snprintf(out, size, "%s Mbps", n);
    } else {
        snprintf(out, size, "%g kbps", k);
    }
}

static void show_number(lv_obj_t *label, const cJSON *object, const char *path, const char *fmt)
{
    double v;
    if (jnum(object, path, &v))
        ui_set_fmt(label, fmt, v);
    else
        ui_set_text(label, "--");
}

static void render_sims(const cJSON *st)
{
    double data_slot;
    bool has_data = jnum(st, "modem.sim_card", &data_slot);
    for (int slot = 0; slot < 2; slot++) {
        const cJSON *sim = NULL, *s;
        cJSON_ArrayForEach(s, jget(st, "sims"))
            if ((int)jnum_or(s, "slot", -1) == slot)
                sim = s;
        bool data = has_data && (int)data_slot == slot;
        const char *op = operator_name(jstr(sim, "operator"));
        ui_set_text(ui.sims[slot].op, op ? op : L("待获取运营商", "Operator unavailable"));
        ui_set_text(ui.sims[slot].role, data ? L("数据卡", "Data") : L("副卡", "Secondary"));
        lv_obj_set_style_text_color(ui.sims[slot].slot, data ? theme.accent : theme.text, 0);
        ui_set_text(ui.sims[slot].state, registration(jstr(sim, "registration")));
        ui_set_text(ui.sims[slot].radio, jstr(sim, "radio_label") ? jstr(sim, "radio_label") : "—");
        double rsrp;
        if (jnum(sim, "signal.rsrp", &rsrp))
            ui_set_fmt(ui.sims[slot].signal, "%g dBm", rsrp);
        else
            ui_set_text(ui.sims[slot].signal, "— dBm");
        const cJSON *error = jget(sim, "error");
        bool unavailable = jtrue(sim, "stale") || (error && !cJSON_IsNull(error));
        const char *reg = jstr(sim, "registration");
        ui_set_text(ui.sims[slot].note, unavailable ? L("状态暂不可用", "Status unavailable")
                                        : reg && !strcmp(reg, "camped") ? L("注册详情未上报", "Registration details not reported")
                                                                        : "");
        lv_obj_set_hidden(ui.sims[slot].note, !*lv_label_get_text(ui.sims[slot].note));
    }
}

static void status(const cJSON *st)
{
    const cJSON *m = jget(st, "modem"), *c = jget(m, "cell");
    const char *radio = jstr(m, "radio_label");
    ui_set_text(ui.tech, radio && *radio ? radio : tech_name(jstr(m, "tech")));
    ui_set_text(ui.band, jstr(c, "band") ? jstr(c, "band") : "");
    const char *op = operator_name(jstr(m, "operator"));
    const char *reg = jstr(m, "registration");
    if (op && reg && !strcmp(reg, "roaming"))
        ui_set_fmt(ui.op, "%s · %s", op, L("漫游", "Roaming"));
    else
        ui_set_text(ui.op, op ? op : "--");
    render_sims(st);

    for (int i = 0; i < 3; i++) {
        double v;
        if (!jnum(st, grades[i].path, &v)) {
            ui_meter_set(&ui.meters[i], "--", 0, theme.accent);
            continue;
        }
        const char *word = v >= grades[i].excellent ? "excellent" : v >= grades[i].good ? "good"
                         : v >= grades[i].fair ? "fair" : v >= grades[i].weak ? "weak" : "poor";
        static const char *const words[][3] = {{"excellent", "极好", "Excellent"}, {"good", "好", "Good"},
                                               {"fair", "一般", "Fair"}, {"weak", "弱", "Weak"}, {"poor", "差", "Poor"}};
        const char *name = "";
        for (int w = 0; w < 5; w++)
            if (!strcmp(words[w][0], word))
                name = L(words[w][1], words[w][2]);
        double frac = fmax(0, fmin(1, (v - grades[i].min) / (grades[i].max - grades[i].min)));
        char text[64];
        snprintf(text, sizeof text, "%.1f%s · %s", v, grades[i].unit, name);
        ui_meter_set(&ui.meters[i], text, (int)lround(frac * 100), grade_color(word));
    }

    show_number(ui.pci, c, "pci", "%g");
    show_number(ui.arfcn, c, "arfcn", "%g");
    show_number(ui.bw, c, "bandwidth_mhz", "%g MHz");
    if (jtrue(m, "present"))
        ui_set_fmt(ui.nb, "%g", jnum_or(m, "neighbours", 0));
    else
        ui_set_text(ui.nb, "--");
    const cJSON *q = jget(m, "qos");
    if (cJSON_IsObject(q)) {
        char dl[24], ul[24];
        kbps(jnum_or(q, "dl_kbps", 0), dl, sizeof dl);
        kbps(jnum_or(q, "ul_kbps", 0), ul, sizeof ul);
        ui_set_fmt(ui.qos, "↓%s ↑%s · %s %g", dl, ul, jtrue(q, "nr") ? "5QI" : "QCI", jnum_or(q, "qci", 0));
    } else {
        ui_set_text(ui.qos, "--");
    }
}

const struct page_ops signal_page = {
    .icon = "signal", .zh = "信号", .en = "Signal",
    .create = create, .status = status,
};
