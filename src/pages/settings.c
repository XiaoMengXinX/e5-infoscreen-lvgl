/* Settings are described by /api/settings; this page renders that model as a
 * stack of views: menu → category → editor, plus the dedicated views below. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api.h"
#include "app.h"
#include "keyboard.h"
#include "theme.h"
#include "ui.h"
#include "util.h"

#define DEPTH 8
#define ROWS 96

enum kind {
    V_MENU, V_CAT, V_EDIT, V_AT, V_ATRES, V_DEVICES, V_DEVICE,
    V_BT, V_BTDEV, V_APN, V_APNEDIT,
};

struct view {
    enum kind kind;
    unsigned serial;
    char cat[64];
    char title[128];
    cJSON *data;
    cJSON *item;
    cJSON *draft;
    char text[512];
    char *reply;
    bool warn, saving, loading, has_password;
    int slot;
    int32_t scroll;
    char busy[64];
};

static struct view stack[DEPTH];
static int depth;
static unsigned serials;
static cJSON *categories;
static bool categories_busy;

static lv_obj_t *path, *body;
static int32_t scroll_to = -1;
static char row_keys[ROWS][96];
static int row_count;
static char armed[96], focused[96];
static lv_timer_t *disarm_timer, *bt_timer;

static const struct {
    const char *id, *icon;
} icons[] = {
    {"network", "signal"}, {"at", "terminal"}, {"devices", "device"}, {"appmgr", "apps"}, {"bluetooth", "bluetooth"},
    {"charge", "battery"}, {"notify", "bell"}, {"sound", "volume"}, {"screen", "device"}, {"ui", "palette"},
    {"navigation", "navigation"}, {"usb", "usb"}, {"system", "chip"}, {"about", "info"}, {"cpu", "chip"},
    {"cpu_thermal", "sun"}, {"persist", "power"}, {"current", "chip"}, {"voltage", "battery"},
    {"speed_unit", "traffic"}, {"ui_theme", "palette"}, {"ui_size", "type"},
    {"ui_accent", "palette"}, {"ui_motion", "motion"}, {"status_bar", "layout"},
    {"nav_bar", "navigation"}, {"lock_enabled", "lock"}, {"lock_daily", "traffic"}, {"lock_monthly", "traffic"},
    {"lock_speed", "signal"}, {"unlock_distance", "touch"}, {"brightness", "sun"}, {"idle", "clock"},
    {"touch", "touch"}, {"lang", "globe"}, {"volume", "volume"}, {"call_volume", "phone"}, {"vibrate", "bell"},
    {"sim_card", "simcard"}, {"apn", "globe"}, {"net_mode", "signal"}, {"nr_mode", "signal"},
    {"lte_bands", "signal"}, {"nr_bands", "signal"}, {"cell_lock", "lock"}, {"bands_default", "refresh"},
    {"reconnect", "refresh"}, {"start", "battery"}, {"stop", "battery"}, {"full_once", "battery"},
    {"state", "info"}, {"timezone", "globe"}, {"clock_seconds", "clock"}, {"version", "info"}, {"built", "clock"},
    {"screen_version", "info"}, {"update_check", "refresh"}, {"update_apply", "download"},
    {"update_rollback", "refresh"}, {"traffic_clear", "trash"}, {"clear_0", "trash"}, {"clear_1", "trash"},
    {"clear_all", "trash"}, {"next_boot", "power"}, {"default_boot", "power"}, {"reboot", "refresh"},
    {"poweroff", "power"}, {"android_once", "power"}, {"check", "search"}, {"diagnosis", "info"},
    {"wireless_error", "wifi"}, {"hardware_error", "chip"}, {"controller", "usb"}, {"host_address", "device"},
    {"reset_status", "info"}, {"reset", "refresh"}, {"usb_role_state", "usb"}, {"usb_role", "usb"},
    {"usb_host_boot", "power"}, {"usb_net_current", "globe"}, {"usb_net_mode", "globe"},
    {"usb_net_scope", "clock"}, {"usb_net_auto", "check"}, {"usb_lan_auto", "device"}, {"name", "info"},
    {"maintainer", "user"}, {"copyright", "shield"}, {"license", "shield"}, {"control_slot", "simcard"},
};

static const char *const page_icons[] = {"home", "signal", "traffic", "sms", "wifi", "device", "info", "settings", "apps"};

static const char *icon_for(const char *id, const char *fallback)
{
    if (id && !strncmp(id, "nav_slot", 8))
        return "layout";
    if (id && (!strncmp(id, "cpu_policy", 10) || !strncmp(id, "policy", 6)))
        return "chip";
    if (id && !strncmp(id, "thermal_zone", 12))
        return "sun";
    if (id && !strncmp(id, "voltage_", 8))
        return "battery";
    for (size_t i = 0; id && i < sizeof icons / sizeof *icons; i++)
        if (!strcmp(icons[i].id, id))
            return icons[i].icon;
    return fallback ? fallback : "settings";
}

static struct view *top(void)
{
    return depth ? &stack[depth - 1] : NULL;
}

static struct view *find(unsigned serial)
{
    for (int i = 0; i < depth; i++)
        if (stack[i].serial == serial)
            return &stack[i];
    return NULL;
}

static void *tag(struct view *v)
{
    return (void *)(uintptr_t)v->serial;
}

static struct view *untag(void *ctx)
{
    return find((unsigned)(uintptr_t)ctx);
}

static void release(struct view *v)
{
    cJSON_Delete(v->data);
    cJSON_Delete(v->item);
    cJSON_Delete(v->draft);
    free(v->reply);
    memset(v, 0, sizeof *v);
}

/* A new view starts at the top; going back restores where the list was. */
static struct view *push(enum kind kind, const char *title)
{
    if (depth == DEPTH)
        release(&stack[--depth]);
    if (depth && body)
        stack[depth - 1].scroll = lv_obj_get_scroll_y(lv_obj_get_parent(body));
    scroll_to = 0;
    struct view *v = &stack[depth++];
    memset(v, 0, sizeof *v);
    v->kind = kind;
    v->serial = ++serials;
    snprintf(v->title, sizeof v->title, "%s", title);
    return v;
}

static void pop(void)
{
    if (depth > 1) {
        release(&stack[--depth]);
        scroll_to = stack[depth - 1].scroll;
    }
}

static void render(void);

static void disarm(void)
{
    armed[0] = 0;
    lv_timer_pause(disarm_timer);
}

static void disarm_tick(lv_timer_t *t)
{
    disarm();
    if (app.page == PAGE_SETTINGS)
        render();
}

/* True on the second press of the same row within three seconds. */
static bool confirmed(const char *key)
{
    if (!strcmp(armed, key)) {
        disarm();
        return true;
    }
    snprintf(armed, sizeof armed, "%s", key);
    lv_timer_reset(disarm_timer);
    lv_timer_resume(disarm_timer);
    render();
    return false;
}

static void toast_failure(const cJSON *r)
{
    char error[256] = "";
    if (jstr(r, "error"))
        error_text(jstr(r, "error"), error, sizeof error);
    toast("%s%s%s", L("失败", "Failed"), *error ? ": " : "", error);
}

/* ---------- rows ---------- */

static void on_row(lv_event_t *e);

static lv_obj_t *row(const char *key, const char *icon, const char *label, const char *value, unsigned flags)
{
    lv_obj_t *r = ui_row(body, icon, label, value, flags, NULL, NULL);
    if (!(flags & ROW_INFO) && row_count < ROWS) {
        snprintf(row_keys[row_count], sizeof row_keys[0], "%s", key);
        lv_obj_add_event_cb(r, on_row, LV_EVENT_CLICKED, (void *)(intptr_t)row_count);
        lv_obj_set_user_data(r, (void *)(intptr_t)row_count);
        if (!strcmp(key, focused))
            lv_group_focus_obj(r);
        row_count++;
    }
    if (*armed && !strcmp(armed, key)) {
        lv_obj_set_style_border_color(r, theme.accent, 0);
        lv_obj_set_style_border_width(r, 1, 0);
        lv_obj_t *hint = ui_text(r, 11, false, theme.accent);
        lv_obj_set_width(hint, LV_PCT(100));
        lv_label_set_text(hint, L("再按一次确认", "Press again"));
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
    }
    return r;
}

static void note(const cJSON *label)
{
    char text[1024];
    label_text(label, text, sizeof text);
    if (*text)
        ui_note(body, text);
}

