/* The phone: dialer and call screen over the voice backend (/api/plugins/phone).
 * An incoming call opens it above the lock screen; the lock underneath stays locked. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api.h"
#include "app.h"
#include "icons.h"
#include "input.h"
#include "theme.h"
#include "ui.h"
#include "util.h"

#define TONES 32
/* LVGL's long-press time, which the on-screen delete key uses. */
#define LONG_PRESS_MS LV_INDEV_DEF_LONG_PRESS_TIME

static struct {
    lv_obj_t *root, *body;
    lv_obj_t *number, *placeholder, *dial, *erase, *sims[2];
    lv_obj_t *call_state, *call_name, *call_sim, *duration, *choices, *avatar, *controls;
    lv_obj_t *accept, *hangup, *route, *mute, *keys, *keypad, *digits;
    lv_obj_t *sheet;
} ui;

static lv_group_t *group, *outer_group;
static bool opened, call_only, showing;
static cJSON *calls, *audio;
static char ringing[256];
static char number[33];
static int card;
static bool card_picked;
static const char *pending;
static char last_notice[256];
static bool full;
static uint32_t guard_until, dial_after, awaiting_since, active_since;
static char awaiting[32], active_id[32], selected[32], tone_call[32];
static char tone_digits[TONES + 1];
static struct {
    char id[32];
    char tone;
} tones[TONES];
static int tone_count;
static bool sending_tone, keypad_open, power_captured, zero_held;
static lv_timer_t *clock_timer, *zero_timer;
static uint32_t back_down;

static void render(void);
static void draw(void);
static void build(void);
static void mount(bool fullscreen);

/* ---------- state ---------- */

static bool live_state(const char *state)
{
    static const char *const live[] = {"dialing", "ringing-out", "ringing-in", "waiting", "active", "held"};
    for (size_t i = 0; state && i < sizeof live / sizeof *live; i++)
        if (!strcmp(state, live[i]))
            return true;
    return false;
}

static bool incoming(const cJSON *c)
{
    const char *state = jstr(c, "state");
    return state && (!strcmp(state, "ringing-in") || !strcmp(state, "waiting"));
}

static void call_id(const cJSON *c, char *out)
{
    const cJSON *id = jget(c, "id");
    if (cJSON_IsNumber(id))
        snprintf(out, 32, "%g", id->valuedouble);
    else
        snprintf(out, 32, "%s", cJSON_IsString(id) ? id->valuestring : "");
}

static const cJSON *current(void)
{
    const cJSON *c, *in = NULL;
    char id[32];
    cJSON_ArrayForEach(c, calls) {
        call_id(c, id);
        if (*selected && !strcmp(id, selected))
            return c;
        if (!in && incoming(c))
            in = c;
    }
    return in ? in : cJSON_GetArrayItem(calls, 0);
}

static const char *state_word(const char *state)
{
    static const char *const words[][3] = {
        {"dialing", "正在拨出…", "Calling…"}, {"ringing-out", "等待对方接听", "Ringing…"},
        {"ringing-in", "来电", "Incoming call"}, {"waiting", "来电等待", "Call waiting"},
        {"active", "通话中", "In call"}, {"held", "通话保持", "On hold"},
    };
    for (size_t i = 0; state && i < sizeof words / sizeof *words; i++)
        if (!strcmp(state, words[i][0]))
            return L(words[i][1], words[i][2]);
    return state ? state : "";
}

/* Background state repeats with every update: show each message once. */
static void notice(const char *text)
{
    if (text && *text && strcmp(text, last_notice))
        toast("%s", text);
    snprintf(last_notice, sizeof last_notice, "%s", text ? text : "");
}

bool phone_is_open(void)
{
    return opened;
}

bool phone_call_active(void)
{
    return showing;
}

bool phone_keep_awake(void)
{
    return opened && (cJSON_GetArraySize(calls) || pending);
}

/* ---------- requests ---------- */

static void status_read(int status, cJSON *r, void *ctx)
{
    if (!api_read_ok(status, r)) {
        notice(jstr_or(r, "error", L("服务不可用", "Unavailable")));
        render();
        return;
    }
    double sim;
    if (!card_picked && jnum(r, "sim.card", &sim) && (sim == 0 || sim == 1))
        card = (int)sim;
    phone_calls(r);
}

static void read_status(void)
{
    api_get("/plugins/phone/status", status_read, NULL);
}

struct action {
    char name[16];
};

