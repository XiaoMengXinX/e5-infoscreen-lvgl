#include "esim.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "app.h"
#include "input.h"
#include "install.h"
#include "keyboard.h"
#include "lpac.h"
#include "theme.h"
#include "ui.h"
#include "util.h"

#define RETRY_MS 3000
#define NAME_CHARS 64
#define NAME_BYTES (NAME_CHARS * 4 + 1)

enum view { V_LOADING, V_ROOT, V_MISSING, V_DOWNLOAD, V_RENAME, V_NOTIF_LOADING, V_NOTIFS, V_INFO, V_INTERFACE, V_WAIT };
enum action { A_SWITCH, A_RENAME, A_DELETE, A_DOWNLOAD, A_NOTIFICATION };

struct slot {
    char chip_error[64];
    cJSON *chip, *profiles, *notifs;
    const char *aid;
    unsigned ses;
    bool scanned;
};

static const char *const se_aids[] = {"A06573746B6D65FFFF4953442D522030", "A06573746B6D65FFFF4953442D522031"};

static struct {
    lv_obj_t *root, *body, *switcher, *tabs[2], *se_switcher, *interfaces[2], *notif_actions, *notif_list;
} ui;

static struct {
    enum view view;
    enum action action;
    struct slot slots[2];
    int current;
    char iccid[24], name[NAME_BYTES];
    char *activation, *confirmation;
    bool enabled, downloading, cancelling, csim;
    cJSON *metadata, *before;
    uint32_t seq;
    int reloads, pending, probe;
    lv_obj_t *step;
    char backend[16];
    unsigned backends;
    bool backends_ready;
    struct ui_meter write;
} state = {.current = 1, .csim = true};

/* Bulk handling walks the notifications one at a time in sequence order: one
 * failure must not stop the rest, which is what lpac's own -a does. */
static struct {
    uint32_t *seqs;
    int total, next, ok, failed;
    bool remove, running, automatic;
} bulk;

static bool open_now, on_switcher, pick;
static bool restore_se_focus;
static lv_timer_t *reload_timer;

static void render(void);
static void load(void);
static void on_tab(lv_event_t *e);
static void on_profile(lv_event_t *e);
static void on_add(lv_event_t *e);
static void on_notifs(lv_event_t *e);
static void on_retry(lv_event_t *e);
static void on_install(lv_event_t *e);
static void on_start_download(lv_event_t *e);
static void on_notification(lv_event_t *e);
static void on_confirm_switch(lv_event_t *e);
static void on_switch(lv_event_t *e);
static void on_switched(bool ok, const cJSON *data, const char *error, const char *detail);
static void on_refreshed(bool ok, const cJSON *data, const char *error, const char *detail);
static void reload_switch(lv_timer_t *t);
static void on_switch_list(bool ok, const cJSON *data, const char *error, const char *detail);
static void on_rename(lv_event_t *e);
static void on_save(lv_event_t *e);
static void on_confirm_delete(lv_event_t *e);
static void on_delete(lv_event_t *e);
static void on_changed(bool ok, const cJSON *data, const char *error, const char *detail);
static void on_list(bool ok, const cJSON *data, const char *error, const char *detail);
static void on_loaded(bool ok, const cJSON *data, const char *error, const char *detail);
static void on_notif_count(bool ok, const cJSON *data, const char *error, const char *detail);
static void on_process(lv_event_t *e);
static void on_process_all(lv_event_t *e);
static void on_remove(lv_event_t *e);
static void on_confirm_remove_all(lv_event_t *e);
static void on_remove_all(lv_event_t *e);
static void bulk_start(bool remove);
static void on_bulk(bool ok, const cJSON *data, const char *error, const char *detail);
static void on_notified(bool ok, const cJSON *data, const char *error, const char *detail);
static void on_download_progress(const char *step, const cJSON *data);
static void on_downloaded(bool ok, const cJSON *data, const char *error, const char *detail);
static void arrived(void);
static void on_info(lv_event_t *e);
static void on_before_change(bool ok, const cJSON *data, const char *error, const char *detail);
static void on_auto_list(bool ok, const cJSON *data, const char *error, const char *detail);
static void download_dialog(void);
static void download_command(void);
static void prompt_install(const char *packages);
static const char *install_packages;

static void reset_se(void)
{
    for (int card = 0; card < 2; card++) {
        state.slots[card].scanned = false;
        state.slots[card].ses = 0;
        state.slots[card].aid = NULL;
    }
}

static void choose_interface(bool csim)
{
    if (state.csim == csim) {
        state.view = V_ROOT;
        render();
        return;
    }
    state.csim = csim;
    screen_option_set("esim_interface", csim ? "csim" : "logical");
    lpac_set_interface(csim);
    reset_se();
    load();
}

static void on_choose_interface(lv_event_t *e)
{
    choose_interface((bool)(intptr_t)lv_event_get_user_data(e));
}

static const char *const backend_modes[] = {"", "system", "at:curl", "stdio"};

static void on_backend(lv_event_t *e)
{
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    snprintf(state.backend, sizeof state.backend, "%s", backend_modes[index]);
    screen_option_set("esim_backend", state.backend);
    lpac_set_backend(state.backend);
    reset_se();
    load();
}

static void on_backends(bool ok, const cJSON *data, const char *error, const char *detail)
{
    state.backends_ready = true;
    state.backends = 0;
    if (ok) {
        for (int i = 1; i < 4; i++)
            if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(data, backend_modes[i])))
                state.backends |= 1u << i;
    } else {
        toast("%s", L("无法检测后端", "Cannot detect backends"));
    }
    if (state.view == V_INTERFACE)
        render();
}

static void on_interface(lv_event_t *e)
{
    on_switcher = false;
    state.view = V_INTERFACE;
    state.backends_ready = false;
    render();
    lv_obj_scroll_to_view_recursive(ui.switcher, LV_ANIM_OFF);
    lpac_call(state.current, NULL, (char *const[]){"__backends", NULL}, NULL, on_backends);
}

static void call(char *const argv[], lpac_progress_fn progress, lpac_done_fn done)
{
    lpac_call(state.current, state.slots[state.current].aid, argv, progress, done);
}

static void copy_text(char *out, size_t size, const char *in)
{
    size_t n = strlen(in);
    if (n >= size) {
        n = size - 1;
        while (((unsigned char)in[n] & 0xc0) == 0x80)
            n--;
    }
    memcpy(out, in, n);
    out[n] = 0;
}

static void clean_text(char *text)
{
    char *out = text;
    for (const char *in = text; *in; in++) {
        unsigned char c = (unsigned char)*in;
        if (c < 0x20 || c == 0x7f)
            continue;
        *out++ = *in;
    }
    *out = 0;
    char *start = text;
    while (*start == ' ')
        start++;
    size_t n = strlen(start);
    while (n && start[n - 1] == ' ')
        n--;
    memmove(text, start, n);
    text[n] = 0;
}

static const char *profile_name(const cJSON *profile)
{
    const char *name = jstr_or(profile, "profileNickname", "");
    if (!*name)
        name = jstr_or(profile, "profileName", "");
    return *name ? name : jstr_or(profile, "iccid", "--");
}

