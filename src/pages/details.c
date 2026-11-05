#include <stdio.h>
#include <string.h>

#include "api.h"
#include "app.h"
#include "theme.h"
#include "ui.h"
#include "util.h"

static const char *const temp_fields[] = {"cpu", "gpu", "soc", "lte", "nr", "mm", "board", "pa", "battery"};
static const char *const temp_names[][2] = {
    {"CPU", "CPU"}, {"GPU", "GPU"}, {"SoC", "SoC"}, {"4G 基带", "4G modem"}, {"5G 基带", "5G modem"},
    {"多媒体", "Media"}, {"主板", "Board"}, {"射频功放", "RF PA"}, {"电池", "Battery"},
};
static const char *const direct_zones[] = {"soc-thmzone", "gpu-thmzone", "lte-thmzone", "mm-thmzone",
                                           "board-thmzone", "pa-thmzone", "battery"};

static struct {
    lv_obj_t *freshness, *model, *os, *image, *kernel, *disk, *temp, *battery;
    lv_obj_t *temps[9], *more, *extra;
    lv_obj_t *bb, *sa, *modes, *firmware, *lte, *nr, *cell, *slot, *op, *reg;
    lv_obj_t *ids, *imei, *iccid, *imsi, *number, *show_ids;
} ui;

static cJSON *advanced;
static bool busy, extra_temps, show_ids;
static lv_timer_t *retry, *refresh;