static void value_text(const cJSON *it, char *out, size_t size)
{
    const char *type = jstr(it, "type");
    const cJSON *value = jget(it, "value");
    out[0] = 0;
    if (!type)
        return;
    if (!strcmp(type, "toggle")) {
        snprintf(out, size, "%s", cJSON_IsNull(value) || !value ? "--" : cJSON_IsTrue(value) ? L("开", "On") : L("关", "Off"));
    } else if (!strcmp(type, "choice")) {
        const cJSON *o;
        char v[64];
        snprintf(v, sizeof v, "%s", cJSON_IsString(value) ? value->valuestring : "");
        if (cJSON_IsNumber(value))
            snprintf(v, sizeof v, "%g", value->valuedouble);
        cJSON_ArrayForEach(o, jget(it, "options")) {
            const cJSON *ov = jget(o, "value");
            char s[64];
            snprintf(s, sizeof s, "%s", cJSON_IsString(ov) ? ov->valuestring : "");
            if (cJSON_IsNumber(ov))
                snprintf(s, sizeof s, "%g", ov->valuedouble);
            if (!strcmp(s, v)) {
                label_text(jget(o, "label"), out, size);
                return;
            }
        }
        snprintf(out, size, "%s", *v ? v : "--");
    } else if (!strcmp(type, "number")) {
        if (cJSON_IsNumber(value))
            snprintf(out, size, "%g%s", value->valuedouble, jstr(it, "unit") ? jstr(it, "unit") : "");
        else
            snprintf(out, size, "--");
    } else if (!strcmp(type, "multi")) {
        if (jget(it, "display_value") && !cJSON_IsNull(jget(it, "display_value"))) {
            label_text(jget(it, "display_value"), out, size);
        } else if (!cJSON_GetArraySize(value)) {
            label_text(jget(it, "none_label"), out, size);
            if (!*out)
                snprintf(out, size, "%s", L("未锁定", "Not locked"));
        } else {
            size_t len = 0;
            const cJSON *x, *o;
            cJSON_ArrayForEach(x, value) {
                char name[64] = "";
                cJSON_ArrayForEach(o, jget(it, "options"))
                    if (cJSON_Compare(jget(o, "value"), x, true))
                        label_text(jget(o, "label"), name, sizeof name);
                if (!*name)
                    snprintf(name, sizeof name, "%s", cJSON_IsString(x) ? x->valuestring : "");
                if (!*name && cJSON_IsNumber(x))
                    snprintf(name, sizeof name, "%g", x->valuedouble);
                if (len < size)
                    len += (size_t)snprintf(out + len, size - len, "%s%s", len ? " " : "", name);
            }
        }
    } else if (!strcmp(type, "info")) {
        label_text(value, out, size);
        if (!*out)
            snprintf(out, size, "--");
    }
}

/* ---------- requests ---------- */

static void categories_loaded(int status, cJSON *r, void *ctx)
{
    categories_busy = false;
    shell_page_loading(PAGE_SETTINGS, false);
    shell_page_error(PAGE_SETTINGS, !api_read_ok(status, r));
    if (api_read_ok(status, r)) {
        cJSON_Delete(categories);
        categories = cJSON_Duplicate(jget(r, "categories"), true);
    }
    render();
}

static void load_categories(void)
{
    if (categories || categories_busy)
        return;
    categories_busy = true;
    shell_page_loading(PAGE_SETTINGS, true);
    api_get("/settings", categories_loaded, NULL);
}

/* Loaded into v->data: the category's items, a list, or a whole document. */
static void data_loaded(int status, cJSON *r, void *ctx)
{
    shell_page_loading(PAGE_SETTINGS, false);
    struct view *v = untag(ctx);
    bool ok = api_read_ok(status, r);
    shell_page_error(PAGE_SETTINGS, !ok);
    if (!v)
        return;
    v->loading = false;
    if (ok) {
        const char *field = v->kind == V_CAT ? "items" : v->kind == V_AT ? "presets" : v->kind == V_DEVICES ? "devices" : NULL;
        cJSON_Delete(v->data);
        v->data = cJSON_Duplicate(field ? jget(r, field) : r, true);
        if (field && !v->data)
            v->data = cJSON_CreateArray();
    }
    if (v == top())
        render();
}

static void load(struct view *v, const char *path)
{
    v->loading = true;
    shell_page_loading(PAGE_SETTINGS, true);
    api_get(path, data_loaded, tag(v));
}

static void load_cat(struct view *v)
{
    char p[96];
    snprintf(p, sizeof p, "/settings/%s", v->cat);
    load(v, p);
}

struct post {
    unsigned serial;
    char id[64];
    cJSON *value;
    char job[40], service[16];
    int slot;
    uint32_t until;
};

static void post_finish(struct post *p, const cJSON *r);

static void job_poll(lv_timer_t *t);

static void job_read(int status, cJSON *r, void *ctx)
{
    struct post *p = ctx;
    const char *state = jstr(r, "state");
    if (r && state && (!strcmp(state, "queued") || !strcmp(state, "running")) && lv_tick_get() < p->until) {
        lv_timer_set_repeat_count(lv_timer_create(job_poll, 800, p), 1);
        return;
    }
    cJSON *result = cJSON_CreateObject();
    if (state && !strcmp(state, "done")) {
        cJSON_AddTrueToObject(result, "ok");
    } else {
        cJSON_AddFalseToObject(result, "ok");
        const char *error = jstr(r, "error");
        cJSON_AddStringToObject(result, "error", error ? error : status ? L("操作尚未完成，请刷新实际状态", "Operation incomplete; refresh actual state") : "network");
    }
    struct view *v = find(p->serial);
    if (v)
        load_cat(v);
    post_finish(p, result);
    cJSON_Delete(result);
}

static void job_poll(lv_timer_t *t)
{
    struct post *p = lv_timer_get_user_data(t);
    char path[160];
    snprintf(path, sizeof path, "/network-job?id=%s&slot=%d&service=%s", p->job, p->slot, p->service);
    api_get(path, job_read, p);
}

static void screen_applied(const char *id, const cJSON *value)
{
    cJSON *screen = (cJSON *)jget(app.status, "screen");
    if (!screen)
        return;
    if (!strcmp(id, "brightness") && cJSON_IsNumber(value)) {
        app.brightness = value->valuedouble * 255 / 100 < 1 ? 1 : (int)(value->valuedouble * 255 / 100 + 0.5);
        cJSON_ReplaceItemInObject(screen, "brightness", cJSON_CreateNumber(app.brightness));
        cJSON *b = cJSON_CreateObject();
        cJSON_AddNumberToObject(b, "level", app.brightness);
        api_post("/backlight", b, NULL, NULL);
        return;
    }
    cJSON *copy = cJSON_Duplicate(value, true);
    if (!strcmp(id, "idle") && cJSON_IsString(value)) {
        cJSON_Delete(copy);
        copy = cJSON_CreateNumber(atof(value->valuestring));
    } else if (!strcmp(id, "touch") || !strcmp(id, "clock_seconds")) {
        cJSON_Delete(copy);
        copy = cJSON_CreateBool(cJSON_IsTrue(value) || (cJSON_IsNumber(value) && value->valuedouble));
    }
    if (cJSON_GetObjectItem(screen, id))
        cJSON_ReplaceItemInObject(screen, id, copy);
    else
        cJSON_AddItemToObject(screen, id, copy);
    shell_screen_changed();
}

static void usb_refresh(lv_timer_t *t);

static void post_finish(struct post *p, const cJSON *r)
{
    struct view *v = find(p->serial);
    bool ok = api_reply_ok(r);
    shell_page_loading(PAGE_SETTINGS, false);
    shell_page_error(PAGE_SETTINGS, false);
    if (v)
        v->saving = false;
    const char *reconnect = jstr(r, "reconnect");
    if (!ok)
        toast_failure(r);
    else if (reconnect && !strcmp(reconnect, "deferred"))
        toast("%s", L("设置已保存，模组未就绪或射频已关闭，已跳过重连", "Settings saved; modem not ready or radio off, reconnect skipped"));
    else if (reconnect && !strcmp(reconnect, "failed"))
        toast("%s", L("设置已接受，但重连失败", "Settings accepted, but reconnect failed"));
    else if (jget(r, "refresh_error") && !cJSON_IsNull(jget(r, "refresh_error")) && !cJSON_IsFalse(jget(r, "refresh_error")))
        toast("%s", L("设置已应用，请稍后刷新状态", "Settings applied; refresh status later"));
    else if (v && !strcmp(v->cat, "usb") && !strcmp(p->id, "reset"))
        toast("%s", L("USB 修复已启动", "USB recovery started"));
    else
        toast("%s", L("已保存", "Saved"));

    const cJSON *item = jget(r, "item");
    if (ok && v && !strcmp(v->cat, "traffic_clear")) {
        traffic_invalidate();
        toast("%s", L("流量记录已清空", "Traffic history cleared"));
    }
    if (v && cJSON_IsObject(item)) {
        cJSON *it;
        int i = 0;
        cJSON_ArrayForEach(it, v->data) {
            if (jstr(it, "id") && jstr(item, "id") && !strcmp(jstr(it, "id"), jstr(item, "id"))) {
                cJSON_ReplaceItemInArray(v->data, i, cJSON_Duplicate(item, true));
                break;
            }
            i++;
        }
    }
    if (ok && v && jtrue(item, "reload"))
        load_cat(v);
    if (ok && v && !strcmp(v->cat, "usb") &&
        (!strcmp(p->id, "reset") || !strcmp(p->id, "usb_role") || !strcmp(p->id, "usb_host_boot")))
        lv_timer_set_repeat_count(lv_timer_create(usb_refresh, 2000, (void *)(uintptr_t)v->serial), 1);
    if (ok && v && (!strcmp(v->cat, "screen") || !strcmp(v->cat, "ui") || !strcmp(v->cat, "navigation") ||
                    (!strcmp(v->cat, "system") && !strcmp(p->id, "clock_seconds"))))
        screen_applied(p->id, jget(item, "value") ? jget(item, "value") : p->value);
    struct view *t = top();
    if (ok && t && t->kind == V_EDIT && t != v && find(p->serial) == &stack[depth - 2])
        pop();
    else if (ok && v && v->kind == V_CAT && t == v && jstr(item, "type") == NULL)
        load_cat(v);
    cJSON_Delete(p->value);
    free(p);
    render();
}