static void save_json(cJSON **target, const cJSON *data)
{
    cJSON_Delete(*target);
    *target = cJSON_Duplicate(data, true);
}

static const char *operation_name(const char *operation)
{
    if (!strcmp(operation, "install"))
        return L("安装", "Install");
    if (!strcmp(operation, "enable"))
        return L("启用", "Enable");
    if (!strcmp(operation, "disable"))
        return L("停用", "Disable");
    if (!strcmp(operation, "delete"))
        return L("删除", "Delete");
    return operation;
}

/* lpac names the failing function in error and leaves the human detail in
 * detail; either can be empty. */
static const char *failure_text(const char *error, const char *detail)
{
    if (!strcmp(error, "operation_busy"))
        return L("其他 eSIM 操作正在进行，请稍后重试", "Another eSIM operation is running; try again later");
    if (!strncmp(error, "lpac_", 5) && strstr(error, "driver"))
        return L("lpac 缺少兼容的通信驱动", "lpac has no compatible communication driver");
    if (!strcmp(error, "esim_backend"))
        return L("eSIM 后端运行失败", "The eSIM backend failed");
    if (!strcmp(error, "euicc_init"))
        return L("无法访问 eSIM", "Cannot reach the eSIM");
    if (!strcmp(error, "activation_code") || !strcmp(error, "matching_id"))
        return L("激活码无效", "The activation code is invalid");
    if (!strcmp(error, "confirmation_code") && !strcmp(detail, "required"))
        return L("该激活码需要确认码", "This activation code requires a confirmation code");
    if (!strcmp(error, "es10c_get_profiles_info") || !strcmp(error, "es10b_list_notification"))
        return L("读取 eSIM 失败", "Cannot read the eSIM");
    if ((!strncmp(error, "es9p_", 5) || !strncmp(error, "es8p_", 5)) &&
        (strstr(detail, "HTTP") || strstr(detail, "http")))
        return L("服务器通信失败", "Cannot reach the server");
    if (strstr(detail, "iccid or aid not found"))
        return L("找不到该配置文件", "The profile was not found");
    if (strstr(detail, "profile not in disabled state"))
        return L("请先停用该配置文件", "Disable the profile first");
    if (strstr(detail, "profile not in enabled state"))
        return L("该配置文件未启用", "The profile is not enabled");
    if (strstr(detail, "disallowed by policy"))
        return L("eSIM 策略不允许", "The eSIM policy does not allow it");
    if (strstr(detail, "timeout") || strstr(detail, "Timeout"))
        return L("操作超时", "The operation timed out");
    return *detail ? detail : error;
}

static void toast_failure(const char *error, const char *detail)
{
    toast("%s", failure_text(error, detail));
}

static const char *wait_text(void)
{
    switch (state.action) {
    case A_SWITCH:
        return L("正在切换配置文件…", "Switching profile…");
    case A_RENAME:
        return L("正在重命名…", "Renaming…");
    case A_DELETE:
        return L("正在删除配置文件…", "Deleting the profile…");
    case A_DOWNLOAD:
        return state.cancelling ? L("正在取消下载…", "Cancelling download…")
                                : L("正在上报下载通知…", "Reporting download…");
    case A_NOTIFICATION:
        return L("正在处理通知…", "Handling the notification…");
    }
    return "";
}

static const char *download_step(const char *step)
{
    static const struct {
        const char *step, *zh, *en;
    } steps[] = {
        {"es10b_get_euicc_challenge_and_info", "正在验证 eSIM…", "Verifying the eSIM…"},
        {"es9p_initiate_authentication", "正在连接服务器…", "Connecting to the server…"},
        {"es10b_authenticate_server", "正在验证服务器…", "Verifying the server…"},
        {"es9p_authenticate_client", "正在认证…", "Authenticating…"},
        {"es8p_metadata_parse", "正在解析配置文件…", "Parsing the profile…"},
        {"es10b_prepare_download", "正在准备下载…", "Preparing the download…"},
        {"es9p_get_bound_profile_package", "正在获取配置文件…", "Fetching the profile…"},
        {"es10b_load_bound_profile_package", "正在写入 eSIM…", "Writing to the eSIM…"},
        {"es10b_load_bound_profile_package:progress", "正在写入 eSIM…", "Writing to the eSIM…"},
        {"es10b_cancel_session", "正在取消会话…", "Cancelling the session…"},
        {"es9p_cancel_session", "正在取消会话…", "Cancelling the session…"},
    };
    for (size_t i = 0; i < sizeof steps / sizeof *steps; i++)
        if (!strcmp(step, steps[i].step))
            return L(steps[i].zh, steps[i].en);
    return L("正在处理…", "Working…");
}

static void message(const char *text)
{
    lv_obj_t *card = ui_card(ui.body);
    lv_obj_t *label = ui_text(card, 14, false, theme.text);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(label, text);
}

static void on_retry(lv_event_t *e)
{
    load();
}

static void retry(void)
{
    lv_obj_t *row = ui_box(ui.body, LV_FLEX_FLOW_ROW, 8);
    lv_obj_t *button = ui_button(row, L("重试", "Retry"), on_retry, NULL);
    ui_primary(button);
    lv_obj_set_flex_grow(button, 1);
}

static void missing(void)
{
    message(L("eSIM 管理需要安装运行组件，是否从软件源安装？",
              "Install the eSIM components from the package feed?"));
    lv_obj_t *row = ui_box(ui.body, LV_FLEX_FLOW_ROW, 8);
    lv_obj_t *button = ui_button(row, L("下载安装", "Install"), on_install, NULL);
    ui_primary(button);
    lv_obj_set_flex_grow(button, 1);
}

static void select_se(int index)
{
    const char *aid = se_aids[index];
    if (state.slots[state.current].aid == aid)
        return;
    restore_se_focus = input_key_focus(input_group()) == ui.se_switcher;
    state.slots[state.current].aid = aid;
    load();
}

static void on_se(lv_event_t *e)
{
    select_se((int)(intptr_t)lv_event_get_user_data(e));
}