static void render(const cJSON *a)
{
    const cJSON *d = jget(a, "device"), *b = jget(a, "baseband"), *s = jget(a, "sim");
    bool radio_error = jtrue(a, "radio_error");
    lv_obj_set_hidden(ui.freshness, !radio_error);
    ui_set_text(ui.model, jstr(d, "model") ? jstr(d, "model") : "--");
    ui_set_text(ui.os, jstr(d, "os") ? jstr(d, "os") : "--");
    ui_set_text(ui.image, jstr(d, "image") ? jstr(d, "image") : "--");
    ui_set_text(ui.kernel, jstr(d, "kernel") ? jstr(d, "kernel") : "--");
    double used, total, v;
    if (jnum(d, "disk.used", &used) && jnum(d, "disk.total", &total)) {
        char u[24], t[24];
        fmt_bytes(used, u, sizeof u);
        fmt_bytes(total, t, sizeof t);
        ui_set_fmt(ui.disk, "%s / %s", u, t);
    }
    if (jnum(d, "thermal.temp", &v))
        ui_set_fmt(ui.temp, "%.0f °C", v);
    else
        ui_set_text(ui.temp, "--");
    char text[128] = "";
    size_t len = 0;
    if (jnum(d, "battery_mv", &v) && v)
        len += (size_t)snprintf(text, sizeof text, "%.2f V", v / 1000);
    if (jnum(d, "battery_temp", &v))
        snprintf(text + len, sizeof text - len, "%s%.0f °C", len ? " · " : "", v);
    ui_set_text(ui.battery, *text ? text : "--");

    const cJSON *temps = jget(d, "temperatures") ? jget(d, "temperatures") : jget(app.status, "system.temperatures");
    for (int i = 0; i < 9; i++) {
        fmt_temp(jget(temps, temp_fields[i]), text, sizeof text);
        ui_set_text(ui.temps[i], text);
    }
    lv_obj_clean(ui.extra);
    const cJSON *zone;
    cJSON_ArrayForEach(zone, jget(d, "thermal_zones")) {
        const char *name = jstr(zone, "zone");
        bool direct = false;
        for (size_t i = 0; name && i < sizeof direct_zones / sizeof *direct_zones; i++)
            direct |= !strcmp(name, direct_zones[i]);
        if (direct)
            continue;
        fmt_temp(jget(zone, "temp"), text, sizeof text);
        len = strlen(text);
        if (jtrue(zone, "estimated"))
            len += (size_t)snprintf(text + len, sizeof text - len, " · %s", L("估算", "Estimated"));
        if (jtrue(zone, "unverified"))
            snprintf(text + len, sizeof text - len, " · %s", L("待核实", "Unverified"));
        lv_obj_t *value = ui_kv(ui.extra, name ? name : "");
        lv_label_set_text(value, text);
        if (jtrue(zone, "unverified"))
            lv_obj_set_style_text_color(value, theme.fair, 0);
    }
    lv_obj_set_hidden(ui.extra, !extra_temps);
    ui_button_text(ui.more, extra_temps ? L("收起", "Show less") : L("查看更多", "Show more"));

    if (cJSON_IsObject(b)) {
        const char *maker = jstr(b, "manufacturer"), *model = jstr(b, "model");
        ui_set_fmt(ui.bb, "%s%s%s", maker ? maker : "", maker && model ? " " : "", model ? model : "");
    } else {
        ui_set_text(ui.bb, L("无模组", "No modem"));
    }
    const cJSON *sa = jget(b, "sa_allowed");
    ui_set_text(ui.sa, cJSON_IsBool(sa) ? (cJSON_IsTrue(sa) ? L("允许", "Allowed") : L("仅 NSA", "NSA only")) : "--");
    const char *modes = jstr(b, "modes");
    if (modes) {
        char m[160];
        snprintf(m, sizeof m, "%s", strncmp(modes, "allowed: ", 9) ? modes : modes + 9);
        char *pref = strstr(m, "; preferred: none");
        if (pref && !pref[17])
            *pref = 0;
        ui_set_text(ui.modes, m);
    } else {
        ui_set_text(ui.modes, "--");
    }
    char fw[512] = "";
    len = 0;
    const cJSON *f;
    cJSON_ArrayForEach(f, jget(b, "firmware"))
        if (len < sizeof fw)
            len += (size_t)snprintf(fw + len, sizeof fw - len, "%s%s: %s", len ? "\n" : "", jstr(f, "name") ? jstr(f, "name") : "",
                                    jstr(f, "value") ? jstr(f, "value") : "");
    ui_set_text(ui.firmware, fw);

    struct {
        lv_obj_t *label;
        const char *key, *prefix;
    } locks[2] = {{ui.lte, "lte_lock", "B"}, {ui.nr, "nr_lock", "n"}};
    for (int i = 0; i < 2; i++) {
        const cJSON *list = jget(b, locks[i].key), *x;
        if (!cJSON_IsObject(b) || !cJSON_IsArray(list)) {
            ui_set_text(locks[i].label, "--");
            continue;
        }
        len = 0;
        text[0] = 0;
        cJSON_ArrayForEach(x, list)
            if (len < sizeof text)
                len += (size_t)snprintf(text + len, sizeof text - len, "%s%s%g", len ? " " : "", locks[i].prefix,
                                        x->valuedouble);
        ui_set_text(locks[i].label, len ? text : L("未锁定", "Not locked"));
    }
    const cJSON *lte = jget(b, "lte_cell_lock"), *nr = jget(b, "nr_cell_lock"), *c;
    if (cJSON_IsObject(b) && cJSON_IsArray(lte) && cJSON_IsArray(nr)) {
        len = 0;
        text[0] = 0;
        cJSON_ArrayForEach(c, lte)
            if (len < sizeof text)
                len += (size_t)snprintf(text + len, sizeof text - len, "%sLTE %g/%g", len ? ", " : "",
                                        jnum_or(c, "arfcn", 0), jnum_or(c, "pci", 0));
        cJSON_ArrayForEach(c, nr)
            if (len < sizeof text)
                len += (size_t)snprintf(text + len, sizeof text - len, "%sNR %g/%g", len ? ", " : "",
                                        jnum_or(c, "arfcn", 0), jnum_or(c, "pci", 0));
        ui_set_text(ui.cell, len ? text : L("未锁定", "Not locked"));
    } else {
        ui_set_text(ui.cell, "--");
    }

    if (jnum(s, "active_slot", &v) && v)
        ui_set_fmt(ui.slot, "%s%g", L("卡", "SIM "), v);
    else
        ui_set_text(ui.slot, "--");
    const char *op = jstr(s, "operator"), *code = jstr(s, "operator_code");
    if (op || code)
        ui_set_fmt(ui.op, "%s%s%s", op ? op : "", op && code ? " · " : "", code ? code : "");
    else
        ui_set_text(ui.op, "--");
    static const char *const regs[][3] = {
        {"home", "本地网", "Home"}, {"roaming", "漫游", "Roaming"}, {"idle", "未注册", "Not registered"},
        {"denied", "被拒绝", "Denied"}, {"searching", "搜索网络", "Searching"},
    };
    const char *reg = jstr(s, "registration");
    const char *word = reg ? reg : "--";
    for (size_t i = 0; reg && i < sizeof regs / sizeof *regs; i++)
        if (!strcmp(reg, regs[i][0]))
            word = L(regs[i][1], regs[i][2]);
    ui_set_text(ui.reg, word);
}