static void performed(int status, cJSON *r, void *ctx)
{
    struct action *a = ctx;
    if (status >= 200 && status < 300 && api_reply_ok(r)) {
        if (!strcmp(a->name, "call")) {
            call_id(r, awaiting);
            awaiting_since = lv_tick_get();
        }
        read_status();
    } else {
        char error[200];
        error_text(jstr_or(r, "error", status ? "HTTP error" : L("请求超时，通话结果尚未确认。请查看通话状态，不要重复拨号。",
                   "Request timed out; call result is unknown. Check its state before dialing again.")), error, sizeof error);
        toast("%s%s%s", jstr_or(r, "stage", ""), jstr(r, "stage") ? ": " : "", error);
    }
    if (!strcmp(a->name, "call") || !strcmp(a->name, "hangup"))
        dial_after = lv_tick_get() + 2000;
    pending = NULL;
    free(a);
    render();
}

static void perform(const char *name, cJSON *body)
{
    if (pending) {
        cJSON_Delete(body);
        return;
    }
    if (!strcmp(name, "hangup"))
        tone_count = 0;
    static const char *const names[] = {"call", "answer", "hangup", "audio"};
    for (size_t i = 0; i < sizeof names / sizeof *names; i++)
        if (!strcmp(name, names[i]))
            pending = names[i];
    struct action *a = calloc(1, sizeof *a);
    snprintf(a->name, sizeof a->name, "%s", name);
    char path[48];
    snprintf(path, sizeof path, "/plugins/phone/%s", name);
    api_post(path, body, performed, a);
    render();
}

static void call_action(const char *name, const cJSON *c)
{
    cJSON *body = cJSON_CreateObject();
    cJSON_AddItemToObject(body, "id", cJSON_Duplicate(jget(c, "id"), true));
    perform(name, body);
}

static bool valid_number(const char *n)
{
    const char *p = n + (*n == '+');
    size_t len = strlen(p);
    if (len < 3 || len > 32)
        return false;
    for (; *p; p++)
        if (!isdigit((unsigned char)*p) && *p != '*' && *p != '#')
            return false;
    return true;
}

static void dial(void)
{
    if (pending || current() || lv_tick_get() < dial_after || *awaiting)
        return;
    if (!valid_number(number)) {
        toast("%s", L("请输入至少 3 位有效号码", "Enter a valid number (at least 3 digits)"));
        return;
    }
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "number", number);
    cJSON_AddNumberToObject(body, "card", card);
    perform("call", body);
}

static void send_next_tone(void);

static void tone_sent(int status, cJSON *r, void *ctx)
{
    const cJSON *c = current();
    char id[32] = "";
    call_id(c, id);
    if (!api_reply_ok(r)) {
        tone_count = 0;
        sending_tone = false;
        toast("%s: %s", L("按键音发送失败，请确认通话状态后重试", "Tone failed; check the call before trying again"),
              jstr_or(r, "error", ""));
        return;
    }
    if (strcmp(id, tones[0].id) || !jstr_is(c, "state", "active")) {
        tone_count = 0;
        sending_tone = false;
        return;
    }
    size_t len = strlen(tone_digits);
    if (len == TONES)
        memmove(tone_digits, tone_digits + 1, --len);
    tone_digits[len] = tones[0].tone;
    tone_digits[len + 1] = 0;
    memmove(tones, tones + 1, sizeof tones[0] * (size_t)--tone_count);
    if (ui.digits)
        ui_set_text(ui.digits, tone_digits);
    send_next_tone();
}

static void tone_gap(lv_timer_t *t)
{
    const cJSON *c = current();
    char id[32];
    call_id(c, id);
    if (!tone_count || strcmp(id, tones[0].id)) {
        tone_count = 0;
        sending_tone = false;
        return;
    }
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "id", tones[0].id);
    char tone[2] = {tones[0].tone, 0};
    cJSON_AddStringToObject(body, "tone", tone);
    api_post("/plugins/phone/dtmf", body, tone_sent, NULL);
}

static void send_next_tone(void)
{
    if (!tone_count) {
        sending_tone = false;
        return;
    }
    sending_tone = true;
    lv_timer_set_repeat_count(lv_timer_create(tone_gap, 60, NULL), 1);
}

static void send_tone(char tone)
{
    const cJSON *c = current();
    const char *state = jstr(c, "state");
    if (!state || strcmp(state, "active") || pending || !strchr("0123456789*#", tone) || tone_count >= TONES)
        return;
    call_id(c, tones[tone_count].id);
    tones[tone_count++].tone = tone;
    if (!sending_tone)
        send_next_tone();
}

static void set_number(const char *value)
{
    snprintf(number, sizeof number, "%s", value);
    render();
}

static void append(char ch)
{
    size_t len = strlen(number);
    if (len + 1 < sizeof number) {
        char next[33];
        snprintf(next, sizeof next, "%s%c", number, ch);
        set_number(next);
    }
}