static void root(void)
{
    struct slot *s = &state.slots[state.current];
    if (s->ses) {
        lv_obj_t *scope = ui_card(ui.body);
        lv_label_set_text(ui_text(scope, 12, false, theme.dim), L("选择 SE", "Select SE"));
        lv_obj_t *row = ui.se_switcher = ui_choices(scope);
        ui_focusable(row);
        lv_obj_set_style_outline_pad(row, S(4), LV_STATE_FOCUS_KEY);
        int selected = 0;
        for (int index = 0; index < 2; index++) {
            if (!(s->ses & (1u << index)))
                continue;
            ui_choice(row, index ? "SE 1" : "SE 0", on_se, (void *)(intptr_t)index);
            if (s->aid == se_aids[index])
                selected = (int)lv_obj_get_child_count(row) - 1;
        }
        ui_choices_paint(row, selected, false);
    }
    if (!s->chip && !s->profiles) {
        message(!strcmp(s->chip_error, "euicc_init")
                    ? L("该卡槽没有 eUICC", "No eUICC in this slot")
                    : L("无法读取 eSIM 信息", "Cannot read the eSIM"));
        retry();
        return;
    }
    lv_obj_t *card = ui_card(ui.body);
    lv_obj_t *head = ui_box(card, LV_FLEX_FLOW_ROW, 8);
    lv_obj_t *title = ui_text(head, 12, false, theme.dim);
    lv_label_set_text(title, L("eSIM 芯片", "eSIM chip"));
    lv_obj_set_flex_grow(title, 1);
    lv_obj_t *details = ui_button(head, L("详情 ›", "Details ›"), on_info, NULL);
    lv_obj_set_style_bg_opa(details, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(details, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_pad_ver(details, S(3), 0);
    lv_obj_set_style_pad_hor(details, S(6), 0);
    lv_obj_set_style_text_font(lv_obj_get_child(details, 0), F(12), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(details, 0), theme.accent, 0);
    lv_label_set_text(ui_kv(card, L("EID", "EID")), jstr_or(s->chip, "eidValue", "--"));
    lv_label_set_text(ui_kv(card, L("固件", "Firmware")), jstr_or(s->chip, "EUICCInfo2.euiccFirmwareVer", "--"));
    if (!s->profiles) {
        message(L("读取 eSIM 失败", "Cannot read the eSIM"));
        retry();
        return;
    }
    ui_heading(ui.body, L("配置文件", "Profiles"));
    lv_obj_t *list = ui_box(ui.body, LV_FLEX_FLOW_COLUMN, ui_stack_gap());
    const cJSON *profile;
    int i = 0;
    cJSON_ArrayForEach(profile, s->profiles) {
        bool enabled = jstr_is(profile, "profileState", "enabled");
        ui_row(list, NULL, profile_name(profile), enabled ? L("已启用", "Enabled") : L("未启用", "Disabled"),
               ROW_CHEVRON | (enabled ? ROW_ON : 0), on_profile, (void *)(intptr_t)i++);
    }
    ui_row(list, "plus", L("添加配置文件", "Add profile"), NULL, ROW_CHEVRON, on_add, NULL);
    char count[16];
    snprintf(count, sizeof count, "%d", cJSON_GetArraySize(s->notifs));
    ui_row(list, "bell", L("通知", "Notifications"), count, ROW_CHEVRON, on_notifs, NULL);
}

static void bind(lv_obj_t *area)
{
    keyboard_bind(area);
    ui_focusable(area);
}

static lv_obj_t *textarea(lv_obj_t *parent, uint32_t max, const char *text)
{
    lv_obj_t *area = lv_textarea_create(parent);
    lv_obj_set_width(area, LV_PCT(100));
    lv_textarea_set_one_line(area, true);
    lv_textarea_set_max_length(area, max);
    lv_textarea_set_text(area, text);
    lv_obj_set_style_text_font(area, F(14), 0);
    lv_obj_set_style_text_color(area, theme.text, 0);
    lv_obj_set_style_bg_color(area, theme.surface, 0);
    lv_obj_set_style_bg_opa(area, theme.surface_opa, 0);
    lv_obj_set_style_border_color(area, theme.line, 0);
    lv_obj_set_style_border_color(area, theme.accent, LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(area, 1, 0);
    lv_obj_set_style_radius(area, S(10), 0);
    lv_obj_set_style_pad_all(area, S(8), 0);
    return area;
}

static void edit_text(char **target, lv_event_t *e)
{
    const char *text = lv_textarea_get_text(lv_event_get_target_obj(e));
    char *value = strdup(text);
    if (!value) {
        toast("%s", L("内存不足", "Out of memory"));
        return;
    }
    free(*target);
    *target = value;
}

static void on_activation(lv_event_t *e)
{
    edit_text(&state.activation, e);
}

static void on_name(lv_event_t *e)
{
    copy_text(state.name, sizeof state.name, lv_textarea_get_text(lv_event_get_target_obj(e)));
}

static void on_confirmation(lv_event_t *e)
{
    edit_text(&state.confirmation, e);
}

static void download(void)
{
    ui_heading(ui.body, L("添加配置文件", "Add profile"));
    lv_obj_t *box = ui_box(ui.body, LV_FLEX_FLOW_COLUMN, 4);
    lv_label_set_text(ui_text(box, 12, false, theme.dim), L("激活码", "Activation code"));
    lv_obj_t *activation = textarea(box, 0, state.activation ? state.activation : "");
    lv_textarea_set_placeholder_text(activation, "LPA:1$…");
    lv_obj_add_event_cb(activation, on_activation, LV_EVENT_VALUE_CHANGED, NULL);
    bind(activation);
    box = ui_box(ui.body, LV_FLEX_FLOW_COLUMN, 4);
    lv_label_set_text(ui_text(box, 12, false, theme.dim),
                      L("确认码（可选）", "Confirmation code (optional)"));
    lv_obj_t *confirmation = textarea(box, 0, state.confirmation ? state.confirmation : "");
    lv_obj_add_event_cb(confirmation, on_confirmation, LV_EVENT_VALUE_CHANGED, NULL);
    bind(confirmation);
    lv_obj_t *row = ui_box(ui.body, LV_FLEX_FLOW_ROW, 8);
    lv_obj_t *button = ui_button(row, L("下载", "Download"), on_start_download, NULL);
    ui_primary(button);
    lv_obj_set_flex_grow(button, 1);
}

static void renaming(void)
{
    ui_heading(ui.body, L("重命名", "Rename"));
    lv_obj_t *box = ui_box(ui.body, LV_FLEX_FLOW_COLUMN, 4);
    lv_label_set_text(ui_text(box, 12, false, theme.dim), L("名称", "Name"));
    lv_obj_t *area = textarea(box, NAME_CHARS, state.name);
    lv_obj_add_event_cb(area, on_name, LV_EVENT_VALUE_CHANGED, NULL);
    bind(area);
    lv_obj_t *row = ui_box(ui.body, LV_FLEX_FLOW_ROW, 8);
    lv_obj_t *button = ui_button(row, L("保存", "Save"), on_save, NULL);
    ui_primary(button);
    lv_obj_set_flex_grow(button, 1);
}

static void notifs(void)
{
    const cJSON *notifications = state.slots[state.current].notifs;
    ui_heading(ui.body, L("通知", "Notifications"));
    if (!cJSON_GetArraySize(notifications)) {
        message(L("没有待处理的通知", "No pending notifications"));
        return;
    }
    lv_obj_t *actions = ui.notif_actions = ui_box(ui.body, LV_FLEX_FLOW_ROW, 8);
    lv_obj_t *process = ui_button(actions, L("处理全部", "Process all"), on_process_all, NULL);
    ui_primary(process);
    lv_obj_set_flex_grow(process, 1);
    lv_obj_t *remove = ui_button(actions, L("全部删除", "Delete all"), on_confirm_remove_all, NULL);
    ui_danger(remove);
    lv_obj_set_flex_grow(remove, 1);
    lv_obj_t *list = ui.notif_list = ui_box(ui.body, LV_FLEX_FLOW_COLUMN, ui_stack_gap());
    const cJSON *notification;
    int i = 0;
    cJSON_ArrayForEach(notification, notifications) {
        char label[64];
        snprintf(label, sizeof label, "%s #%u", operation_name(jstr_or(notification, "profileManagementOperation", "")),
                 (uint32_t)jnum_or(notification, "seqNumber", 0));
        ui_row(list, NULL, label, jstr_or(notification, "iccid", ""), ROW_CHEVRON, on_notification,
               (void *)(intptr_t)i++);
    }
}

static const char *info_value(const char *value)
{
    while (*value && isspace((unsigned char)*value))
        value++;
    return *value ? value : L("未提供", "Not provided");
}

static void info_text(lv_obj_t *parent, const char *label, const char *value)
{
    lv_obj_t *row = ui_box(parent, LV_FLEX_FLOW_COLUMN, 4);
    lv_label_set_text(ui_text(row, 12, false, theme.dim), label);
    ui_note(row, info_value(value));
}

static void info_field(lv_obj_t *parent, const cJSON *chip, const char *label, const char *path)
{
    const cJSON *value = jget(chip, path);
    if (cJSON_IsString(value)) {
        info_text(parent, label, value->valuestring);
    } else if (cJSON_IsNumber(value)) {
        char text[32];
        fmt_bytes(value->valuedouble, text, sizeof text);
        info_text(parent, label, text);
    } else if (cJSON_IsArray(value)) {
        lv_label_set_text(ui_text(parent, 12, false, theme.dim), label);
        const cJSON *item;
        cJSON_ArrayForEach(item, value)
            if (cJSON_IsString(item))
                ui_note(parent, info_value(item->valuestring));
        if (!cJSON_GetArraySize(value))
            ui_note(parent, L("未提供", "Not provided"));
    } else {
        info_text(parent, label, "");
    }
}

static void info(void)
{
    const cJSON *chip = state.slots[state.current].chip;
    lv_obj_t *card = ui_card(ui.body);
    info_field(card, chip, "EID", "eidValue");
    info_field(card, chip, L("固件版本", "Firmware"), "EUICCInfo2.euiccFirmwareVer");
    info_field(card, chip, L("SGP.22 版本", "SGP.22 version"), "EUICCInfo2.svn");
    info_field(card, chip, L("配置格式版本", "Profile format version"), "EUICCInfo2.profileVersion");
    info_field(card, chip, L("GlobalPlatform 版本", "GlobalPlatform version"), "EUICCInfo2.globalplatformVersion");
    card = ui_card(ui.body);
    info_field(card, chip, L("剩余存储空间", "Free storage"), "EUICCInfo2.extCardResource.freeNonVolatileMemory");
    info_field(card, chip, L("剩余运行内存", "Free RAM"), "EUICCInfo2.extCardResource.freeVolatileMemory");
    card = ui_card(ui.body);
    info_field(card, chip, L("签名证书公钥标识", "Signing certificate key IDs"), "EUICCInfo2.euiccCiPKIdListForSigning");
    info_field(card, chip, L("验证证书公钥标识", "Verification certificate key IDs"), "EUICCInfo2.euiccCiPKIdListForVerification");
    info_field(card, chip, L("SAS 认证编号", "SAS accreditation"), "EUICCInfo2.sasAcreditationNumber");
}

static void on_info(lv_event_t *e)
{
    state.view = V_INFO;
    render();
}

static void paint_switcher(void)
{
    /* Actions, forms and reloads finish in the slot they started in, and their
     * callbacks keep writing to state.current. */
    bool switchable = state.view == V_ROOT || state.view == V_NOTIFS;
    for (int c = 0; c < 2; c++) {
        lv_obj_set_style_bg_opa(ui.tabs[c], c == state.current ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(ui.tabs[c], 0),
                                    c == state.current ? theme.accent : theme.dim, 0);
        lv_obj_set_state(ui.tabs[c], LV_STATE_DISABLED, !switchable);
        lv_obj_set_state(ui.tabs[c], LV_STATE_FOCUS_KEY, on_switcher && switchable && c == state.current);
    }
}

static void render(void)
{
    if (!ui.root || !open_now)
        return;
    paint_switcher();
    keyboard_close();
    lv_obj_clean(ui.body);
    ui.se_switcher = NULL;
    ui.interfaces[0] = ui.interfaces[1] = NULL;
    ui.notif_actions = ui.notif_list = NULL;
    switch (state.view) {
    case V_LOADING:
        message(L("正在读取 eSIM 信息…", "Reading the eSIM…"));
        break;
    case V_NOTIF_LOADING:
        message(L("正在读取通知…", "Reading notifications…"));
        break;
    case V_ROOT:
        root();
        ui_row(ui.body, "settings", L("高级设置", "Advanced settings"), NULL, ROW_CHEVRON, on_interface, NULL);
        if (restore_se_focus && ui.se_switcher) {
            lv_group_focus_obj(ui.se_switcher);
            lv_obj_add_state(ui.se_switcher, LV_STATE_FOCUS_KEY);
            restore_se_focus = false;
        }
        break;
    case V_MISSING:
        missing();
        break;
    case V_DOWNLOAD:
        download();
        break;
    case V_RENAME:
        renaming();
        break;
    case V_NOTIFS:
        notifs();
        break;
    case V_INFO:
        info();
        break;
    case V_INTERFACE:
        ui_heading(ui.body, L("高级设置", "Advanced settings"));
        ui_note(ui.body, L("后端选择", "Backend"));
        const char *names[] = {L("自动选择", "Automatic"), L("系统 eUICC", "System eUICC"),
                               "lpac AT + curl", "lpac stdio"};
        for (int i = 0; i < 4; i++) {
            bool selected = !strcmp(state.backend, backend_modes[i]);
            bool available = i == 0 || (state.backends_ready && (state.backends & (1u << i)));
            const char *status = selected ? L("已选择", "Selected") : available ? NULL :
                state.backends_ready ? L("不可用", "Unavailable") : L("检测中…", "Checking…");
            lv_obj_t *row = ui_row(ui.body, NULL, names[i], status, selected ? ROW_ON : 0,
                                  on_backend, (void *)(intptr_t)i);
            lv_obj_set_state(row, LV_STATE_DISABLED, !available);
        }
        ui_note(ui.body, L("接口", "APDU interface"));
        ui.interfaces[0] = ui_row(ui.body, NULL, "AT+CSIM", state.csim ? L("已选择", "Selected") : NULL,
               state.csim ? ROW_ON : 0, on_choose_interface, (void *)1);
        ui.interfaces[1] = ui_row(ui.body, NULL, "AT+CCHO / CGLA", !state.csim ? L("已选择", "Selected") : NULL,
               !state.csim ? ROW_ON : 0, on_choose_interface, NULL);
        break;
    case V_WAIT: {
        char text[64];
        const char *label = wait_text();
        if (bulk.running) {
            snprintf(text, sizeof text,
                     bulk.remove ? L("正在删除通知 %d/%d…", "Deleting notification %d/%d…")
                                 : L("正在处理通知 %d/%d…", "Handling notification %d/%d…"),
                     bulk.next, bulk.total);
            label = text;
        }
        message(label);
        break;
    }
    }
}

static void on_confirm_switch(lv_event_t *e)
{
    const char *action = state.enabled ? L("停用", "Disable") : L("启用", "Enable");
    dialog_open(action, state.enabled
                            ? L("停用后该配置文件不可用，基带将重新读取 SIM 卡",
                                "The profile becomes unusable and the modem re-reads the SIM")
                            : L("将停用当前启用的配置文件，并让基带重新读取 SIM 卡",
                                "The current profile turns off and the modem re-reads the SIM"));
    lv_obj_t *button = dialog_button(action, on_switch, NULL);
    ui_primary(button);
    dialog_cancel();
}

static void on_switch(lv_event_t *e)
{
    dialog_close();
    state.action = A_SWITCH;
    state.view = V_WAIT;
    render();
    call((char *const[]){"profile", state.enabled ? "disable" : "enable", state.iccid, NULL},
         NULL, on_switched);
}

static void on_switched(bool ok, const cJSON *data, const char *error, const char *detail)
{
    if (!ok) {
        toast_failure(error, detail);
        state.view = V_ROOT;
        render();
        return;
    }
    lpac_sim_refresh(state.current, on_refreshed);
}

static void reload_switch(lv_timer_t *t)
{
    reload_timer = NULL;
    call((char *const[]){"profile", "list", NULL}, NULL, on_switch_list);
}

static void cancel_reload(void)
{
    if (reload_timer) {
        lv_timer_delete(reload_timer);
        reload_timer = NULL;
    }
}

static void on_refreshed(bool ok, const cJSON *data, const char *error, const char *detail)
{
    if (!ok)
        toast("%s", L("SIM 刷新失败", "SIM refresh failed"));
    /* The SIM answers nothing useful while it initializes. */
    state.reloads = 5;
    reload_timer = lv_timer_create(reload_switch, RETRY_MS, NULL);
    lv_timer_set_repeat_count(reload_timer, 1);
}

static void on_switch_list(bool ok, const cJSON *data, const char *error, const char *detail)
{
    bool changed = false;
    const cJSON *profile;
    if (ok)
        cJSON_ArrayForEach(profile, data)
            if (jstr_is(profile, "iccid", state.iccid)) {
                changed = jstr_is(profile, "profileState", state.enabled ? "disabled" : "enabled");
                break;
            }
    if (!changed && state.reloads) {
        state.reloads--;
        reload_timer = lv_timer_create(reload_switch, RETRY_MS, NULL);
        lv_timer_set_repeat_count(reload_timer, 1);
        return;
    }
    if (!ok)
        toast_failure(error, detail);
    else {
        save_json(&state.slots[state.current].profiles, data);
        if (!changed)
            toast("%s", L("配置状态尚未更新，请稍后重试", "Profile state has not updated; try again later"));
    }
    state.view = V_ROOT;
    render();
}

static void on_rename(lv_event_t *e)
{
    dialog_close();
    state.view = V_RENAME;
    render();
}

static void on_save(lv_event_t *e)
{
    clean_text(state.name);
    state.action = A_RENAME;
    state.view = V_WAIT;
    render();
    call((char *const[]){"profile", "nickname", state.iccid, state.name, NULL}, NULL,
         on_changed);
}

static void on_confirm_delete(lv_event_t *e)
{
    dialog_open(L("删除配置文件", "Delete the profile"), L("删除后无法恢复", "This cannot be undone"));
    lv_obj_t *button = dialog_button(L("删除", "Delete"), on_delete, NULL);
    ui_danger(button);
    dialog_cancel();
}

static void on_delete(lv_event_t *e)
{
    dialog_close();
    state.action = A_DELETE;
    state.view = V_WAIT;
    render();
    call((char *const[]){"notification", "list", NULL}, NULL, on_before_change);
}

static void on_changed(bool ok, const cJSON *data, const char *error, const char *detail)
{
    if (!ok) {
        cJSON_Delete(state.before);
        state.before = NULL;
        toast_failure(error, detail);
        state.view = V_ROOT;
        render();
        return;
    }
    if (state.action == A_DELETE)
        call((char *const[]){"notification", "list", NULL}, NULL, on_auto_list);
    else
        call((char *const[]){"profile", "list", NULL}, NULL, on_list);
}

static void on_list(bool ok, const cJSON *data, const char *error, const char *detail)
{
    if (ok)
        save_json(&state.slots[state.current].profiles, data);
    else if (open_now)
        toast_failure(error, detail);
    state.view = V_ROOT;
    render();
}

static void on_profile(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    const cJSON *profile = cJSON_GetArrayItem(state.slots[state.current].profiles, i);
    copy_text(state.iccid, sizeof state.iccid, jstr_or(profile, "iccid", ""));
    copy_text(state.name, sizeof state.name, profile_name(profile));
    state.enabled = jstr_is(profile, "profileState", "enabled");
    dialog_open(profile_name(profile), jstr_or(profile, "serviceProviderName", ""));
    ui_note(dialog_sheet(), state.iccid);
    lv_obj_t *toggle = dialog_button(state.enabled ? L("停用", "Disable") : L("启用", "Enable"), on_confirm_switch, NULL);
    ui_primary(toggle);
    dialog_button(L("重命名", "Rename"), on_rename, NULL);
    if (!state.enabled)
        ui_danger(dialog_button(L("删除", "Delete"), on_confirm_delete, NULL));
    dialog_cancel();
}

static void on_add(lv_event_t *e)
{
    state.view = V_DOWNLOAD;
    render();
}

static void on_download_closed(void)
{
    state.step = NULL;
    state.write = (struct ui_meter){0};
    if (!state.downloading)
        return;
    state.cancelling = true;
    state.view = V_WAIT;
    lpac_cancel();
    render();
}

static void on_cancel_download(lv_event_t *e)
{
    dialog_close();
}

static void download_dialog(void)
{
    dialog_on_close(NULL);
    dialog_open(L("正在下载配置文件", "Downloading the profile"), NULL);
    state.step = ui_text(dialog_sheet(), 13, false, theme.text);
    lv_obj_set_width(state.step, LV_PCT(100));
    lv_label_set_long_mode(state.step, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(state.step, L("正在处理…", "Working…"));
    dialog_button(L("取消", "Cancel"), on_cancel_download, NULL);
    dialog_on_close(on_download_closed);
}

static void on_accept_download(lv_event_t *e)
{
    if (lpac_download_confirm(true))
        download_dialog();
    else
        toast("%s", L("无法继续下载", "Cannot continue the download"));
}

static void on_download_progress(const char *step, const cJSON *data)
{
    if (!strcmp(step, "es8p_metadata_parse") || !strcmp(step, "es8p_meatadata_parse"))
        save_json(&state.metadata, data);
    if (state.cancelling)
        return;
    if (!strcmp(step, "preview")) {
        dialog_on_close(NULL);
        dialog_open(L("确认下载配置文件", "Confirm profile download"), NULL);
        state.step = NULL;
        state.write = (struct ui_meter){0};
        lv_obj_t *sheet = dialog_sheet();
        info_text(sheet, L("运营商", "Provider"), jstr_or(state.metadata, "serviceProviderName", ""));
        info_text(sheet, L("配置名称", "Profile name"), jstr_or(state.metadata, "profileName", ""));
        info_text(sheet, "ICCID", jstr_or(state.metadata, "iccid", ""));
        info_text(sheet, L("配置类别", "Profile class"), jstr_or(state.metadata, "profileClass", ""));
        ui_primary(dialog_button(L("确认下载", "Download"), on_accept_download, NULL));
        dialog_button(L("取消", "Cancel"), on_cancel_download, NULL);
        dialog_on_close(on_download_closed);
    } else if (state.step) {
        ui_set_text(state.step, download_step(step));
        const cJSON *written = cJSON_GetObjectItemCaseSensitive(data, "written");
        const cJSON *total = cJSON_GetObjectItemCaseSensitive(data, "total");
        if ((!strcmp(step, "es10b_load_bound_profile_package") ||
             !strcmp(step, "es10b_load_bound_profile_package:progress")) &&
            cJSON_IsNumber(written) && cJSON_IsNumber(total) && total->valuedouble > 0) {
            int percent = (int)(100 * written->valuedouble / total->valuedouble);
            percent = LV_CLAMP(0, percent, 100);
            char done[32], size[32], text[96];
            fmt_bytes(written->valuedouble, done, sizeof done);
            fmt_bytes(total->valuedouble, size, sizeof size);
            snprintf(text, sizeof text, "%d%% · %s / %s", percent, done, size);
            if (!state.write.bar) {
                state.write = ui_meter(dialog_sheet(), L("写入进度", "Write progress"));
                lv_obj_move_to_index(lv_obj_get_parent(state.write.value), -3);
                lv_obj_move_to_index(state.write.bar, -2);
            }
            ui_meter_set(&state.write, text, percent, theme.accent);
        }
    }
}

static void on_downloaded(bool ok, const cJSON *data, const char *error, const char *detail)
{
    state.downloading = false;
    if (dialog_is_open() && !state.cancelling)
        dialog_close();
    state.step = NULL;
    state.write = (struct ui_meter){0};
    cJSON_Delete(state.metadata);
    state.metadata = NULL;
    if (!ok) {
        cJSON_Delete(state.before);
        state.before = NULL;
        if (state.cancelling)
            toast("%s", L("已取消下载", "Download cancelled"));
        else
            toast_failure(error, detail);
        state.cancelling = false;
        state.view = V_DOWNLOAD;
        render();
        return;
    }
    state.cancelling = false;
    free(state.activation);
    state.activation = NULL;
    free(state.confirmation);
    state.confirmation = NULL;
    toast("%s", L("配置文件已下载", "Profile downloaded"));
    state.view = V_WAIT;
    render();
    call((char *const[]){"notification", "list", NULL}, NULL, on_auto_list);
}

static void download_command(void)
{
    char *argv[8] = {"profile", "download", "-p", "-a", state.activation};
    int n = 5;
    if (state.confirmation && *state.confirmation) {
        argv[n++] = "-c";
        argv[n++] = state.confirmation;
    }
    argv[n] = NULL;
    call(argv, on_download_progress, on_downloaded);
}

static void on_before_change(bool ok, const cJSON *data, const char *error, const char *detail)
{
    if (state.cancelling) {
        on_downloaded(false, NULL, "cancelled", "");
        return;
    }
    if (!ok) {
        if (state.action == A_DOWNLOAD)
            on_downloaded(false, NULL, error, detail);
        else {
            toast_failure(error, detail);
            state.view = V_ROOT;
            render();
        }
        return;
    }
    save_json(&state.before, data);
    if (state.action == A_DOWNLOAD)
        download_command();
    else
        call((char *const[]){"profile", "delete", state.iccid, NULL}, NULL, on_changed);
}

static void on_start_download(lv_event_t *e)
{
    if (!state.activation || !*state.activation) {
        toast("%s", L("请输入激活码", "Enter the activation code"));
        return;
    }
    clean_text(state.activation);
    if (state.confirmation)
        clean_text(state.confirmation);
    state.action = A_DOWNLOAD;
    state.downloading = true;
    state.cancelling = false;
    download_dialog();
    call((char *const[]){"notification", "list", NULL}, NULL, on_before_change);
}

static void on_notif_list(bool ok, const cJSON *data, const char *error, const char *detail)
{
    if (ok)
        save_json(&state.slots[state.current].notifs, data);
    else if (open_now)
        toast_failure(error, detail);
    state.view = V_NOTIFS;
    render();
}

static void on_notifs(lv_event_t *e)
{
    state.view = V_NOTIF_LOADING;
    render();
    call((char *const[]){"notification", "list", NULL}, NULL, on_notif_list);
}

static void on_notified(bool ok, const cJSON *data, const char *error, const char *detail)
{
    if (!ok) {
        toast_failure(error, detail);
        state.view = V_NOTIFS;
        render();
        return;
    }
    call((char *const[]){"notification", "list", NULL}, NULL, on_notif_list);
}

static void on_process(lv_event_t *e)
{
    char seq[16];
    dialog_close();
    state.action = A_NOTIFICATION;
    state.view = V_WAIT;
    render();
    snprintf(seq, sizeof seq, "%u", state.seq);
    call((char *const[]){"notification", "process", "-r", seq, NULL}, NULL, on_notified);
}

static void on_remove(lv_event_t *e)
{
    char seq[16];
    dialog_close();
    state.action = A_NOTIFICATION;
    state.view = V_WAIT;
    render();
    snprintf(seq, sizeof seq, "%u", state.seq);
    call((char *const[]){"notification", "remove", seq, NULL}, NULL, on_notified);
}

static void on_process_all(lv_event_t *e)
{
    bulk_start(false);
}

static void on_confirm_remove_all(lv_event_t *e)
{
    dialog_open(L("全部删除通知", "Delete all notifications"),
                L("删除后无法恢复", "This cannot be undone"));
    lv_obj_t *button = dialog_button(L("删除", "Delete"), on_remove_all, NULL);
    ui_danger(button);
    dialog_cancel();
}

static void on_remove_all(lv_event_t *e)
{
    dialog_close();
    bulk_start(true);
}

static bool bulk_next(void)
{
    if (bulk.next == bulk.total) {
        bulk.running = false;
        free(bulk.seqs);
        bulk.seqs = NULL;
        if (bulk.failed)
            toast("%s", bulk.automatic ? L("部分通知未上报\n请到通知页重试", "Notifications pending\nRetry in Notifications")
                                     : L("部分通知处理失败\n请稍后重试", "Some notifications failed\nTry again later"));
        else if (!bulk.automatic)
            toast(bulk.remove ? L("已删除 %d 条通知", "Deleted %d notifications")
                              : L("已处理 %d 条通知", "Handled %d notifications"), bulk.ok);
        if (bulk.automatic)
            load();
        else
            call((char *const[]){"notification", "list", NULL}, NULL, on_notif_list);
        return false;
    }
    char seq[16];
    snprintf(seq, sizeof seq, "%u", bulk.seqs[bulk.next++]);
    if (bulk.remove)
        call((char *const[]){"notification", "remove", seq, NULL}, NULL, on_bulk);
    else
        call((char *const[]){"notification", "process", "-r", seq, NULL}, NULL, on_bulk);
    return true;
}

static bool notification_seen(const cJSON *notifications, uint32_t seq)
{
    const cJSON *item;
    cJSON_ArrayForEach(item, notifications)
        if ((uint32_t)jnum_or(item, "seqNumber", 0) == seq)
            return true;
    return false;
}

static void bulk_start(bool remove)
{
    const cJSON *notifications = state.slots[state.current].notifs;
    int count = cJSON_GetArraySize(notifications);
    free(bulk.seqs);
    bulk = (typeof(bulk)){.remove = remove, .running = true, .automatic = state.before != NULL};
    bulk.seqs = count ? malloc((size_t)count * sizeof *bulk.seqs) : NULL;
    if (count && !bulk.seqs) {
        bulk.running = false;
        cJSON_Delete(state.before);
        state.before = NULL;
        toast("%s", L("内存不足", "Out of memory"));
        state.view = V_ROOT;
        render();
        return;
    }
    const cJSON *item;
    cJSON_ArrayForEach(item, notifications) {
        uint32_t seq = (uint32_t)jnum_or(item, "seqNumber", 0);
        if (!bulk.automatic || !notification_seen(state.before, seq))
            bulk.seqs[bulk.total++] = seq;
    }
    cJSON_Delete(state.before);
    state.before = NULL;
    for (int i = 1; i < bulk.total; i++) {
        uint32_t seq = bulk.seqs[i];
        int j;
        for (j = i; j && bulk.seqs[j - 1] > seq; j--)
            bulk.seqs[j] = bulk.seqs[j - 1];
        bulk.seqs[j] = seq;
    }
    if (!bulk.automatic)
        state.action = A_NOTIFICATION;
    state.view = V_WAIT;
    if (bulk_next())
        render();
}

static void on_auto_list(bool ok, const cJSON *data, const char *error, const char *detail)
{
    if (!ok) {
        cJSON_Delete(state.before);
        state.before = NULL;
        toast("%s", L("读取通知失败\n请到通知页重试", "Cannot read notifications\nRetry in Notifications"));
        load();
        return;
    }
    save_json(&state.slots[state.current].notifs, data);
    bulk_start(false);
}

static void on_bulk(bool ok, const cJSON *data, const char *error, const char *detail)
{
    if (ok)
        bulk.ok++;
    else
        bulk.failed++;
    if (bulk_next())
        render();
}

static void on_notification(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    const cJSON *notification = cJSON_GetArrayItem(state.slots[state.current].notifs, i);
    state.seq = (uint32_t)jnum_or(notification, "seqNumber", 0);
    dialog_open(L("通知", "Notifications"), jstr_or(notification, "notificationAddress", ""));
    ui_note(dialog_sheet(), jstr_or(notification, "iccid", ""));
    ui_primary(dialog_button(L("处理", "Process"), on_process, NULL));
    ui_danger(dialog_button(L("删除", "Delete"), on_remove, NULL));
    dialog_cancel();
}

static void chip_ready(void)
{
    state.pending += 2;
    call((char *const[]){"profile", "list", NULL}, NULL, on_loaded);
    call((char *const[]){"notification", "list", NULL}, NULL, on_notif_count);
    arrived();
}

static void on_se_info(bool ok, const cJSON *data, const char *error, const char *detail)
{
    struct slot *slot = &state.slots[state.current];
    if (ok && *jstr_or(data, "eidValue", "") && cJSON_IsObject(jget(data, "EUICCInfo2"))) {
        if (!slot->ses) {
            slot->aid = se_aids[state.probe];
            save_json(&slot->chip, data);
        }
        slot->ses |= 1u << state.probe;
    }
    if (++state.probe < 2) {
        lpac_call(state.current, se_aids[state.probe], (char *const[]){"chip", "info", NULL}, NULL, on_se_info);
        return;
    }
    chip_ready();
}

static void on_vendor(bool ok, const cJSON *data, const char *error, const char *detail)
{
    state.slots[state.current].scanned = true;
    if (!ok) {
        chip_ready();
        return;
    }
    state.probe = 0;
    lpac_call(state.current, se_aids[0], (char *const[]){"chip", "info", NULL}, NULL, on_se_info);
}

static void on_chip(bool ok, const cJSON *data, const char *error, const char *detail)
{
    struct slot *s = &state.slots[state.current];
    if (!ok) {
        if (!strcmp(error, "python_ssl") || !strcmp(error, "curl_gnutls")) {
            state.view = V_MISSING;
            prompt_install(!strcmp(error, "python_ssl") ? "python3-openssl" : "libcurl-gnutls4");
            render();
            return;
        }
        if (pick) {
            pick = false;
            state.current = 1;
            load();
            return;
        }
        copy_text(s->chip_error, sizeof s->chip_error, error);
        arrived();
        return;
    }
    pick = false;
    save_json(&s->chip, data);
    if (!s->scanned)
        lpac_probe_estk(state.current, on_vendor);
    else
        chip_ready();
}

/* The open sequence ends when its last answer arrives, so the root shows the
 * notification count the first time it is drawn. */
static void arrived(void)
{
    if (--state.pending)
        return;
    state.view = V_ROOT;
    render();
}

static void on_loaded(bool ok, const cJSON *data, const char *error, const char *detail)
{
    if (ok)
        save_json(&state.slots[state.current].profiles, data);
    else if (open_now)
        toast_failure(error, detail);
    arrived();
}

static void on_notif_count(bool ok, const cJSON *data, const char *error, const char *detail)
{
    if (ok)
        save_json(&state.slots[state.current].notifs, data);
    arrived();
}

static void on_install(lv_event_t *e)
{
    char script[384];
    snprintf(script, sizeof script,
             "echo @update\n"
             "apk --timeout 15 update --progress-fd 3 3>&1 >/dev/null 2>&1\n"
             "echo @add\n"
             "exec apk --timeout 30 add --progress-fd 3 %s 3>&1 >/dev/null 2>&1\n", install_packages);
    install_run(L("正在安装 eSIM 组件", "Installing eSIM components"), script, load);
}

static void prompt_install(const char *packages)
{
    install_packages = packages;
    dialog_open(L("安装 eSIM 组件", "Install eSIM components"),
                L("eSIM 管理需要安装运行组件，是否从软件源安装？", "Install the eSIM components from the package feed?"));
    dialog_button(L("下载安装", "Install"), on_install, NULL);
    dialog_cancel();
}

static void load(void)
{
    cancel_reload();
    lpac_stop();
    if (!lpac_present()) {
        state.view = V_MISSING;
        prompt_install("lpac libcurl-gnutls4");
        render();
        return;
    }
    struct slot *s = &state.slots[state.current];
    s->chip_error[0] = 0;
    cJSON_Delete(s->chip);
    cJSON_Delete(s->profiles);
    cJSON_Delete(s->notifs);
    s->chip = s->profiles = s->notifs = NULL;
    state.pending = 1;
    state.view = V_LOADING;
    render();
    call((char *const[]){"chip", "info", NULL}, NULL, on_chip);
}

static void on_tab(lv_event_t *e)
{
    pick = false;
    state.current = (int)(intptr_t)lv_event_get_user_data(e);
    load();
}

void esim_create(lv_obj_t *parent)
{
    memset(&ui, 0, sizeof ui);
    ui.root = ui_box(parent, LV_FLEX_FLOW_COLUMN, ui_stack_gap());
    lv_obj_t *tabs = ui.switcher = ui_box(ui.root, LV_FLEX_FLOW_ROW, 0);
    lv_obj_set_style_bg_color(tabs, theme.surface, 0);
    lv_obj_set_style_bg_opa(tabs, theme.surface_opa, 0);
    lv_obj_set_style_radius(tabs, S(18), 0);
    lv_obj_set_style_pad_all(tabs, S(3), 0);
    for (int c = 0; c < 2; c++) {
        ui.tabs[c] = ui_button(tabs, c ? "SIM 2" : "SIM 1", on_tab, (void *)(intptr_t)c);
        /* The keys reach the switcher only through Left and Right on it. */
        lv_group_remove_obj(ui.tabs[c]);
        lv_obj_set_flex_grow(ui.tabs[c], 1);
        lv_obj_set_style_pad_ver(ui.tabs[c], S(5), 0);
        lv_obj_set_style_radius(ui.tabs[c], S(15), 0);
        lv_obj_set_style_bg_color(ui.tabs[c], theme.active, 0);
        lv_obj_set_style_text_font(lv_obj_get_child(ui.tabs[c], 0), F(12), 0);
    }
    ui.body = ui_box(ui.root, LV_FLEX_FLOW_COLUMN, ui_stack_gap());
}

void esim_open(void)
{
    open_now = true;
    if (state.view == V_WAIT || state.downloading) {
        render();
        return;
    }
    on_switcher = pick = true;
    char interface[16];
    screen_option("esim_interface", interface, sizeof interface);
    state.csim = strcmp(interface, "logical") != 0;
    lpac_set_interface(state.csim);
    screen_option("esim_backend", state.backend, sizeof state.backend);
    if (strcmp(state.backend, "system") && strcmp(state.backend, "at:curl") && strcmp(state.backend, "stdio"))
        state.backend[0] = 0;
    lpac_set_backend(state.backend);
    reset_se();
    state.current = 0;
    load();
}

void esim_close(void)
{
    if (!open_now)
        return;
    open_now = false;
    restore_se_focus = false;
    keyboard_close();
    install_cancel();
    if (state.view != V_WAIT && !state.downloading) {
        cancel_reload();
        lpac_stop();
    }
}

bool esim_back(void)
{
    switch (state.view) {
    case V_LOADING:
    case V_ROOT:
    case V_MISSING:
        return false;
    case V_WAIT:
        return true;
    case V_NOTIF_LOADING:
        lpac_stop();
        state.view = V_ROOT;
        render();
        return true;
    case V_DOWNLOAD:
    case V_RENAME:
    case V_NOTIFS:
    case V_INFO:
    case V_INTERFACE:
        state.view = V_ROOT;
        render();
        return true;
    }
    return false;
}

static bool inside(lv_obj_t *obj, lv_obj_t *parent)
{
    while (obj && obj != parent)
        obj = lv_obj_get_parent(obj);
    return obj != NULL;
}

/* The first (or last) entry the keys can reach under parent. */
static lv_obj_t *end_entry(lv_obj_t *parent, bool last)
{
    uint32_t n = lv_obj_get_child_count(parent);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *child = lv_obj_get_child(parent, (int32_t)(last ? n - 1 - i : i));
        if (lv_obj_is_hidden(child))
            continue;
        if (lv_obj_get_group(child) == input_group() && !lv_obj_has_state(child, LV_STATE_DISABLED))
            return child;
        if ((child = end_entry(child, last)))
            return child;
    }
    return NULL;
}

static void focus_entry(lv_obj_t *entry)
{
    if (!entry)
        return;
    lv_group_focus_obj(entry);
    lv_obj_add_state(entry, LV_STATE_FOCUS_KEY);
    lv_obj_scroll_to_view_recursive(entry, LV_ANIM_ON);
}

/* The switcher sits above the first entry: Left and Right pick the card there,
 * Up from the first entry walks onto it. */
bool esim_key(const struct key_event *key)
{
    lv_obj_t *focused = input_key_focus(input_group());
    if (state.view == V_INTERFACE && focused &&
        (focused == ui.interfaces[0] || focused == ui.interfaces[1]) &&
        (key->kind == KEY_KIND_LEFT || key->kind == KEY_KIND_RIGHT)) {
        choose_interface(key->kind == KEY_KIND_LEFT);
        return true;
    }
    if (state.view != V_ROOT && state.view != V_NOTIFS)
        return key->kind == KEY_KIND_LEFT || key->kind == KEY_KIND_RIGHT;
    if (state.view == V_NOTIFS && !on_switcher && focused && ui.notif_actions) {
        if (inside(focused, ui.notif_actions)) {
            switch (key->kind) {
            case KEY_KIND_LEFT:
            case KEY_KIND_RIGHT:
                focus_entry(lv_obj_get_child(ui.notif_actions, key->kind == KEY_KIND_RIGHT));
                return true;
            case KEY_KIND_DOWN:
                focus_entry(end_entry(ui.notif_list, false));
                return true;
            case KEY_KIND_UP:
                focus_entry(lv_obj_get_child(ui.notif_actions, 0));
                break;
            default:
                break;
            }
        } else if (key->kind == KEY_KIND_UP && focused == end_entry(ui.notif_list, false)) {
            focus_entry(lv_obj_get_child(ui.notif_actions, 0));
            return true;
        }
    }
    switch (key->kind) {
    case KEY_KIND_LEFT:
    case KEY_KIND_RIGHT:
        if (!on_switcher && ui.se_switcher && input_key_focus(input_group()) == ui.se_switcher) {
            int index = key->kind == KEY_KIND_RIGHT;
            if (state.slots[state.current].ses & (1u << index))
                select_se(index);
            return true;
        }
        if (on_switcher && state.current != (key->kind == KEY_KIND_RIGHT)) {
            state.current = key->kind == KEY_KIND_RIGHT;
            load();
        }
        return true;
    case KEY_KIND_UP:
        if (on_switcher) {
            on_switcher = false;
            paint_switcher();
            focus_entry(end_entry(ui.body, true));
            return true;
        }
        lv_obj_t *focused = input_key_focus(input_group());
        if (focused && inside(focused, ui.body) && focused != end_entry(ui.body, false))
            return false;
        input_defocus();
        on_switcher = true;
        paint_switcher();
        lv_obj_scroll_to_view_recursive(ui.switcher, LV_ANIM_ON);
        return true;
    case KEY_KIND_DOWN:
        if (!on_switcher)
            return false;
        on_switcher = false;
        paint_switcher();
        focus_entry(end_entry(ui.body, false));
        return true;
    case KEY_KIND_OK:
        return on_switcher || (ui.se_switcher && input_key_focus(input_group()) == ui.se_switcher);
    default:
        return false;
    }
}