static void posted(int status, cJSON *r, void *ctx)
{
    struct post *p = ctx;
    if (api_reply_ok(r) && jtrue(r, "pending") && jstr(r, "job")) {
        snprintf(p->job, sizeof p->job, "%s", jstr(r, "job"));
        snprintf(p->service, sizeof p->service, "%s", jstr(r, "service") ? jstr(r, "service") : "lock");
        p->slot = (int)jnum_or(r, "slot", 0);
        p->until = lv_tick_get() + 245000;
        lv_timer_set_repeat_count(lv_timer_create(job_poll, 800, p), 1);
        return;
    }
    const char *action = jstr(r, "power_action");
    if (api_reply_ok(r) && action && !strcmp(action, p->id)) {
        shell_page_loading(PAGE_SETTINGS, false);
        toast("%s", L("执行中…", "Running…"));
        cJSON_Delete(p->value);
        free(p);
        return;
    }
    post_finish(p, r);
}

static void save(struct view *v, const char *id, const cJSON *value)
{
    struct post *p = calloc(1, sizeof *p);
    p->serial = v->serial;
    snprintf(p->id, sizeof p->id, "%s", id);
    p->value = value ? cJSON_Duplicate(value, true) : NULL;
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "id", id);
    if (value)
        cJSON_AddItemToObject(b, "value", cJSON_Duplicate(value, true));
    const cJSON *it;
    cJSON_ArrayForEach(it, v->data)
        if (jstr(it, "id") && !strcmp(jstr(it, "id"), id) && jget(it, "slot") && !cJSON_IsNull(jget(it, "slot")))
            cJSON_AddItemToObject(b, "slot", cJSON_Duplicate(jget(it, "slot"), true));
    v->saving = true;
    shell_page_loading(PAGE_SETTINGS, true);
    char path[96];
    snprintf(path, sizeof path, "/settings/%s", v->cat);
    api_post(path, b, posted, p);
}

static void usb_refresh_loaded(int status, cJSON *r, void *ctx)
{
    data_loaded(status, r, ctx);
    struct view *v = untag(ctx);
    const cJSON *it, *items = v ? v->data : NULL;
    cJSON_ArrayForEach(it, items) {
        const char *id = jstr(it, "id");
        if (id && (!strcmp(id, "reset_status") || !strcmp(id, "usb_role_state")) && jtrue(it, "busy")) {
            lv_timer_set_repeat_count(lv_timer_create(usb_refresh, 2000, ctx), 1);
            return;
        }
    }
}

static void usb_refresh(lv_timer_t *t)
{
    struct view *v = untag(lv_timer_get_user_data(t));
    if (!v)
        return;
    v->loading = true;
    shell_page_loading(PAGE_SETTINGS, true);
    api_get("/settings/usb", usb_refresh_loaded, tag(v));
}

/* ---------- views ---------- */

static void render_menu(void)
{
    const cJSON *c;
    int i = 0;
    cJSON_ArrayForEach(c, categories) {
        bool hidden = (jget(c, "plugin") && !cJSON_IsNull(jget(c, "plugin"))) ||
                      (jget(c, "parent") && !cJSON_IsNull(jget(c, "parent"))) ||
                      (jstr(c, "view") && !strcmp(jstr(c, "view"), "apps"));
        char key[32], label[128];
        snprintf(key, sizeof key, "cat:%d", i++);
        label_text(jget(c, "label"), label, sizeof label);
        if (!hidden)
            row(key, icon_for(jstr(c, "id"), NULL), label, NULL, ROW_CHEVRON);
    }
}

static void render_cat(struct view *v)
{
    const cJSON *it;
    cJSON_ArrayForEach(it, v->data) {
        const char *type = jstr(it, "type") ? jstr(it, "type") : "";
        const char *id = jstr(it, "id") ? jstr(it, "id") : "";
        /* Not offered here: the opacity and density preferences, the tip row and
         * the disclaimer. */
        if (!strcmp(id, "ui_opacity") || !strcmp(id, "ui_density") || !strcmp(id, "donate") ||
            !strcmp(id, "disclaimer"))
            continue;
        char key[96], label[256], value[512];
        snprintf(key, sizeof key, "item:%s", id);
        label_text(jget(it, "label"), label, sizeof label);
        /* The backend names the shared package; this UI is its LVGL rewrite. */
        if (!strcmp(id, "name"))
            snprintf(value, sizeof value, "%s", "e5-infoscreen-lvgl");
        else
            value_text(it, value, sizeof value);
        const char *icon = icon_for(id, icon_for(v->cat, NULL));
        if (!strncmp(id, "nav_slot", 8)) {
            int page = atoi(jstr(it, "value") ? jstr(it, "value") : "0");
            icon = page >= 0 && page < PAGE_COUNT ? page_icons[page] : "layout";
        }
        unsigned flags = !strcmp(type, "info") ? ROW_INFO : 0;
        if (!strcmp(type, "toggle") && jtrue(it, "value"))
            flags |= ROW_ON;
        if (!strcmp(type, "choice") || !strcmp(type, "number") || !strcmp(type, "multi") || !strcmp(type, "image") ||
            !strcmp(type, "category"))
            flags |= ROW_CHEVRON;
        row(key, icon, label, value, flags);
        note(jget(it, "note"));
    }
}

static bool same_value(const cJSON *a, const cJSON *b)
{
    char x[64] = "", y[64] = "";
    if (cJSON_IsString(a))
        snprintf(x, sizeof x, "%s", a->valuestring);
    else if (cJSON_IsNumber(a))
        snprintf(x, sizeof x, "%g", a->valuedouble);
    if (cJSON_IsString(b))
        snprintf(y, sizeof y, "%s", b->valuestring);
    else if (cJSON_IsNumber(b))
        snprintf(y, sizeof y, "%g", b->valuedouble);
    return !strcmp(x, y);
}

static bool in_draft(const cJSON *draft, const cJSON *value)
{
    const cJSON *x;
    cJSON_ArrayForEach(x, draft)
        if (cJSON_Compare(x, value, true))
            return true;
    return false;
}

static void render_edit(struct view *v)
{
    const char *type = jstr(v->item, "type");
    const char *id = jstr(v->item, "id") ? jstr(v->item, "id") : "";
    const cJSON *o;
    int i = 0;
    if (!strcmp(type, "choice")) {
        cJSON_ArrayForEach(o, jget(v->item, "options")) {
            char key[32], label[128];
            snprintf(key, sizeof key, "opt:%d", i++);
            label_text(jget(o, "label"), label, sizeof label);
            const char *icon = icon_for(id, NULL);
            if (!strcmp(id, "ui_theme"))
                icon = jstr(o, "value") && !strcmp(jstr(o, "value"), "light") ? "sun" : "moon";
            else if (!strncmp(id, "nav_slot", 8) && jstr(o, "value"))
                icon = page_icons[atoi(jstr(o, "value")) % PAGE_COUNT];
            row(key, icon, label, NULL, same_value(jget(o, "value"), jget(v->item, "value")) ? ROW_CHECK : 0);
        }
    } else if (!strcmp(type, "number")) {
        lv_obj_t *card = ui_card(body);
        lv_obj_t *n = ui_text(card, 28, true, theme.text);
        lv_obj_set_style_align(n, LV_ALIGN_CENTER, 0);
        lv_label_set_text_fmt(n, "%g%s", v->draft ? v->draft->valuedouble : 0, jstr(v->item, "unit") ? jstr(v->item, "unit") : "");
        lv_obj_t *buttons = ui_box(body, LV_FLEX_FLOW_ROW, 14);
        row("num:-", "minus", "−", NULL, 0);
        row("num:+", "plus", "+", NULL, 0);
        lv_obj_t *minus = lv_obj_get_child(body, -2), *plus = lv_obj_get_child(body, -1);
        lv_obj_set_parent(minus, buttons);
        lv_obj_set_parent(plus, buttons);
        lv_obj_set_flex_grow(minus, 1);
        lv_obj_set_flex_grow(plus, 1);
        row("num:save", "check", L("保存", "Save"), NULL, 0);
    } else if (!strcmp(type, "multi")) {
        cJSON_ArrayForEach(o, jget(v->item, "options")) {
            char key[32], label[128];
            snprintf(key, sizeof key, "chk:%d", i++);
            label_text(jget(o, "label"), label, sizeof label);
            row(key, icon_for(id, NULL), label, NULL, in_draft(v->draft, jget(o, "value")) ? ROW_CHECK : 0);
        }
        row("multi:clear", "trash", L("全部取消", "Clear all"), NULL, 0);
        row("multi:apply", "check", L("应用", "Apply"), cJSON_GetArraySize(v->draft) ? NULL : L("未锁定", "Not locked"), 0);
    }
    note(jget(v->item, "note"));
}