static void backspace(void)
{
    char next[33];
    snprintf(next, sizeof next, "%s", number);
    if (*next)
        next[strlen(next) - 1] = 0;
    set_number(next);
}

/* ---------- live call state ---------- */

static void close_sheet(void)
{
    if (ui.sheet) {
        lv_obj_delete(ui.sheet);
        ui.sheet = NULL;
    }
}

static void apply_live(const cJSON *r)
{
    const cJSON *error = jget(r, "error");
    bool failed = error && !cJSON_IsNull(error);
    if (!failed && cJSON_IsArray(jget(r, "calls"))) {
        cJSON *list = cJSON_CreateArray();
        const cJSON *c;
        cJSON_ArrayForEach(c, jget(r, "calls"))
            if (live_state(jstr(c, "state")))
                cJSON_AddItemToArray(list, cJSON_Duplicate(c, true));
        cJSON_Delete(calls);
        calls = list;
    }
    if (cJSON_IsObject(jget(r, "audio"))) {
        cJSON_Delete(audio);
        audio = cJSON_Duplicate(jget(r, "audio"), true);
        const char *state = jstr(current(), "state");
        bool missing = state && !strcmp(state, "active") && (!jtrue(audio, "active") || jhas(audio, "error"));
        notice(missing ? jstr_or(audio, "error", L("通话音频尚未启动", "Call audio has not started")) : NULL);
    }
    if (failed && cJSON_IsString(error))
        notice(error->valuestring);
    if (*awaiting) {
        const cJSON *c;
        char id[32];
        bool seen = false;
        cJSON_ArrayForEach(c, calls) {
            call_id(c, id);
            seen |= !strcmp(id, awaiting);
        }
        if (seen) {
            awaiting[0] = 0;
        } else if (lv_tick_elaps(awaiting_since) > 6000) {
            awaiting[0] = 0;
            toast("%s", L("拨号请求已发送，暂未收到通话状态。", "Call requested; waiting for modem state."));
        }
    }
}

/* Every call update: an incoming call wakes the screen (unless the user
 * turned that off) and opens the call screen; the screen closes once no
 * call is left. */
void phone_calls(cJSON *event)
{
    const cJSON *list = jget(event, "calls"), *c;
    char now[256] = "", id[32];
    bool fresh = false, any_incoming = false, any_live = false;
    cJSON_ArrayForEach(c, list) {
        if (!live_state(jstr(c, "state")))
            continue;
        any_live = true;
        if (!incoming(c))
            continue;
        any_incoming = true;
        call_id(c, id);
        char token[34];
        snprintf(token, sizeof token, "|%s|", id);
        fresh |= !strstr(ringing, token);
        size_t len = strlen(now);
        snprintf(now + len, sizeof now - len, "|%s|", id);
    }
    snprintf(ringing, sizeof ringing, "%s", now);
    apply_live(event);
    if (any_incoming && (fresh || (!showing && !app.blank)) && !(app.blank && jfalse(event, "wake"))) {
        if (app.blank)
            screen_set_blank(false);
        power_menu_close();
        if (!showing) {
            showing = true;
            guard_until = lv_tick_get() + 600;
            if (opened)
                phone_close();
            call_only = true;
            phone_open();
        }
    }
    if (showing && !any_live) {
        showing = false;
        if (opened)
            phone_close();
        idle_reset();
    }
    if (opened)
        render();
}

void phone_screen_wake(void)
{
    if (ringing[0] && !showing) {
        cJSON *event = cJSON_CreateObject();
        cJSON_AddItemToObject(event, "calls", cJSON_Duplicate(calls, true));
        cJSON_AddBoolToObject(event, "wake", true);
        ringing[0] = 0;
        phone_calls(event);
        cJSON_Delete(event);
    }
}

/* ---------- widgets ---------- */

static bool guarded(void)
{
    return lv_tick_get() < guard_until || app.blank || jfalse(screen_config(), "touch");
}

static void on_key(lv_event_t *e)
{
    if (guarded() || pending)
        return;
    char ch = (char)(intptr_t)lv_event_get_user_data(e);
    if (ch == '0' && zero_held) {
        zero_held = false;
        return;
    }
    append(ch);
}

static void zero_long(lv_timer_t *t)
{
    lv_timer_pause(zero_timer);
    if (!pending && !*number) {
        zero_held = true;
        set_number("+");
    }
}

static void on_zero(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        zero_held = false;
        lv_timer_reset(zero_timer);
        lv_timer_resume(zero_timer);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        lv_timer_pause(zero_timer);
    }
}

static void on_tone(lv_event_t *e)
{
    if (!guarded())
        send_tone((char)(intptr_t)lv_event_get_user_data(e));
}

