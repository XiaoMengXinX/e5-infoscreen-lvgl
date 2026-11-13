#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"
#include "lvgl.h"

enum page {
    PAGE_OVERVIEW,
    PAGE_SIGNAL,
    PAGE_TRAFFIC,
    PAGE_SMS,
    PAGE_HOTSPOT,
    PAGE_DEVICE,
    PAGE_DETAILS,
    PAGE_SETTINGS,
    PAGE_APPS,
    PAGE_COUNT
};

enum key_kind {
    KEY_KIND_NONE,
    KEY_KIND_POWER,
    KEY_KIND_VOLUP,
    KEY_KIND_VOLDOWN,
    KEY_KIND_BACK,
    KEY_KIND_MENU,
    KEY_KIND_CALL,
    KEY_KIND_HOTSPOT,
    KEY_KIND_OK,
    KEY_KIND_UP,
    KEY_KIND_DOWN,
    KEY_KIND_LEFT,
    KEY_KIND_RIGHT,
    KEY_KIND_CHAR,
};

struct key_event {
    enum key_kind kind;
    char ch;
    bool down;
    bool repeat;
};

struct page_ops {
    const char *icon;
    const char *zh, *en;
    void (*create)(lv_obj_t *parent);
    void (*shown)(void);
    void (*hidden)(void);
    void (*status)(const cJSON *st);
    bool (*key)(const struct key_event *key);
    void (*touch)(void);
    bool (*back)(void);
    void (*top)(void);
    /* True while the page keeps horizontal swipes, as in an open app. */
    bool (*held)(void);
    /* The points of a touch no page swipe has taken; pressed is false at its end. */
    void (*drag)(bool pressed, lv_point_t point);
};

extern const struct page_ops overview_page, signal_page, traffic_page, sms_page, hotspot_page,
    device_page, details_page, settings_page, apps_page;

struct app {
    cJSON *status;
    bool en;
    bool blank;
    enum page page;
    int brightness;
};

extern struct app app;

static inline const char *L(const char *zh, const char *en) { return app.en ? en : zh; }

/* shell.c */
void shell_init(void);
void shell_rebuild(void);
void shell_show(enum page page);
void shell_step(int delta);
void shell_page_loading(enum page page, bool on);
void shell_page_error(enum page page, bool on);
void shell_title(void);
void shell_key(const struct key_event *key);
void shell_touch_press(void);
bool shell_touch_allowed(void);
/* time is when the panel sampled the point, in monotonic ms. */
void shell_swipe(bool pressed, lv_point_t point, double time);
void shell_poll_now(void);
/* A status poll once ms have passed, for a change the backend takes time to show. */
void shell_poll_after(uint32_t ms);
void shell_screen_changed(void);
void toast(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void idle_reset(void);
void screen_set_blank(bool blank);
void picture_open(const char *src, const char *caption);
bool shell_overlay_open(void);
const cJSON *screen_config(void);
lv_obj_t *shell_root(void);
/* Shows the recording time in the status bar; a negative value hides it. */
void shell_record_indicator(int seconds);
lv_obj_t *shell_content(void);
int shell_status_bar_height(void);
void shell_wallpaper_locked(bool locked);
void shell_bar_hide(void);
void shell_bar_over_lock(bool over);
void shell_bar_lock_place(int32_t y, lv_opa_t opa);

/* lock.c */
void lock_create(void);
void lock_engage(void);
void lock_dismiss(void);
bool lock_active(void);
bool lock_touch_blocked(void);
void lock_status(const cJSON *st);
void lock_clock(void);
void lock_power(bool blank);
bool lock_key(const struct key_event *key);
/* Every key press and release, for the * and # hold and the double OK unlock. */
void lock_key_held(const struct key_event *key);
/* The * and # unlock's setting, which the screen keeps itself (UCI lock_keys). */
bool lock_keys_enabled(void);
void lock_set_keys(bool on);
void lock_refresh_usage(void);

/* power_menu.c */
void power_menu_open(void);
void power_menu_close(void);
bool power_menu_is_open(void);
bool power_menu_key(const struct key_event *key);

/* dialog.c */
void dialog_open(const char *title, const char *note);
void dialog_set_closable(bool closable);
lv_obj_t *dialog_button(const char *text, lv_event_cb_t cb, void *ctx);
/* A dialog_button in the quiet style of Cancel, for the way out. */
lv_obj_t *dialog_button_quiet(const char *text, lv_event_cb_t cb, void *ctx);
void dialog_cancel(void);
void dialog_close(void);
/* The open dialog's sheet, for content above its buttons. */
lv_obj_t *dialog_sheet(void);
/* Called once the open dialog closes, however it closes. */
void dialog_on_close(void (*cb)(void));
bool dialog_is_open(void);
bool dialog_key(const struct key_event *key);

/* record.c */
void record_prompt(void);
bool record_active(void);
void record_stop(void);

/* phone.c */
void phone_init(void);
void phone_open(void);
void phone_close(void);
bool phone_is_open(void);
bool phone_call_active(void);
bool phone_key(const struct key_event *key);
bool phone_keep_awake(void);
void phone_screen_wake(void);
void phone_calls(cJSON *event);

/* pages/apps.c */
void apps_open_toolkit(void);

/* pages/settings.c */
void settings_reset(void);
void settings_open_category(const char *id, const char *title, const char *item);
/* The connected devices, as Devices in the settings opens them. */
void settings_open_devices(void);

/* pages/sms.c */
void sms_check(const cJSON *st);

/* pages/traffic.c */
void traffic_invalidate(void);