static void render_at(struct view *v)
{
    if (!v->data)
        return;
    char slot[16];
    snprintf(slot, sizeof slot, "SIM %d", v->slot + 1);
    row("atslot:", "simcard", L("AT 卡槽", "AT SIM"), slot, ROW_CHEVRON);
    const cJSON *p;
    int i = 0;
    cJSON_ArrayForEach(p, v->data) {
        char key[32], label[128];
        snprintf(key, sizeof key, "atp:%d", i++);
        label_text(jget(p, "label"), label, sizeof label);
        lv_obj_t *r = row(key, "terminal", label, NULL, 0);
        lv_obj_t *cmd = ui_text(r, 11, false, theme.dim);
        lv_obj_set_width(cmd, LV_PCT(100));
        lv_label_set_text(cmd, jstr(p, "cmd") ? jstr(p, "cmd") : "");
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
    }
    ui_note(body, L("任意指令可通过 SSH 的 e5-at 或插件的 /api/at 发送", "Any command: e5-at over SSH, or /api/at from a plugin"));
}

static void render_atres(struct view *v)
{
    lv_obj_t *card = ui_card(body);
    lv_obj_t *cmd = ui_text(card, 12, true, theme.accent);
    lv_label_set_text(cmd, v->text);
    lv_obj_t *out = ui_text(card, 12, false, theme.text);
    lv_obj_set_width(out, LV_PCT(100));
    lv_label_set_long_mode(out, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(out, v->reply ? v->reply : L("执行中…", "Running…"));
    if (v->warn)
        ui_note(body, L("该指令可能让基带直到重启前不再响应 AT 或识别不到 SIM 卡",
                        "This command may leave the baseband unresponsive to AT or unaware of the SIM until a reboot"));
    row("atagain:", "refresh", L("再执行一次", "Run again"), NULL, 0);
}

static void render_devices(struct view *v)
{
    if (!v->data)
        return;
    if (!cJSON_GetArraySize(v->data))
        ui_note(body, L("没有设备", "No devices"));
    const cJSON *d;
    int i = 0;
    cJSON_ArrayForEach(d, v->data) {
        char key[32], value[64];
        snprintf(key, sizeof key, "dev:%d", i++);
        const char *name = jstr(d, "name") ? jstr(d, "name") : jstr(d, "ip") ? jstr(d, "ip") : jstr(d, "mac");
        const char *via = jstr(d, "via") && !strcmp(jstr(d, "via"), "usb") ? "USB" : "Wi-Fi";
        if (jtrue(d, "blocked"))
            snprintf(value, sizeof value, "%s", L("已禁止上网", "Blocked"));
        else if (jtrue(d, "online"))
            snprintf(value, sizeof value, "%s · %s", L("在线", "Online"), via);
        else
            snprintf(value, sizeof value, "%s", L("离线", "Offline"));
        row(key, "device", name ? name : "", value, ROW_CHEVRON | (jtrue(d, "online") && !jtrue(d, "blocked") ? ROW_ON : 0));
    }
}

static void render_device(struct view *v)
{
    const cJSON *d = v->item;
    char via[64];
    const char *kind = jstr(d, "via") && !strcmp(jstr(d, "via"), "usb") ? "USB" : "Wi-Fi";
    double signal;
    if (!jtrue(d, "online"))
        snprintf(via, sizeof via, "%s", L("离线", "Offline"));
    else if (jnum(d, "signal", &signal) && signal)
        snprintf(via, sizeof via, "%s · %g dBm", kind, signal);
    else
        snprintf(via, sizeof via, "%s", kind);
    row("info:mac", "device", "MAC", jstr(d, "mac"), ROW_INFO);
    row("info:ip", "globe", "IP", jstr(d, "ip") ? jstr(d, "ip") : "--", ROW_INFO);
    row("info:via", "wifi", L("连接", "Via"), via, ROW_INFO);
    if (jtrue(d, "blocked"))
        row("act:unblock", "shield", L("允许上网", "Allow internet"), NULL, 0);
    else
        row("act:block", "shield", L("禁止上网", "Block internet"), NULL, 0);
    if (jtrue(d, "online") && jstr(d, "via") && !strcmp(jstr(d, "via"), "wifi"))
        row("act:kick", "shield", L("踢下 Wi-Fi", "Kick off Wi-Fi"), NULL, 0);
}

static const char *bt_word(const cJSON *d)
{
    const char *state = jstr(d, "state");
    if (state && !strncmp(state, "failed", 6))
        return L("连接失败", "Failed");
    if (state && !strcmp(state, "pairing"))
        return L("配对中…", "Pairing…");
    if (state && !strcmp(state, "connecting"))
        return L("连接中…", "Connecting…");
    if (jtrue(d, "connected"))
        return L("已连接", "Connected");
    return jtrue(d, "paired") ? L("已配对", "Paired") : "";
}

static void render_bt(struct view *v)
{
    const cJSON *b = v->data;
    if (!b)
        return;
    if (!jtrue(b, "adapter")) {
        ui_note(body, L("没有蓝牙适配器", "No Bluetooth adapter"));
        return;
    }
    bool powered = jtrue(b, "powered");
    row("bt:power", "bluetooth", L("蓝牙", "Bluetooth"), powered ? L("开", "On") : L("关", "Off"), powered ? ROW_ON : 0);
    const cJSON *autostart = jget(b, "autostart");
    if (cJSON_IsBool(autostart))
        row("bt:autostart", "power", L("开机启动", "On at boot"), cJSON_IsTrue(autostart) ? L("开", "On") : L("关", "Off"),
            cJSON_IsTrue(autostart) ? ROW_ON : 0);
    if (powered)
        row("bt:scan", "search", jtrue(b, "discovering") ? L("正在搜索…", "Searching…") : L("搜索设备", "Search"), NULL, 0);
    if (jstr(b, "name"))
        row("info:btname", "bluetooth", L("本机名称", "Name"), jstr(b, "name"), ROW_INFO);
    for (int pass = 0; pass < 2; pass++) {
        bool any = false;
        const cJSON *d;
        cJSON_ArrayForEach(d, jget(b, "devices")) {
            bool mine = jtrue(d, "paired") || jtrue(d, "connected");
            if (mine != (pass == 0))
                continue;
            if (!any)
                ui_heading(body, pass ? L("附近的设备", "Nearby") : L("我的设备", "My devices"));
            any = true;
            char key[64];
            snprintf(key, sizeof key, "btd:%s", jstr(d, "mac") ? jstr(d, "mac") : "");
            row(key, "bluetooth", jstr(d, "name") ? jstr(d, "name") : jstr(d, "mac"), bt_word(d),
                ROW_CHEVRON | (jtrue(d, "connected") ? ROW_ON : 0));
        }
        if (pass && powered && !any) {
            ui_heading(body, L("附近的设备", "Nearby"));
            ui_note(body, jtrue(b, "discovering") ? L("正在搜索…", "Searching…") : L("没有找到设备，点“搜索设备”", "Nothing found; press Search"));
        }
    }
    ui_note(body, L("耳机或音箱请先进入配对模式；连接后声音从蓝牙播放",
                    "Put headphones or a speaker in pairing mode first; once connected the sound plays there"));
}

static void render_btdev(struct view *v)
{
    struct view *parent = depth >= 2 ? &stack[depth - 2] : NULL;
    const cJSON *d = NULL, *x, *devices = parent ? jget(parent->data, "devices") : NULL;
    cJSON_ArrayForEach(x, devices)
        if (jstr(x, "mac") && !strcmp(jstr(x, "mac"), v->text))
            d = x;
    row("info:btmac", "bluetooth", "MAC", v->text, ROW_INFO);
    row("info:btst", "info", L("状态", "State"), d ? bt_word(d) : "", ROW_INFO);
    if (jtrue(d, "connected"))
        row("bta:disconnect", "bluetooth", L("断开", "Disconnect"), NULL, 0);
    else
        row("bta:connect", "bluetooth", jtrue(d, "paired") ? L("连接", "Connect") : L("配对并连接", "Pair and connect"), NULL, 0);
    if (jtrue(d, "paired"))
        row("bta:remove", "trash", L("忘记此设备", "Forget"), NULL, 0);
    const char *state = jstr(d, "state");
    if (state && !strncmp(state, "failed", 6)) {
        const char *reason = state + 6;
        while (*reason == ':' || *reason == ' ')
            reason++;
        char text[256];
        if (!strcmp(reason, "notfound"))
            snprintf(text, sizeof text, "%s", L("没有找到设备：请让它进入配对模式后再试", "Not found: put it in pairing mode and try again"));
        else if (!strcmp(reason, "forgot"))
            snprintf(text, sizeof text, "%s", L("设备已忘记配对：请让它进入配对模式后再连接", "The device forgot the pairing: put it in pairing mode and connect again"));
        else if (!strcmp(reason, "noanswer"))
            snprintf(text, sizeof text, "%s", L("设备没有响应：请打开耳机盒、戴上耳机，并确认它没有连着手机",
                                                "No answer: take the earbuds out of the case and make sure no phone is connected to them"));
        else
            snprintf(text, sizeof text, "%s%s%s", L("连接失败", "Failed"), *reason ? ": " : "", reason);
        ui_note(body, text);
    }
}

/* ---------- APN: the same RPC as the web interface ---------- */

static const char *apn_text(const char *key)
{
    static const char *const texts[][3] = {
        {"apn_invalid", "接入点参数无效", "Invalid access point settings"},
        {"apn_missing", "接入点已不存在，请刷新", "Access point no longer exists; refresh"},
        {"apn_identity", "无法读取当前 SIM 身份，请稍后刷新", "Current SIM identity unavailable; refresh later"},
        {"apn_sim_changed", "上网卡已改变，请刷新后重新确认", "Data SIM changed; refresh and confirm again"},
        {"apn_unavailable", "APN 服务暂不可用，请重试", "APN service unavailable; retry"},
        {"apn_storage", "接入点保存失败", "Could not save access point"},
        {"apn_auth_unsupported", "此固件暂不支持 APN 鉴权", "APN authentication is unsupported by this firmware"},
        {"apn_reload", "配置已保存，但应用失败，请刷新后重试", "Configuration saved, but applying failed; refresh and retry"},
        {"apn_busy", "另一项网络操作仍在执行，请稍后重试", "Another network operation is running; retry later"},
        {"apn_bound", "此接入点仍被 SIM 使用，不能删除", "A SIM still uses this access point; cannot delete"},
        {"apn_current", "请先切换，再删除当前接入点", "Switch before deleting the current access point"},
        {"apn_duplicate", "相同配置的接入点已存在", "An access point with these settings already exists"},
        {"apn_limit", "最多保存 32 个接入点", "Up to 32 access points can be saved"},
        {"apn_unsaved", "网络配置有未应用更改，请先保存或撤销", "Save or discard pending network changes first"},
        {"apn_scope", "当前 SIM 已保存选择，请按卡应用", "This SIM has a saved selection; apply per SIM"},
        {"apn_unsupported", "当前蜂窝接口不支持 APN 管理", "Cellular interface does not support APN management"},
    };
    for (size_t i = 0; key && i < sizeof texts / sizeof *texts; i++)
        if (!strcmp(key, texts[i][0]))
            return L(texts[i][1], texts[i][2]);
    return L("APN 服务暂不可用，请重试", "APN service unavailable; retry");
}

static const char *apn_protocol(const char *pdptype)
{
    return pdptype && !strcmp(pdptype, "IP") ? "IPv4" : "IPv4 / IPv6";
}

static void apn_loaded(int status, cJSON *r, void *ctx)
{
    shell_page_loading(PAGE_SETTINGS, false);
    struct view *v = untag(ctx);
    if (!v)
        return;
    v->loading = false;
    if (api_reply_ok(r)) {
        cJSON_Delete(v->data);
        v->data = cJSON_Duplicate(r, true);
        v->text[0] = 0;
        if (v->slot < 0)
            v->slot = (int)jnum_or(r, "data_sim", -1);
    } else {
        snprintf(v->text, sizeof v->text, "%s", jstr(r, "error") ? jstr(r, "error") : "apn_unavailable");
    }
    if (v == top())
        render();
}

static void apn_load(struct view *v)
{
    if (v->loading || v->saving)
        return;
    v->loading = true;
    shell_page_loading(PAGE_SETTINGS, true);
    api_get("/apn", apn_loaded, tag(v));
}

static void apn_changed(int status, cJSON *r, void *ctx)
{
    shell_page_loading(PAGE_SETTINGS, false);
    struct view *v = untag(ctx);
    toast("%s", api_reply_ok(r) ? L("已保存", "Saved") : apn_text(jstr(r, "error")));
    if (!v)
        return;
    v->saving = false;
    struct view *t = top();
    if (api_reply_ok(r) && t->kind == V_APNEDIT && t != v)
        pop();
    const char *error = jstr(r, "error");
    if (api_reply_ok(r) || jtrue(r, "saved") || (error && !strcmp(error, "apn_sim_changed")))
        apn_load(v);
    render();
}

static void apn_change(struct view *v, const char *action, cJSON *payload)
{
    v->saving = true;
    disarm();
    shell_page_loading(PAGE_SETTINGS, true);
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "action", action);
    cJSON_AddItemToObject(b, "payload", payload);
    api_post("/apn", b, apn_changed, tag(v));
    render();
}