static void on_dial(lv_event_t *e)
{
    if (!guarded())
        dial();
}

static void on_backspace(lv_event_t *e)
{
    if (!guarded() && !pending)
        backspace();
}

static void on_clear(lv_event_t *e)
{
    if (!guarded() && !pending)
        set_number("");
}

static void on_sim(lv_event_t *e)
{
    if (guarded() || pending)
        return;
    card = (int)(intptr_t)lv_event_get_user_data(e);
    card_picked = true;
    render();
}

static void on_accept(lv_event_t *e)
{
    const cJSON *c = current();
    if (!guarded() && incoming(c))
        call_action("answer", c);
}

static void on_hangup(lv_event_t *e)
{
    const cJSON *c = current();
    if (!guarded() && c)
        call_action("hangup", c);
}

static void on_mute(lv_event_t *e)
{
    if (guarded() || !current())
        return;
    cJSON *body = cJSON_CreateObject();
    cJSON_AddBoolToObject(body, "muted", !jtrue(audio, "muted"));
    perform("audio", body);
}

static void on_keypad_toggle(lv_event_t *e)
{
    keypad_open = !keypad_open;
    render();
}

static void on_choice(lv_event_t *e)
{
    const cJSON *c = cJSON_GetArrayItem(calls, (int)(intptr_t)lv_event_get_user_data(e));
    if (c)
        call_id(c, selected);
    render();
}

static lv_obj_t *sheet(const char *title)
{
    close_sheet();
    ui.sheet = lv_obj_create(ui.root);
    lv_obj_remove_style_all(ui.sheet);
    lv_obj_set_size(ui.sheet, LV_PCT(100), LV_PCT(100));
    lv_obj_set_floating(ui.sheet, true);
    lv_obj_set_style_bg_color(ui.sheet, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ui.sheet, LV_OPA_60, 0);
    lv_obj_t *panel = ui_box(ui.sheet, LV_FLEX_FLOW_COLUMN, 10);
    lv_obj_set_clickable(panel, true);
    lv_obj_align(panel, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(panel, theme.bg, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, S(20), 0);
    lv_obj_set_style_pad_all(panel, S(16), 0);
    lv_label_set_text(ui_text(panel, 16, true, theme.text), title);
    return panel;
}

static void on_route(lv_event_t *e)
{
    const char *route = lv_event_get_user_data(e);
    close_sheet();
    if (!current())
        return;
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "route", route);
    perform("audio", body);
}

static void on_sheet_close(lv_event_t *e)
{
    close_sheet();
    render();
}

static void on_route_sheet(lv_event_t *e)
{
    if (guarded() || !current())
        return;
    lv_group_t *previous = lv_group_get_default();
    lv_group_set_default(group);
    lv_obj_t *panel = sheet(L("通话音频", "Call audio"));
    const char *route = jstr_or(audio, "route", jtrue(audio, "speaker") ? "speaker" : "receiver");
    bool bt = jtrue(audio, "bluetooth.connected");
    static const char *const routes[] = {"receiver", "speaker", "bluetooth"};
    const char *names[] = {L("手机", "Phone"), L("扬声器", "Speaker"), L("蓝牙耳机", "Bluetooth")};
    const char *icons[] = {"device", "volume", "bluetooth"};
    for (int i = 0; i < 3; i++) {
        const char *detail = i == 2 ? (bt ? jstr_or(audio, "bluetooth.device", names[2])
                                          : L("未连接通话耳机", "No call headset connected"))
                                    : NULL;
        lv_obj_t *r = ui_row(panel, icons[i], names[i], detail, !strcmp(route, routes[i]) ? ROW_CHECK : 0, on_route,
                             (void *)routes[i]);
        if (i == 2 && !bt)
            lv_obj_set_state(r, LV_STATE_DISABLED, true);
    }
    ui_button(panel, L("完成", "Done"), on_sheet_close, NULL);
    lv_group_set_default(previous);
}

static lv_obj_t *circle(lv_obj_t *parent, int size, const char *icon, lv_color_t bg, lv_color_t fg, lv_event_cb_t cb)
{
    lv_obj_t *b = ui_button(parent, "", cb, NULL);
    lv_obj_delete(lv_obj_get_child(b, 0));
    lv_obj_set_size(b, S(size), S(size));
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_bg_color(b, lv_color_mix(lv_color_white(), bg, 50), LV_STATE_PRESSED);
    lv_obj_t *glyph = icon_create(b, icon, S(size * 2 / 5));
    lv_obj_set_style_text_color(glyph, fg, 0);
    lv_obj_center(glyph);
    return b;
}