static void load(void);

static void retry_tick(lv_timer_t *t)
{
    if (app.page == PAGE_DETAILS && !app.blank)
        load();
}

static void loaded(int status, cJSON *r, void *ctx)
{
    busy = false;
    shell_page_loading(PAGE_DETAILS, false);
    bool ok = api_read_ok(status, r);
    shell_page_error(PAGE_DETAILS, !ok);
    if (ok) {
        cJSON_Delete(advanced);
        advanced = cJSON_Duplicate(r, true);
    }
    if (advanced)
        render(advanced);
    if (jtrue(advanced, "radio_pending") && app.page == PAGE_DETAILS && !app.blank) {
        lv_timer_reset(retry);
        lv_timer_resume(retry);
    }
}

static void load(void)
{
    if (busy)
        return;
    lv_timer_pause(retry);
    busy = true;
    shell_page_loading(PAGE_DETAILS, true);
    api_get("/advanced", loaded, NULL);
}

static void hide_ids(void)
{
    show_ids = false;
    lv_obj_set_hidden(ui.ids, true);
    ui_button_text(ui.show_ids, L("显示识别码", "Show identifiers"));
    lv_obj_t *labels[] = {ui.imei, ui.iccid, ui.imsi, ui.number};
    for (int i = 0; i < 4; i++)
        ui_set_text(labels[i], "--");
}

static void ids_loaded(int status, cJSON *r, void *ctx)
{
    shell_page_loading(PAGE_DETAILS, false);
    if (!api_read_ok(status, r))
        return;
    show_ids = true;
    ui_set_text(ui.imei, jstr(r, "imei") ? jstr(r, "imei") : "--");
    ui_set_text(ui.iccid, jstr(r, "iccid") ? jstr(r, "iccid") : "--");
    ui_set_text(ui.imsi, jstr(r, "imsi") ? jstr(r, "imsi") : "--");
    char numbers[128] = "";
    size_t len = 0;
    const cJSON *n;
    cJSON_ArrayForEach(n, jget(r, "numbers"))
        if (cJSON_IsString(n) && len < sizeof numbers)
            len += (size_t)snprintf(numbers + len, sizeof numbers - len, "%s%s", len ? ", " : "", n->valuestring);
    ui_set_text(ui.number, len ? numbers : "--");
    lv_obj_set_hidden(ui.ids, false);
    ui_button_text(ui.show_ids, L("隐藏识别码", "Hide identifiers"));
}

static void on_ids(lv_event_t *e)
{
    if (show_ids) {
        hide_ids();
        return;
    }
    shell_page_loading(PAGE_DETAILS, true);
    api_get("/identity", ids_loaded, NULL);
}

static void on_more(lv_event_t *e)
{
    extra_temps = !extra_temps;
    lv_obj_set_hidden(ui.extra, !extra_temps);
    ui_button_text(ui.more, extra_temps ? L("收起", "Show less") : L("查看更多", "Show more"));
}