static void apn_button(lv_obj_t *parent, const char *key, const char *label, bool disabled, bool active)
{
    lv_obj_t *b = row(key, NULL, label, NULL, 0);
    lv_obj_set_parent(b, parent);
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_style_pad_ver(b, S(10), 0);
    lv_obj_set_style_pad_hor(b, S(8), 0);
    lv_obj_set_style_bg_color(b, active ? theme.active : theme.button, 0);
    lv_obj_set_style_text_align(lv_obj_get_child(b, 0), LV_TEXT_ALIGN_CENTER, 0);
    if (active)
        lv_obj_set_style_text_color(lv_obj_get_child(b, 0), theme.accent, 0);
    lv_obj_set_state(b, LV_STATE_DISABLED, disabled);
}

static void render_apn(struct view *v)
{
    const cJSON *data = v->data;
    bool busy = v->saving || v->loading, ok = api_reply_ok(data);
    lv_obj_t *bar = ui_box(body, LV_FLEX_FLOW_ROW, 8);
    apn_button(bar, "apn:refresh", L("刷新", "Refresh"), busy, false);
    apn_button(bar, "apn:add", L("添加接入点", "Add access point"), busy || !ok, false);
    if (*v->text) {
        lv_obj_t *n = ui_note(body, apn_text(v->text));
        lv_obj_set_style_text_color(n, theme.poor, 0);
    }
    if (!ok)
        return;
    double sim = jnum_or(data, "data_sim", -1);
    bool ready = jtrue(data, "identity_ready") && (sim == 0 || sim == 1);
    lv_obj_t *card = ui_card(body);
    if (sim == 0 || sim == 1)
        lv_label_set_text_fmt(ui_text(card, 14, true, theme.text), "SIM %g", sim + 1);
    else
        lv_label_set_text(ui_text(card, 14, true, theme.text), "SIM —");
    lv_label_set_text(ui_text(card, 11, false, theme.dim), L("当前配置", "Current configuration"));
    const char *current = jstr(data, "current.apn");
    lv_label_set_text(ui_text(card, 15, true, theme.accent), current && *current ? current : L("SIM 默认", "SIM default"));
    lv_label_set_text(ui_text(card, 11, false, theme.dim), apn_protocol(jstr(data, "current.pdptype")));
    lv_label_set_text(ui_text(card, 10, false, theme.dim), jtrue(data, "remembered") ? L("已按当前 SIM 保存选择", "Selection saved for this SIM")
                                                                                    : L("正在使用全局接口配置", "Using global interface configuration"));
    lv_obj_t *autobar = ui_box(body, LV_FLEX_FLOW_ROW, 8);
    apn_button(autobar, "apn:auto", L("自动匹配当前 SIM", "Match current SIM automatically"), busy || !ready, false);
    ui_note(body, L("应用于当前上网卡，不切换 SIM。切换 APN 会短暂中断蜂窝连接。",
                    "Applies to the data SIM without switching SIMs. Changing APN briefly interrupts cellular data."));
    ui_note(body, L("按运营商匹配公网 APN，未知运营商使用 SIM 默认。专网或物联卡请手动设置。",
                    "Matches public APNs by operator; unknown operators use SIM defaults. Configure private APNs manually."));
    if (!ready)
        ui_note(body, apn_text("apn_identity"));
    bool auth = jtrue(data, "auth_supported");
    if (!auth)
        ui_note(body, L("当前固件暂不支持 APN 鉴权；可以保存配置，但不能应用需要鉴权的接入点。",
                        "This firmware cannot apply authenticated APNs yet. Such profiles can still be saved."));
    if (!cJSON_GetArraySize(jget(data, "profiles")))
        ui_note(body, L("暂无接入点，请添加", "No access points; add one"));
    const cJSON *p;
    int i = 0;
    cJSON_ArrayForEach(p, jget(data, "profiles")) {
        const char *kind = jstr(p, "auth");
        bool authed = kind && strcmp(kind, "none");
        lv_obj_t *profile = ui_card(body);
        if (jtrue(p, "current")) {
            lv_obj_set_style_border_color(profile, theme.accent, 0);
            lv_obj_set_style_border_width(profile, 1, 0);
        }
        lv_obj_t *head = ui_box(profile, LV_FLEX_FLOW_ROW, 8);
        const char *name = jstr(p, "name") ? jstr(p, "name") : "";
        bool automatic = jstr(p, "id") && !strcmp(jstr(p, "id"), "automatic") && !strcmp(name, "SIM default");
        lv_label_set_text(ui_text(head, 14, true, theme.text), automatic ? L("SIM 默认", "SIM default") : name);
        if (jtrue(p, "current"))
            lv_label_set_text(ui_text(head, 10, false, theme.accent), L("当前使用", "Current"));
        const char *apn = jstr(p, "apn");
        lv_label_set_text(ui_text(profile, 13, false, theme.text), apn && *apn ? apn : L("SIM 默认", "SIM default"));
        char upper[16] = "";
        for (size_t c = 0; authed && kind[c] && c + 1 < sizeof upper; c++)
            upper[c] = (char)(kind[c] >= 'a' && kind[c] <= 'z' ? kind[c] - 32 : kind[c]);
        lv_label_set_text_fmt(ui_text(profile, 11, false, theme.dim), "%s · %s", apn_protocol(jstr(p, "pdptype")),
                              authed ? upper : L("无鉴权", "None"));
        lv_obj_t *actions = ui_box(profile, LV_FLEX_FLOW_ROW, 6);
        char key[32];
        snprintf(key, sizeof key, "apn:use:%d", i);
        apn_button(actions, key, L("切换", "Use"), busy || !ready || (!auth && authed), false);
        snprintf(key, sizeof key, "apn:edit:%d", i);
        apn_button(actions, key, L("编辑", "Edit"), busy, false);
        snprintf(key, sizeof key, "apn:delete:%d", i);
        apn_button(actions, key, L("删除", "Delete"), busy || jtrue(p, "current"), false);
        i++;
    }
}