/* A round control with its caption below; the caption is the second child of the column. */
static lv_obj_t *control(lv_obj_t *parent, const char *icon, lv_event_cb_t cb)
{
    lv_obj_t *column = ui_box(parent, LV_FLEX_FLOW_COLUMN, 6);
    lv_obj_set_width(column, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(column, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    circle(column, 56, icon, theme.button, theme.text, cb);
    ui_text(column, 11, false, theme.dim);
    return column;
}

static void control_set(lv_obj_t *column, const char *caption, bool on, bool disabled)
{
    lv_obj_t *button = lv_obj_get_child(column, 0);
    lv_obj_set_style_bg_color(button, on ? theme.accent : theme.button, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(button, 0), on ? theme.bg : theme.text, 0);
    lv_obj_set_state(button, LV_STATE_DISABLED, disabled);
    ui_set_text(lv_obj_get_child(column, 1), caption);
}

static void keypad(lv_obj_t *parent, lv_event_cb_t cb, bool letters)
{
    static const char keys[] = "123456789*0#";
    static const char *const sub[] = {"", "ABC", "DEF", "GHI", "JKL", "MNO", "PQRS", "TUV", "WXYZ", "", "+", ""};
    lv_obj_t *pad = ui_box(parent, LV_FLEX_FLOW_ROW_WRAP, 0);
    lv_obj_set_style_pad_row(pad, S(6), 0);
    lv_obj_set_style_pad_column(pad, S(10), 0);
    int32_t width = (lv_display_get_horizontal_resolution(lv_display_get_default()) - 2 * S(16) - 2 * S(10)) / 3;
    for (int i = 0; i < 12; i++) {
        lv_obj_t *b = ui_button(pad, "", cb, (void *)(intptr_t)keys[i]);
        lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_set_style_pad_gap(b, 0, 0);
        lv_obj_set_style_radius(b, S(20), 0);
        lv_obj_set_style_bg_color(b, theme.surface, 0);
        lv_obj_set_style_bg_opa(b, theme.surface_opa, 0);
        lv_obj_set_size(b, width, S(40));
        char text[2] = {keys[i], 0};
        lv_obj_t *digit = lv_obj_get_child(b, 0);
        lv_label_set_text(digit, text);
        lv_obj_set_style_text_font(digit, F(22), 0);
        if (letters && *sub[i]) {
            lv_obj_t *small = ui_text(b, 8, false, theme.dim);
            lv_obj_set_style_text_letter_space(small, 1, 0);
            lv_obj_set_style_margin_top(small, -S(5), 0);
            lv_label_set_text(small, sub[i]);
        }
        if (keys[i] == '0' && letters)
            lv_obj_add_event_cb(b, on_zero, LV_EVENT_ALL, NULL);
    }
}

static void build_dialer(void)
{
    lv_obj_t *sims = ui_box(ui.body, LV_FLEX_FLOW_ROW, 0);
    lv_obj_set_style_bg_color(sims, theme.surface, 0);
    lv_obj_set_style_bg_opa(sims, theme.surface_opa, 0);
    lv_obj_set_style_radius(sims, S(18), 0);
    lv_obj_set_style_pad_all(sims, S(3), 0);
    for (int i = 0; i < 2; i++) {
        char text[8];
        snprintf(text, sizeof text, "SIM %d", i + 1);
        ui.sims[i] = ui_button(sims, text, on_sim, (void *)(intptr_t)i);
        lv_obj_set_flex_grow(ui.sims[i], 1);
        lv_obj_set_style_pad_ver(ui.sims[i], S(5), 0);
        lv_obj_set_style_radius(ui.sims[i], S(15), 0);
        lv_obj_set_style_text_font(lv_obj_get_child(ui.sims[i], 0), F(12), 0);
    }
    lv_obj_t *area = ui_box(ui.body, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_set_flex_grow(area, 1);
    lv_obj_set_style_min_height(area, S(44), 0);
    lv_obj_set_flex_align(area, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ui.placeholder = ui_text(area, 13, false, theme.dim);
    lv_label_set_text(ui.placeholder, L("输入电话号码", "Enter a number"));
    ui.number = ui_text(area, 28, false, theme.text);
    lv_obj_set_width(ui.number, LV_PCT(100));
    lv_obj_set_style_text_align(ui.number, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_letter_space(ui.number, 1, 0);
    lv_label_set_long_mode(ui.number, LV_LABEL_LONG_MODE_CLIP);
    keypad(ui.body, on_key, true);
    lv_obj_t *actions = ui_box(ui.body, LV_FLEX_FLOW_ROW, 0);
    lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *spacer = lv_obj_create(actions);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_size(spacer, S(44), 1);
    ui.dial = circle(actions, 56, "phone", theme.accent, theme.bg, on_dial);
    ui.erase = circle(actions, 44, "backspace", theme.bg, theme.dim, NULL);
    lv_obj_add_event_cb(ui.erase, on_backspace, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(ui.erase, on_clear, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_set_style_bg_opa(ui.erase, LV_OPA_TRANSP, 0);
}

static void build_call(void)
{
    ui.choices = ui_box(ui.body, LV_FLEX_FLOW_ROW, 6);
    lv_obj_t *head = ui_box(ui.body, LV_FLEX_FLOW_COLUMN, 2);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ui.call_state = ui_text(head, 13, false, theme.accent);
    ui.call_sim = ui_text(head, 11, false, theme.dim);
    ui.avatar = lv_obj_create(ui.body);
    lv_obj_remove_style_all(ui.avatar);
    lv_obj_set_size(ui.avatar, S(84), S(84));
    lv_obj_set_style_radius(ui.avatar, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(ui.avatar, theme.active, 0);
    lv_obj_set_style_bg_opa(ui.avatar, LV_OPA_COVER, 0);
    lv_obj_set_style_align(ui.avatar, LV_ALIGN_CENTER, 0);
    lv_obj_t *glyph = icon_create(ui.avatar, "user", S(38));
    lv_obj_set_style_text_color(glyph, theme.accent, 0);
    lv_obj_center(glyph);
    lv_obj_t *who = ui_box(ui.body, LV_FLEX_FLOW_COLUMN, 4);
    lv_obj_set_flex_align(who, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ui.call_name = ui_text(who, 26, false, theme.text);
    lv_obj_set_style_text_letter_space(ui.call_name, 1, 0);
    ui.duration = ui_text(who, 14, false, theme.dim);
    ui.keypad = ui_box(ui.body, LV_FLEX_FLOW_COLUMN, 4);
    ui.digits = ui_text(ui.keypad, 18, false, theme.text);
    lv_obj_set_width(ui.digits, LV_PCT(100));
    lv_obj_set_style_text_align(ui.digits, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_letter_space(ui.digits, 3, 0);
    lv_label_set_text(ui.digits, tone_digits);
    keypad(ui.keypad, on_tone, false);
    lv_obj_t *spacer = lv_obj_create(ui.body);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_flex_grow(spacer, 1);
    ui.controls = ui_box(ui.body, LV_FLEX_FLOW_ROW, 0);
    lv_obj_set_flex_align(ui.controls, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    ui.route = control(ui.controls, "volume", on_route_sheet);
    ui.mute = control(ui.controls, "mic-off", on_mute);
    ui.keys = control(ui.controls, "keypad", on_keypad_toggle);
    lv_obj_t *actions = ui_box(ui.body, LV_FLEX_FLOW_ROW, 0);
    lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(actions, S(6), 0);
    ui.hangup = circle(actions, 64, "hangup", lv_color_hex(0xe5484d), lv_color_white(), on_hangup);
    ui.accept = circle(actions, 64, "phone", lv_color_hex(0x30b86e), lv_color_white(), on_accept);
}

/* The dialer and the call screen are laid out for the panel: past the standard
 * size the keypad would no longer fit, so they never scale up. */
static void render(void)
{
    float scale = theme_cap(1);
    draw();
    theme.scale = scale;
}

static void draw(void)
{
    if (!opened)
        return;
    const cJSON *c = current();
    char id[32] = "";
    call_id(c, id);
    if (strcmp(tone_call, id)) {
        snprintf(tone_call, sizeof tone_call, "%s", id);
        tone_digits[0] = 0;
        tone_count = 0;
        keypad_open = false;
    }
    const char *state = jstr(c, "state");
    bool active = state && !strcmp(state, "active");
    if (!active) {
        tone_count = 0;
        keypad_open = false;
    }
    bool in_call = c != NULL, built_call = ui.call_state != NULL;
    if ((call_only || in_call) != full)
        mount(call_only || in_call);
    else if (in_call != built_call)
        build();

    if (!in_call) {
        lv_obj_set_hidden(ui.placeholder, *number != 0);
        lv_obj_set_hidden(ui.number, *number == 0);
        ui_set_text(ui.number, number);
        size_t len = strlen(number);
        lv_obj_set_style_text_font(ui.number, F(len > 16 ? 20 : len > 12 ? 24 : 28), 0);
        for (int i = 0; i < 2; i++) {
            bool on = i == card;
            lv_obj_set_style_bg_opa(ui.sims[i], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
            lv_obj_set_style_bg_color(ui.sims[i], theme.active, 0);
            lv_obj_set_style_text_color(lv_obj_get_child(ui.sims[i], 0), on ? theme.accent : theme.dim, 0);
            lv_obj_set_state(ui.sims[i], LV_STATE_DISABLED, pending != NULL);
        }
        bool settling = lv_tick_get() < dial_after || *awaiting;
        lv_obj_set_state(ui.dial, LV_STATE_DISABLED, pending || settling);
        lv_obj_set_style_opa(ui.erase, *number ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_clickable(ui.erase, *number != 0);
        return;
    }

    lv_obj_clean(ui.choices);
    lv_obj_set_hidden(ui.choices, cJSON_GetArraySize(calls) < 2);
    const cJSON *x;
    int i = 0;
    cJSON_ArrayForEach(x, calls) {
        char label[96], xid[32];
        call_id(x, xid);
        snprintf(label, sizeof label, "%s%s%s", jstr_or(x, "sim", ""), jstr(x, "sim") ? " · " : "",
                 state_word(jstr(x, "state")));
        lv_obj_t *chip = ui_button(ui.choices, label, on_choice, (void *)(intptr_t)i++);
        lv_obj_set_flex_grow(chip, 1);
        lv_obj_set_style_text_font(lv_obj_get_child(chip, 0), F(11), 0);
        lv_obj_set_style_bg_color(chip, !strcmp(xid, id) ? theme.active : theme.button, 0);
    }
    ui_set_text(ui.call_state, state_word(state));
    ui_set_text(ui.call_sim, jstr_or(c, "sim", ""));
    const char *num = jstr(c, "number");
    ui_set_text(ui.call_name, num && *num ? num : L("未知号码", "Unknown"));
    if (active && strcmp(active_id, id)) {
        snprintf(active_id, sizeof active_id, "%s", id);
        active_since = lv_tick_get();
    } else if (!active) {
        active_id[0] = 0;
    }
    if (*active_id) {
        uint32_t s = lv_tick_elaps(active_since) / 1000;
        ui_set_fmt(ui.duration, "%u:%02u", s / 60, s % 60);
    } else {
        ui_set_text(ui.duration, "");
    }
    lv_obj_set_hidden(ui.accept, !incoming(c));
    lv_obj_set_state(ui.accept, LV_STATE_DISABLED, pending != NULL);
    lv_obj_set_state(ui.hangup, LV_STATE_DISABLED, pending != NULL);
    lv_obj_set_hidden(ui.avatar, keypad_open);
    lv_obj_set_hidden(ui.keypad, !keypad_open);
    const char *audio_sim = jstr(audio, "sim"), *call_sim = jstr(c, "sim");
    bool other = jtrue(audio, "active") && audio_sim && call_sim && strcmp(audio_sim, call_sim);
    bool unavailable = pending || !jtrue(audio, "available") || other;
    const char *route = jstr_or(audio, "route", jtrue(audio, "speaker") ? "speaker" : "receiver");
    control_set(ui.route,
                jtrue(audio, "route_pending") ? L("连接蓝牙…", "Connecting…")
                : !strcmp(route, "speaker")   ? L("扬声器", "Speaker")
                : !strcmp(route, "bluetooth") ? L("蓝牙耳机", "Bluetooth")
                                              : L("听筒", "Earpiece"),
                strcmp(route, "receiver") != 0, unavailable);
    control_set(ui.mute, L("静音", "Mute"), jtrue(audio, "muted"), unavailable);
    control_set(ui.keys, keypad_open ? L("收起", "Hide") : L("键盘", "Keypad"), keypad_open, !active);
}

static void build(void)
{
    if (!ui.root)
        return;
    lv_obj_clean(ui.body);
    ui.call_state = ui.placeholder = NULL;
    lv_group_remove_all_objs(group);
    lv_group_t *previous = lv_group_get_default();
    lv_group_set_default(group);
    if (current())
        build_call();
    else
        build_dialer();
    lv_group_set_default(previous);
}

static void tick(lv_timer_t *t)
{
    if (!opened)
        return;
    if (*awaiting && lv_tick_elaps(awaiting_since) > 6000) {
        awaiting[0] = 0;
        toast("%s", L("拨号请求已发送，暂未收到通话状态。", "Call requested; waiting for modem state."));
    }
    if (dial_after && lv_tick_get() >= dial_after)
        dial_after = 0;
    render();
}

void phone_init(void)
{
    memset(&ui, 0, sizeof ui);
    if (!group) {
        group = lv_group_create();
        lv_group_set_wrap(group, false);
        clock_timer = lv_timer_create(tick, 1000, NULL);
        zero_timer = lv_timer_create(zero_long, 700, NULL);
        lv_timer_pause(zero_timer);
    }
    if (opened) {
        opened = false;
        phone_open();
    }
}

/* The dialer sits in the shell's content area; a call takes the whole display. */
static void mount(bool fullscreen)
{
    float scale = theme_cap(1);
    close_sheet();
    if (ui.root)
        lv_obj_delete(ui.root);
    memset(&ui, 0, sizeof ui);
    full = fullscreen;
    ui.root = lv_obj_create(full ? lv_layer_top() : shell_content());
    lv_obj_remove_style_all(ui.root);
    lv_obj_set_floating(ui.root, true);
    lv_obj_set_size(ui.root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(ui.root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(ui.root, full ? theme.hero_start : theme.bg, 0);
    lv_obj_set_style_bg_grad_color(ui.root, theme.bg, 0);
    lv_obj_set_style_bg_grad_dir(ui.root, full ? LV_GRAD_DIR_VER : LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_pad_hor(ui.root, S(16), 0);
    lv_obj_set_style_pad_ver(ui.root, full ? S(28) : S(8), 0);
    lv_obj_set_scrollable(ui.root, false);
    ui.body = ui_box(ui.root, LV_FLEX_FLOW_COLUMN, 8);
    lv_obj_set_height(ui.body, LV_PCT(100));
    lv_obj_set_flex_align(ui.body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    build();
    theme.scale = scale;
}

void phone_open(void)
{
    if (opened || (!showing && (lock_active() || app.blank)))
        return;
    if (!showing)
        call_only = false;
    opened = true;
    outer_group = input_group();
    input_set_group(group);
    mount(call_only || current());
    render();
    shell_title();
    read_status();
    idle_reset();
}

void phone_close(void)
{
    if (!opened)
        return;
    close_sheet();
    opened = false;
    lv_obj_delete(ui.root);
    memset(&ui, 0, sizeof ui);
    lv_group_remove_all_objs(group);
    input_set_group(outer_group);
    shell_title();
    idle_reset();
}

bool phone_key(const struct key_event *key)
{
    const cJSON *c = current();
    if (key->kind == KEY_KIND_POWER) {
        if (key->down) {
            power_captured = opened && c && !app.blank && !power_menu_is_open();
            if (power_captured)
                call_action("hangup", c);
            return power_captured;
        }
        bool captured = power_captured;
        power_captured = false;
        return captured;
    }
    if (!opened || !key->down)
        return false;
    idle_reset();
    /* Holding Back clears the number, as holding the delete key does. */
    if (key->kind == KEY_KIND_BACK && !key->repeat)
        back_down = lv_tick_get();
    if (key->kind == KEY_KIND_BACK && key->repeat && !c && *number && !ui.sheet && !keypad_open &&
        lv_tick_elaps(back_down) >= LONG_PRESS_MS) {
        set_number("");
        return true;
    }
    if (key->repeat && key->kind != KEY_KIND_CHAR && key->kind != KEY_KIND_UP && key->kind != KEY_KIND_DOWN)
        return true;
    if (ui.sheet) {
        if (key->kind == KEY_KIND_BACK)
            on_sheet_close(NULL);
        else if (key->kind == KEY_KIND_UP || key->kind == KEY_KIND_LEFT)
            input_focus_move(group, -1);
        else if (key->kind == KEY_KIND_DOWN || key->kind == KEY_KIND_RIGHT)
            input_focus_move(group, 1);
        else if (key->kind == KEY_KIND_OK)
            input_focus_key(LV_KEY_ENTER);
        return true;
    }
    switch (key->kind) {
    case KEY_KIND_CHAR:
        if (c && jstr_is(c, "state", "active")) {
            if (!key->repeat)
                send_tone(key->ch);
        } else if (!c && !pending && !call_only) {
            append(key->ch);
        }
        return true;
    case KEY_KIND_CALL:
        if (incoming(c))
            call_action("answer", c);
        else if (!c)
            dial();
        return true;
    case KEY_KIND_BACK:
        if (keypad_open) {
            keypad_open = false;
            render();
        } else if (!c && *number) {
            backspace();
        } else if (!c) {
            phone_close();
        }
        return true;
    case KEY_KIND_OK:
        if (lv_group_get_focused(group))
            input_focus_key(LV_KEY_ENTER);
        else
            input_focus_move(group, 1);
        return true;
    case KEY_KIND_UP:
    case KEY_KIND_LEFT:
        input_focus_move(group, -1);
        return true;
    case KEY_KIND_DOWN:
    case KEY_KIND_RIGHT:
        input_focus_move(group, 1);
        return true;
    case KEY_KIND_MENU:
        if (c)
            return true;
        phone_close();
        return false;
    default:
        return false;
    }
}