static lv_obj_t *section(lv_obj_t *page, const char *title)
{
    lv_obj_t *card = ui_card(page);
    lv_label_set_text(ui_text(card, 12, true, theme.accent), title);
    return card;
}

static void create(lv_obj_t *page)
{
    ui.freshness = ui_note(page, L("部分无线信息暂不可用", "Some radio details unavailable"));
    lv_obj_set_hidden(ui.freshness, true);
    lv_obj_t *device = section(page, L("设备", "Device"));
    ui.model = ui_kv(device, L("型号", "Model"));
    ui.os = ui_kv(device, L("系统", "System"));
    ui.image = ui_kv(device, L("镜像版本", "Image"));
    ui.kernel = ui_kv(device, L("内核", "Kernel"));
    ui.disk = ui_kv(device, L("存储", "Storage"));
    ui.temp = ui_kv(device, L("温度", "Temperature"));
    ui.battery = ui_kv(device, L("电池", "Battery"));
    lv_obj_t *temps = section(page, L("温度详情", "Temperatures"));
    for (int i = 0; i < 9; i++)
        ui.temps[i] = ui_kv(temps, L(temp_names[i][0], temp_names[i][1]));
    ui.more = ui_button(temps, "", on_more, NULL);
    ui.extra = ui_box(temps, LV_FLEX_FLOW_COLUMN, 6);
    lv_obj_t *bb = section(page, L("基带", "Baseband"));
    ui.bb = ui_kv(bb, L("型号", "Model"));
    ui.sa = ui_kv(bb, "5G SA");
    ui.modes = ui_kv(bb, L("网络模式", "Modes"));
    ui.firmware = ui_text(bb, 11, false, theme.dim);
    lv_obj_set_width(ui.firmware, LV_PCT(100));
    lv_label_set_long_mode(ui.firmware, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_t *locks = section(page, L("锁定", "Locks"));
    ui.lte = ui_kv(locks, L("LTE 频段", "LTE bands"));
    ui.nr = ui_kv(locks, L("NR 频段", "NR bands"));
    ui.cell = ui_kv(locks, L("锁小区", "Cell lock"));
    lv_obj_t *sim = section(page, "SIM");
    ui.slot = ui_kv(sim, L("卡槽", "Slot"));
    ui.op = ui_kv(sim, L("运营商", "Operator"));
    ui.reg = ui_kv(sim, L("注册", "Registration"));
    lv_obj_t *ids = ui_card(page);
    ui.ids = ui_box(ids, LV_FLEX_FLOW_COLUMN, 6);
    ui.imei = ui_kv(ui.ids, "IMEI");
    ui.iccid = ui_kv(ui.ids, "ICCID");
    ui.imsi = ui_kv(ui.ids, "IMSI");
    ui.number = ui_kv(ui.ids, L("本机号码", "Number"));
    ui.show_ids = ui_button(ids, "", on_ids, NULL);
    lv_obj_set_width(ui.show_ids, LV_PCT(100));
    hide_ids();
    if (!retry) {
        retry = lv_timer_create(retry_tick, 800, NULL);
        lv_timer_pause(retry);
        refresh = lv_timer_create(retry_tick, 30000, NULL);
        lv_timer_pause(refresh);
    }
    ui_button_text(ui.more, L("查看更多", "Show more"));
    lv_obj_set_hidden(ui.extra, true);
    if (advanced)
        render(advanced);
}

static void shown(void)
{
    load();
    lv_timer_resume(refresh);
}

static void reset(void)
{
    hide_ids();
    extra_temps = false;
    lv_obj_set_hidden(ui.extra, true);
    ui_button_text(ui.more, L("查看更多", "Show more"));
}

static void hidden(void)
{
    lv_timer_pause(refresh);
    lv_timer_pause(retry);
    hide_ids();
}

const struct page_ops details_page = {
    .icon = "info", .zh = "详细信息", .en = "Details",
    .create = create, .shown = shown, .hidden = hidden, .top = reset,
};