static void on_field(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target_obj(e);
    struct view *v = top();
    const char *field = lv_obj_get_user_data(ta);
    if (v && v->kind == V_APNEDIT && field)
        cJSON_ReplaceItemInObject(v->draft, field, cJSON_CreateString(lv_textarea_get_text(ta)));
}

static void field(const char *name, const char *label, int max, bool password, const char *placeholder)
{
    struct view *v = top();
    lv_obj_t *box = ui_box(body, LV_FLEX_FLOW_COLUMN, 4);
    lv_label_set_text(ui_text(box, 12, false, theme.dim), label);
    lv_obj_t *ta = lv_textarea_create(box);
    lv_obj_set_width(ta, LV_PCT(100));
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, (uint32_t)max);
    lv_textarea_set_password_mode(ta, password);
    lv_obj_set_style_text_font(ta, F(14), 0);
    lv_obj_set_style_text_color(ta, theme.text, 0);
    lv_obj_set_style_bg_color(ta, theme.surface, 0);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(ta, theme.line, 0);
    lv_obj_set_style_border_color(ta, theme.accent, LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(ta, 1, 0);
    lv_obj_set_style_radius(ta, S(10), 0);
    lv_obj_set_style_pad_all(ta, S(8), 0);
    if (placeholder)
        lv_textarea_set_placeholder_text(ta, placeholder);
    lv_textarea_set_text(ta, jstr(v->draft, name) ? jstr(v->draft, name) : "");
    lv_obj_set_user_data(ta, (void *)name);
    lv_obj_add_event_cb(ta, on_field, LV_EVENT_VALUE_CHANGED, NULL);
    keyboard_bind(ta);
    lv_obj_set_state(ta, LV_STATE_DISABLED, stack[depth - 2].saving);
    ui_focusable(ta);
}

static void choices(const char *name, const char *label, const char *const options[][2], int count)
{
    struct view *v = top();
    lv_label_set_text(ui_text(body, 12, false, theme.dim), label);
    lv_obj_t *bar = ui_box(body, LV_FLEX_FLOW_ROW_WRAP, 6);
    const char *current = jstr(v->draft, name);
    for (int i = 0; i < count; i++) {
        char key[48];
        snprintf(key, sizeof key, "apnfield:%s:%s", name, options[i][0]);
        apn_button(bar, key, options[i][1], stack[depth - 2].saving, current && !strcmp(current, options[i][0]));
    }
}

static void render_apnedit(struct view *v)
{
    static const char *const protocols[][2] = {{"IPV4V6", "IPv4 / IPv6"}, {"IP", "IPv4"}};
    const char *const auths[][2] = {{"none", L("无鉴权", "None")}, {"pap", "PAP"}, {"chap", "CHAP"}, {"papchap", "PAP / CHAP"}};
    field("name", L("名称", "Name"), 64, false, NULL);
    field("apn", "APN", 100, false, NULL);
    choices("pdptype", L("IP 协议", "IP protocol"), protocols, 2);
    choices("auth", L("鉴权方式", "Authentication"), auths, 4);
    field("username", L("用户名", "Username"), 64, false, NULL);
    field("password", L("密码", "Password"), 64, true, v->has_password ? L("留空保留原密码", "Leave blank to keep saved password") : NULL);
    if (v->has_password)
        row("apnfield:clear_password:toggle", "trash", L("清除已保存密码", "Clear saved password"), NULL,
            jtrue(v->draft, "clear_password") ? ROW_CHECK : 0);
    ui_note(body, L("保存只更新列表；切换后才会应用。", "Saving only updates the list; choose Use to apply."));
    lv_obj_t *bar = ui_box(body, LV_FLEX_FLOW_ROW, 8);
    apn_button(bar, "apn:save", L("保存", "Save"), stack[depth - 2].saving, false);
    apn_button(bar, "apn:cancel", L("取消", "Cancel"), stack[depth - 2].saving, false);
}

static void apn_edit(struct view *v, const cJSON *p)
{
    struct view *e = push(V_APNEDIT, p ? L("编辑", "Edit") : L("添加接入点", "Add access point"));
    e->has_password = jtrue(p, "has_password");
    e->draft = cJSON_CreateObject();
    static const char *const fields[] = {"id", "name", "apn", "username"};
    for (int i = 0; i < 4; i++)
        cJSON_AddStringToObject(e->draft, fields[i], jstr(p, fields[i]) ? jstr(p, fields[i]) : "");
    cJSON_AddStringToObject(e->draft, "pdptype", jstr(p, "pdptype") ? jstr(p, "pdptype") : "IPV4V6");
    cJSON_AddStringToObject(e->draft, "auth", jstr(p, "auth") ? jstr(p, "auth") : "none");
    cJSON_AddStringToObject(e->draft, "password", "");
    cJSON_AddFalseToObject(e->draft, "clear_password");
}

static void trimmed(cJSON *payload, const char *name, const char *value)
{
    while (*value == ' ')
        value++;
    size_t n = strlen(value);
    while (n && value[n - 1] == ' ')
        n--;
    char text[128];
    snprintf(text, sizeof text, "%.*s", (int)n, value);
    cJSON_AddStringToObject(payload, name, text);
}

static void apn_save(struct view *e)
{
    const cJSON *d = e->draft;
    const char *name = jstr(d, "name") ? jstr(d, "name") : "";
    while (*name == ' ')
        name++;
    if (!*name) {
        toast("%s", apn_text("apn_invalid"));
        return;
    }
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "id", jstr(d, "id"));
    trimmed(payload, "name", jstr(d, "name"));
    trimmed(payload, "apn", jstr(d, "apn"));
    cJSON_AddStringToObject(payload, "pdptype", jstr(d, "pdptype"));
    cJSON_AddStringToObject(payload, "auth", jstr(d, "auth"));
    cJSON_AddStringToObject(payload, "username", jstr(d, "username"));
    cJSON_AddStringToObject(payload, "roaming", jstr(d, "pdptype"));
    cJSON_AddStringToObject(payload, "attach", "0");
    bool clear = jtrue(d, "clear_password");
    if (clear || *jstr(d, "password") || !e->has_password)
        cJSON_AddStringToObject(payload, "password", clear ? "" : jstr(d, "password"));
    keyboard_close();
    apn_change(&stack[depth - 2], "save", payload);
}

static void apn_clicked(struct view *v, const char *key)
{
    char kind[16] = "", action[32] = "", arg[32] = "";
    sscanf(key, "%15[^:]:%31[^:]:%31s", kind, action, arg);
    if (v->saving || v->loading || (v->kind == V_APNEDIT && stack[depth - 2].saving))
        return;
    if (!strcmp(kind, "apnfield") && v->kind == V_APNEDIT) {
        if (!strcmp(action, "clear_password"))
            cJSON_ReplaceItemInObject(v->draft, action, cJSON_CreateBool(!jtrue(v->draft, action)));
        else
            cJSON_ReplaceItemInObject(v->draft, action, cJSON_CreateString(arg));
        render();
        return;
    }
    if (!strcmp(action, "cancel")) {
        keyboard_close();
        pop();
        render();
    } else if (!strcmp(action, "save") && v->kind == V_APNEDIT) {
        apn_save(v);
    } else if (!strcmp(action, "refresh")) {
        disarm();
        apn_load(v);
    } else if (v->kind == V_APN) {
        const cJSON *p = cJSON_GetArrayItem(jget(v->data, "profiles"), atoi(arg));
        if (!strcmp(action, "add") || !strcmp(action, "edit")) {
            if (!api_reply_ok(v->data) || (!strcmp(action, "edit") && !p))
                return;
            disarm();
            apn_edit(v, p);
            render();
            return;
        }
        if (strcmp(action, "auto") && !p)
            return;
        if (!confirmed(key))
            return;
        cJSON *payload = cJSON_CreateObject();
        if (!strcmp(action, "delete")) {
            cJSON_AddStringToObject(payload, "id", jstr(p, "id") ? jstr(p, "id") : "");
            apn_change(v, "delete", payload);
            return;
        }
        double sim = jnum_or(v->data, "data_sim", -1);
        if (!jtrue(v->data, "identity_ready") || (sim != 0 && sim != 1)) {
            cJSON_Delete(payload);
            toast("%s", apn_text("apn_identity"));
            return;
        }
        if (p)
            cJSON_AddStringToObject(payload, "id", jstr(p, "id") ? jstr(p, "id") : "");
        cJSON_AddTrueToObject(payload, "remember");
        cJSON_AddNumberToObject(payload, "data_sim", sim);
        apn_change(v, !strcmp(action, "auto") ? "auto" : "apply", payload);
    }
}

/* The data SIM changed elsewhere: reread, without polling AT. */
static void apn_status(const cJSON *st)
{
    struct view *v = top();
    double slot;
    if (!v || v->kind != V_APN || app.page != PAGE_SETTINGS || app.blank || !api_reply_ok(v->data) ||
        !jnum(st, "modem.sim_card", &slot) || (slot != 0 && slot != 1) || (int)slot == v->slot || v->saving || v->loading)
        return;
    v->slot = (int)slot;
    apn_load(v);
}
static void render(void)
{
    struct view *v = top();
    if (!v || !body)
        return;
    lv_obj_t *was = lv_group_get_focused(lv_group_get_default());
    if (was && lv_obj_get_parent(was) == body && lv_obj_get_user_data(was))
        snprintf(focused, sizeof focused, "%s", row_keys[(intptr_t)lv_obj_get_user_data(was)]);
    char crumb[512] = "";
    size_t len = 0;
    for (int i = 0; i < depth && len < sizeof crumb; i++)
        len += (size_t)snprintf(crumb + len, sizeof crumb - len, "%s%s", i ? " › " : "", stack[i].title);
    ui_set_text(path, crumb);
    keyboard_close();
    lv_obj_clean(body);
    row_count = 0;
    switch (v->kind) {
    case V_MENU: render_menu(); break;
    case V_CAT: render_cat(v); break;
    case V_EDIT: render_edit(v); break;
    case V_AT: render_at(v); break;
    case V_ATRES: render_atres(v); break;
    case V_DEVICES: render_devices(v); break;
    case V_DEVICE: render_device(v); break;
    case V_BT: render_bt(v); break;
    case V_BTDEV: render_btdev(v); break;
    case V_APN: render_apn(v); break;
    case V_APNEDIT: render_apnedit(v); break;
    }
    focused[0] = 0;
    if (scroll_to >= 0) {
        lv_obj_update_layout(body);
        lv_obj_scroll_to_y(lv_obj_get_parent(body), scroll_to, LV_ANIM_OFF);
        scroll_to = -1;
    }
}

/* ---------- actions ---------- */

static void open_category(const cJSON *c)
{
    char label[128];
    label_text(jget(c, "label"), label, sizeof label);
    const char *kind = jstr(c, "view") ? jstr(c, "view") : "";
    enum kind k = !strcmp(kind, "at") ? V_AT : !strcmp(kind, "bluetooth") ? V_BT : !strcmp(kind, "devices") ? V_DEVICES
                : !strcmp(kind, "apn") ? V_APN : V_CAT;
    struct view *v = push(k, label);
    snprintf(v->cat, sizeof v->cat, "%s", jstr(c, "id") ? jstr(c, "id") : "");
    switch (k) {
    case V_AT:
        v->slot = (int)jnum_or(app.status, "modem.sim_card", 0);
        load(v, "/at/presets");
        break;
    case V_BT:
        load(v, "/bluetooth");
        lv_timer_resume(bt_timer);
        break;
    case V_DEVICES:
        load(v, "/devices");
        break;
    case V_APN:
        v->slot = -1;
        apn_load(v);
        break;
    default:
        load_cat(v);
    }
    render();
}

static const cJSON *category(const char *id)
{
    const cJSON *c;
    cJSON_ArrayForEach(c, categories)
        if (jstr(c, "id") && !strcmp(jstr(c, "id"), id))
            return c;
    return NULL;
}

static const cJSON *item(struct view *v, const char *id)
{
    const cJSON *it;
    cJSON_ArrayForEach(it, v->data)
        if (jstr(it, "id") && !strcmp(jstr(it, "id"), id))
            return it;
    return NULL;
}

static void open_editor(struct view *v, const cJSON *it)
{
    char label[128];
    label_text(jget(it, "label"), label, sizeof label);
    struct view *e = push(V_EDIT, label);
    snprintf(e->cat, sizeof e->cat, "%s", v->cat);
    e->item = cJSON_Duplicate(it, true);
    const cJSON *value = jget(it, "value");
    if (!strcmp(jstr(it, "type"), "multi"))
        e->draft = cJSON_IsArray(value) ? cJSON_Duplicate(value, true) : cJSON_CreateArray();
    else
        e->draft = value ? cJSON_Duplicate(value, true) : cJSON_CreateNull();
    if (!strcmp(jstr(it, "type"), "number") && !cJSON_IsNumber(e->draft)) {
        cJSON_Delete(e->draft);
        e->draft = cJSON_CreateNumber(jnum_or(it, "min", 0));
    }
}

static void band_reloaded(int status, cJSON *r, void *ctx)
{
    struct view *v = untag(ctx);
    data_loaded(status, r, ctx);
    if (!v || !api_read_ok(status, r) || top() != v)
        return;
    v->saving = false;
    const cJSON *it = item(v, v->busy);
    v->busy[0] = 0;
    if (it && !strcmp(jstr(it, "type"), "multi")) {
        open_editor(v, it);
        render();
    }
}

static void item_clicked(struct view *v, const char *id, const char *key)
{
    const cJSON *it = item(v, id);
    if (!it)
        return;
    const char *type = jstr(it, "type") ? jstr(it, "type") : "";
    if (!strcmp(v->cat, "network") && (!strcmp(id, "lte_bands") || !strcmp(id, "nr_bands"))) {
        snprintf(v->busy, sizeof v->busy, "%s", id);
        v->saving = true;
        shell_page_loading(PAGE_SETTINGS, true);
        api_get("/settings/network", band_reloaded, tag(v));
        return;
    }
    if (!strcmp(type, "category")) {
        const cJSON *c = category(jstr(it, "category") ? jstr(it, "category") : "");
        if (c)
            open_category(c);
    } else if (!strcmp(type, "toggle")) {
        if (jtrue(it, "confirm") && !confirmed(key))
            return;
        cJSON *value = cJSON_CreateBool(!jtrue(it, "value"));
        save(v, id, value);
        cJSON_Delete(value);
    } else if (!strcmp(type, "action")) {
        if (jtrue(it, "confirm") && !confirmed(key))
            return;
        save(v, id, NULL);
    } else if (!strcmp(type, "image")) {
        char caption[256];
        label_text(jget(it, "caption"), caption, sizeof caption);
        if (jstr(it, "src"))
            picture_open(jstr(it, "src"), caption, NULL, NULL);
    } else if (!strcmp(type, "choice") || !strcmp(type, "number") || !strcmp(type, "multi")) {
        open_editor(v, it);
        render();
    }
}

static void edit_clicked(struct view *v, const char *k, const char *arg, const char *key)
{
    struct view *parent = &stack[depth - 2];
    const char *id = jstr(v->item, "id");
    bool confirm = jtrue(v->item, "confirm");
    if (!strcmp(k, "opt")) {
        const cJSON *o = cJSON_GetArrayItem(jget(v->item, "options"), atoi(arg));
        if (!o)
            return;
        if (same_value(jget(o, "value"), jget(v->item, "value"))) {
            pop();
            render();
            return;
        }
        if (confirm && !confirmed(key))
            return;
        save(parent, id, jget(o, "value"));
    } else if (!strcmp(k, "num")) {
        if (!strcmp(arg, "save")) {
            if (confirm && !confirmed(key))
                return;
            save(parent, id, v->draft);
            return;
        }
        double step = jnum_or(v->item, "step", 1), value = v->draft->valuedouble + (*arg == '+' ? step : -step);
        value = value < jnum_or(v->item, "min", value) ? jnum_or(v->item, "min", value) : value;
        value = value > jnum_or(v->item, "max", value) ? jnum_or(v->item, "max", value) : value;
        cJSON_SetNumberValue(v->draft, value);
        render();
    } else if (!strcmp(k, "chk")) {
        const cJSON *o = cJSON_GetArrayItem(jget(v->item, "options"), atoi(arg));
        const cJSON *value = jget(o, "value");
        int i = 0;
        const cJSON *x;
        cJSON_ArrayForEach(x, v->draft) {
            if (cJSON_Compare(x, value, true)) {
                cJSON_DeleteItemFromArray(v->draft, i);
                render();
                return;
            }
            i++;
        }
        cJSON_AddItemToArray(v->draft, cJSON_Duplicate(value, true));
        render();
    } else if (!strcmp(k, "multi")) {
        if (!strcmp(arg, "clear")) {
            cJSON_Delete(v->draft);
            v->draft = cJSON_CreateArray();
            render();
            return;
        }
        if (confirm && !confirmed(key))
            return;
        save(parent, id, v->draft);
    }
}

static void at_replied(int status, cJSON *r, void *ctx)
{
    shell_page_loading(PAGE_SETTINGS, false);
    struct view *v = untag(ctx);
    if (!v)
        return;
    free(v->reply);
    if (api_reply_ok(r)) {
        const char *reply = jstr(r, "reply");
        v->reply = strdup(reply && *reply ? reply : "OK");
    } else {
        char error[256] = "";
        error_text(jstr(r, "error") ? jstr(r, "error") : "", error, sizeof error);
        size_t n = strlen(error) + 32;
        v->reply = malloc(n);
        snprintf(v->reply, n, "%s: %s", L("失败", "Failed"), error);
    }
    v->warn = jtrue(r, "warning");
    if (v == top())
        render();
}

static void at_run(struct view *v)
{
    free(v->reply);
    v->reply = NULL;
    v->warn = false;
    shell_page_loading(PAGE_SETTINGS, true);
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "cmd", v->text);
    char slot[4];
    snprintf(slot, sizeof slot, "%d", v->slot);
    cJSON_AddStringToObject(b, "slot", slot);
    api_post("/at", b, at_replied, tag(v));
    render();
}

static void device_acted(int status, cJSON *r, void *ctx)
{
    struct view *v = untag(ctx);
    const char *action = ctx && v ? v->text : "";
    if (!api_reply_ok(r))
        toast_failure(r);
    else
        toast("%s", !strcmp(action, "kick") ? L("已踢下线", "Kicked") : L("已保存", "Saved"));
    if (!v || !cJSON_IsArray(jget(r, "devices")))
        return;
    struct view *list = v - 1;
    cJSON_Delete(list->data);
    list->data = cJSON_Duplicate(jget(r, "devices"), true);
    const cJSON *d;
    cJSON_ArrayForEach(d, list->data)
        if (jstr(d, "mac") && jstr(v->item, "mac") && !strcmp(jstr(d, "mac"), jstr(v->item, "mac"))) {
            cJSON_Delete(v->item);
            v->item = cJSON_Duplicate(d, true);
        }
    if (v == top())
        render();
}

static void bt_posted(int status, cJSON *r, void *ctx)
{
    struct view *v = untag(ctx);
    if (!v)
        return;
    if (cJSON_IsBool(jget(r, "adapter"))) {
        cJSON_Delete(v->data);
        v->data = cJSON_Duplicate(r, true);
    }
    struct view *t = top();
    if (t && t->kind == V_BTDEV) {
        const char *action = t->busy;
        if (*action && !api_reply_ok(r))
            toast_failure(r);
        else if (*action)
            toast("%s", !strcmp(action, "connect") ? L("连接中…", "Connecting…") : L("已保存", "Saved"));
        if (api_reply_ok(r) && !strcmp(action, "remove"))
            pop();
        if (top()->kind == V_BTDEV)
            top()->busy[0] = 0;
    }
    render();
}

static void bt_post(struct view *bt, cJSON *b)
{
    api_post("/bluetooth", b, bt_posted, tag(bt));
}

static void bt_tick(lv_timer_t *t)
{
    for (int i = 0; i < depth; i++)
        if (stack[i].kind == V_BT) {
            if (!stack[i].loading && !app.blank)
                load(&stack[i], "/bluetooth");
            return;
        }
    lv_timer_pause(bt_timer);
}

static void clicked(const char *key)
{
    struct view *v = top();
    if (!v || v->saving || (depth >= 2 && stack[depth - 2].saving))
        return;
    if (v->kind == V_APN || v->kind == V_APNEDIT) {
        apn_clicked(v, key);
        return;
    }
    char k[16];
    const char *colon = strchr(key, ':');
    snprintf(k, sizeof k, "%.*s", colon ? (int)(colon - key) : 0, key);
    const char *arg = colon ? colon + 1 : "";
    if (!strcmp(k, "cat")) {
        const cJSON *c = cJSON_GetArrayItem(categories, atoi(arg));
        if (c)
            open_category(c);
    } else if (!strcmp(k, "item")) {
        item_clicked(v, arg, key);
    } else if (v->kind == V_EDIT) {
        edit_clicked(v, k, arg, key);
    } else if (!strcmp(k, "atslot")) {
        v->slot = 1 - v->slot;
        render();
    } else if (!strcmp(k, "atp")) {
        const cJSON *p = cJSON_GetArrayItem(v->data, atoi(arg));
        char label[128];
        label_text(jget(p, "label"), label, sizeof label);
        int slot = v->slot;
        struct view *r = push(V_ATRES, L("执行结果", "Result"));
        snprintf(r->text, sizeof r->text, "%s", jstr(p, "cmd") ? jstr(p, "cmd") : "");
        r->slot = slot;
        at_run(r);
    } else if (!strcmp(k, "atagain")) {
        at_run(v);
    } else if (!strcmp(k, "dev")) {
        const cJSON *d = cJSON_GetArrayItem(v->data, atoi(arg));
        const char *name = jstr(d, "name") ? jstr(d, "name") : jstr(d, "ip") ? jstr(d, "ip") : jstr(d, "mac");
        struct view *n = push(V_DEVICE, name ? name : "");
        n->item = cJSON_Duplicate(d, true);
        render();
    } else if (!strcmp(k, "act")) {
        if (!confirmed(key))
            return;
        snprintf(v->text, sizeof v->text, "%s", arg);
        cJSON *b = cJSON_CreateObject();
        cJSON_AddStringToObject(b, "mac", jstr(v->item, "mac") ? jstr(v->item, "mac") : "");
        cJSON_AddStringToObject(b, "action", arg);
        api_post("/devices", b, device_acted, tag(v));
        render();
    } else if (!strcmp(k, "bt")) {
        if (!v->data)
            return;
        cJSON *b = cJSON_CreateObject();
        cJSON_AddStringToObject(b, "action", !strcmp(arg, "power") ? "power" : !strcmp(arg, "autostart") ? "autostart" : "scan");
        if (!strcmp(arg, "power"))
            cJSON_AddBoolToObject(b, "on", !jtrue(v->data, "powered"));
        else if (!strcmp(arg, "autostart"))
            cJSON_AddBoolToObject(b, "on", !jtrue(v->data, "autostart"));
        else
            toast("%s", L("正在搜索…", "Searching…"));
        bt_post(v, b);
    } else if (!strcmp(k, "btd")) {
        const cJSON *d, *found = NULL;
        cJSON_ArrayForEach(d, jget(v->data, "devices"))
            if (jstr(d, "mac") && !strcmp(jstr(d, "mac"), arg))
                found = d;
        struct view *n = push(V_BTDEV, jstr(found, "name") ? jstr(found, "name") : arg);
        snprintf(n->text, sizeof n->text, "%s", arg);
        render();
    } else if (!strcmp(k, "bta")) {
        if ((!strcmp(arg, "remove") || !strcmp(arg, "disconnect")) && !confirmed(key))
            return;
        snprintf(v->busy, sizeof v->busy, "%s", arg);
        cJSON *b = cJSON_CreateObject();
        cJSON_AddStringToObject(b, "action", arg);
        cJSON_AddStringToObject(b, "mac", v->text);
        bt_post(&stack[depth - 2], b);
    }
}

void settings_open_category(const char *id, const char *title)
{
    settings_reset();
    struct view *v = push(V_CAT, title);
    snprintf(v->cat, sizeof v->cat, "%s", id);
    load_cat(v);
    shell_show(PAGE_SETTINGS);
}

static void on_row(lv_event_t *e)
{
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    if (index >= 0 && index < row_count) {
        char key[96];
        snprintf(key, sizeof key, "%s", row_keys[index]);
        clicked(key);
    }
}

/* ---------- page ---------- */

void settings_reset(void)
{
    while (depth)
        release(&stack[--depth]);
    disarm();
    push(V_MENU, L("设置", "Settings"));
}

static void create(lv_obj_t *page)
{
    path = ui_text(page, 12, false, theme.dim);
    lv_obj_set_width(path, LV_PCT(100));
    lv_label_set_long_mode(path, LV_LABEL_LONG_MODE_WRAP);
    body = ui_box(page, LV_FLEX_FLOW_COLUMN, 8);
    if (!disarm_timer) {
        disarm_timer = lv_timer_create(disarm_tick, 3000, NULL);
        lv_timer_pause(disarm_timer);
        bt_timer = lv_timer_create(bt_tick, 2000, NULL);
        lv_timer_pause(bt_timer);
    }
    if (!depth)
        settings_reset();
    snprintf(stack[0].title, sizeof stack[0].title, "%s", L("设置", "Settings"));
    render();
}

static void shown(void)
{
    load_categories();
    struct view *v = top();
    if (v && v->kind == V_CAT && !v->saving && !v->loading)
        load_cat(v);
    render();
}

static bool back(void)
{
    if (depth <= 1)
        return false;
    disarm();
    pop();
    render();
    return true;
}

static void reset(void)
{
    settings_reset();
    render();
}

static void status(const cJSON *st)
{
    apn_status(st);
}

const struct page_ops settings_page = {
    .icon = "settings", .zh = "设置", .en = "Settings",
    .create = create, .shown = shown, .status = status, .back = back, .top = reset,
};
