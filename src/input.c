#include "input.h"

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "src/core/lv_group_private.h"
#include "src/indev/lv_indev_private.h"

#include "app.h"

#define SLOTS 10
#define BITS(n) (((n) + 63) / 64)
#define TEST(bits, n) (((bits)[(n) / 64] >> ((n) % 64)) & 1)

enum device_kind { DEVICE_KEYS, DEVICE_TOUCH };

struct device {
    int fd;
    enum device_kind kind;
    struct input_absinfo x, y;
};

static struct device devices[INPUT_MAX_DEVICES];
static int device_count;
static bool star_key;

static struct {
    int slot;
    int primary;
    bool active[SLOTS];
    int x[SLOTS], y[SLOTS];
    bool legacy_down;
    lv_point_t point;
    /* Monotonic ms. */
    double time;
    bool pressed;
} touch = {.primary = -1};

static lv_indev_t *touch_indev, *keypad_indev;
static int32_t width, height;

/* A touch that stops a coasting list, or moves further than a tap does, is not a click. */
#define TAP_SLOP 12

static struct {
    bool cancelled;
    lv_point_t point;
} press;

static void touch_click(lv_event_t *e)
{
    if (press.cancelled)
        lv_indev_stop_processing(touch_indev);
}

static struct {
    uint32_t key[8];
    lv_indev_state_t state[8];
    int head, count;
} keypad;

static void touch_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    data->point = touch.point;
    data->state = touch.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void keypad_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    if (!keypad.count) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    data->key = keypad.key[keypad.head];
    data->state = keypad.state[keypad.head];
    keypad.head = (keypad.head + 1) % 8;
    keypad.count--;
    data->continue_reading = keypad.count > 0;
}

static void queue_key(uint32_t key, lv_indev_state_t state)
{
    if (keypad.count == 8)
        return;
    int i = (keypad.head + keypad.count++) % 8;
    keypad.key[i] = key;
    keypad.state[i] = state;
}

void input_focus_key(uint32_t key)
{
    queue_key(key, LV_INDEV_STATE_PRESSED);
    queue_key(key, LV_INDEV_STATE_RELEASED);
    while (keypad.count)
        lv_indev_read(keypad_indev);
}

/* lv_group_focus_obj(NULL) is a no-op, so dropping the focus needs the private
 * field. The DEFOCUSED event is what clears the focus outline and closes the
 * keyboard bound to the focused input field. */
void input_defocus(void)
{
    lv_group_t *group = input_group();
    if (!group || !group->obj_focus)
        return;
    lv_obj_t *obj = *group->obj_focus;
    group->obj_focus = NULL;
    lv_group_set_editing(group, false);
    lv_obj_send_event(obj, LV_EVENT_DEFOCUSED, keypad_indev);
}

void input_focus_move(lv_group_t *group, int dir)
{
    lv_obj_t *focused = lv_group_get_focused(group);
    if (!focused || lv_obj_has_state(focused, LV_STATE_FOCUS_KEY)) {
        if (dir > 0)
            lv_group_focus_next(group);
        else
            lv_group_focus_prev(group);
        focused = lv_group_get_focused(group);
    }
    /* LVGL resolves the group's indev to the touch device and never marks it. */
    if (focused)
        lv_obj_add_state(focused, LV_STATE_FOCUS_KEY);
}

static bool shown(lv_obj_t *obj)
{
    for (; obj; obj = lv_obj_get_parent(obj))
        if (lv_obj_is_hidden(obj))
            return false;
    return true;
}

lv_obj_t *input_key_focus(lv_group_t *group)
{
    lv_obj_t *focused = group ? lv_group_get_focused(group) : NULL;
    return focused && lv_obj_has_state(focused, LV_STATE_FOCUS_KEY) && shown(focused) ? focused : NULL;
}

void input_focus_toward(lv_group_t *group, int dx, int dy)
{
    lv_obj_t *from = input_key_focus(group);
    if (!from) {
        input_focus_move(group, dx + dy < 0 ? -1 : 1);
        return;
    }
    lv_area_t a;
    lv_obj_get_coords(from, &a);
    int32_t fx = (a.x1 + a.x2) / 2, fy = (a.y1 + a.y2) / 2;
    lv_obj_t *best = NULL;
    int64_t best_score = 0;
    for (lv_obj_t **o = lv_ll_get_head(&group->obj_ll); o; o = lv_ll_get_next(&group->obj_ll, o)) {
        if (*o == from || !shown(*o) || lv_obj_has_state(*o, LV_STATE_DISABLED))
            continue;
        lv_obj_get_coords(*o, &a);
        int32_t cx = (a.x1 + a.x2) / 2, cy = (a.y1 + a.y2) / 2;
        int32_t along = dx ? (cx - fx) * dx : (cy - fy) * dy;
        int32_t across = dx ? LV_ABS(cy - fy) : LV_ABS(cx - fx);
        int64_t score = (int64_t)along + 2 * (int64_t)across;
        if (along > 0 && (!best || score < best_score)) {
            best = *o;
            best_score = score;
        }
    }
    if (!best)
        return;
    lv_group_focus_obj(best);
    lv_obj_add_state(best, LV_STATE_FOCUS_KEY);
    lv_obj_scroll_to_view_recursive(best, LV_ANIM_ON);
}

void input_set_group(lv_group_t *group)
{
    lv_indev_set_group(keypad_indev, group);
}

lv_group_t *input_group(void)
{
    return lv_indev_get_group(keypad_indev);
}

void input_touch_take(void)
{
    lv_obj_t *pressed = touch_indev->pointer.act_obj;
    if (pressed) {
        lv_obj_remove_state(pressed, LV_STATE_PRESSED);
        lv_obj_send_event(pressed, LV_EVENT_PRESS_LOST, touch_indev);
    }
    lv_indev_wait_release(touch_indev);
}

/* The kernel drops a position equal to the last one it reported, so a touch where
 * the previous one ended (before this program started) brings no coordinates:
 * start from the positions the device already holds. */
static void seed_slots(int fd)
{
    struct {
        uint32_t code;
        int32_t values[SLOTS];
    } slots;
    slots.code = ABS_MT_POSITION_X;
    if (!ioctl(fd, EVIOCGMTSLOTS(sizeof slots), &slots))
        memcpy(touch.x, slots.values, sizeof touch.x);
    slots.code = ABS_MT_POSITION_Y;
    if (!ioctl(fd, EVIOCGMTSLOTS(sizeof slots), &slots))
        memcpy(touch.y, slots.values, sizeof touch.y);
}

static void open_devices(void)
{
    DIR *dir = opendir("/dev/input");
    if (!dir)
        return;
    struct dirent *entry;
    while ((entry = readdir(dir)) && device_count < INPUT_MAX_DEVICES) {
        if (strncmp(entry->d_name, "event", 5))
            continue;
        char path[64];
        snprintf(path, sizeof path, "/dev/input/%s", entry->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;
        unsigned long abs[BITS(ABS_CNT)] = {0}, keys[BITS(KEY_CNT)] = {0};
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs), abs);
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys);
        struct device *d = &devices[device_count];
        d->fd = fd;
        if (TEST(abs, ABS_MT_POSITION_X)) {
            d->kind = DEVICE_TOUCH;
            ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &d->x);
            ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &d->y);
            seed_slots(fd);
        } else if (TEST(abs, ABS_X) && TEST(keys, BTN_TOUCH)) {
            d->kind = DEVICE_TOUCH;
            ioctl(fd, EVIOCGABS(ABS_X), &d->x);
            ioctl(fd, EVIOCGABS(ABS_Y), &d->y);
            touch.x[0] = d->x.value;
            touch.y[0] = d->y.value;
        } else if (TEST(keys, KEY_POWER) || TEST(keys, KEY_1) || TEST(keys, KEY_VOLUMEUP) || TEST(keys, KEY_BACK)) {
            d->kind = DEVICE_KEYS;
            star_key |= TEST(keys, KEY_NUMERIC_STAR) || TEST(keys, KEY_KPASTERISK);
        } else {
            close(fd);
            continue;
        }
        if (d->kind == DEVICE_TOUCH) {
            /* Touch times on the clock the frames are timed by. */
            int clock = CLOCK_MONOTONIC;
            ioctl(fd, EVIOCSCLOCKID, &clock);
        }
        device_count++;
    }
    closedir(dir);
}

void input_init(lv_display_t *display)
{
    width = lv_display_get_horizontal_resolution(display);
    height = lv_display_get_vertical_resolution(display);
    open_devices();

    touch_indev = lv_indev_create();
    lv_indev_set_type(touch_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(touch_indev, touch_read);
    lv_indev_set_mode(touch_indev, LV_INDEV_MODE_EVENT);
    lv_indev_add_event_cb(touch_indev, touch_click, LV_EVENT_SHORT_CLICKED, NULL);
    lv_indev_add_event_cb(touch_indev, touch_click, LV_EVENT_CLICKED, NULL);

    keypad_indev = lv_indev_create();
    lv_indev_set_type(keypad_indev, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(keypad_indev, keypad_read);
    lv_indev_set_mode(keypad_indev, LV_INDEV_MODE_EVENT);
}

int input_poll_fds(struct pollfd *fds)
{
    for (int i = 0; i < device_count; i++)
        fds[i] = (struct pollfd){.fd = devices[i].fd, .events = POLLIN};
    return device_count;
}

double input_touch_time(void)
{
    return touch.time;
}

bool input_has_star(void)
{
    return star_key;
}

int input_touch_count(void)
{
    int n = 0;
    for (int i = 0; i < SLOTS; i++)
        n += touch.active[i];
    return n ? n : touch.legacy_down;
}

static int32_t scale(int value, const struct input_absinfo *abs, int32_t size)
{
    int span = abs->maximum - abs->minimum;
    if (span <= 0)
        return value;
    int32_t v = (int32_t)((int64_t)(value - abs->minimum) * (size - 1) / span);
    return v < 0 ? 0 : v >= size ? size - 1 : v;
}

static void touch_sync(const struct device *d)
{
    if (touch.primary < 0 || !touch.active[touch.primary]) {
        touch.primary = -1;
        for (int i = 0; i < SLOTS; i++)
            if (touch.active[i]) {
                touch.primary = i;
                break;
            }
    }
    bool pressed = touch.primary >= 0 || touch.legacy_down;
    int slot = touch.primary >= 0 ? touch.primary : 0;
    if (!shell_touch_allowed()) {
        pressed = false;
        touch.primary = -1;
    }
    if (pressed)
        touch.point = (lv_point_t){scale(touch.x[slot], &d->x, width), scale(touch.y[slot], &d->y, height)};
    bool was_pressed = touch.pressed;
    touch.pressed = pressed;
    if (pressed && !was_pressed) {
        press.cancelled = touch_indev && lv_indev_get_scroll_obj(touch_indev);
        press.point = touch.point;
        shell_touch_press();
    }
    if (pressed && !press.cancelled)
        press.cancelled = LV_ABS(touch.point.x - press.point.x) > TAP_SLOP ||
                          LV_ABS(touch.point.y - press.point.y) > TAP_SLOP;
    shell_swipe(pressed, touch.point, touch.time);
    lv_indev_read(touch_indev);
}

static void touch_event(const struct device *d, const struct input_event *ev)
{
    if (ev->type == EV_ABS) {
        switch (ev->code) {
        case ABS_MT_SLOT:
            touch.slot = ev->value >= 0 && ev->value < SLOTS ? ev->value : 0;
            break;
        case ABS_MT_TRACKING_ID:
            touch.active[touch.slot] = ev->value >= 0;
            break;
        case ABS_MT_POSITION_X:
        case ABS_X:
            touch.x[ev->code == ABS_X ? 0 : touch.slot] = ev->value;
            break;
        case ABS_MT_POSITION_Y:
        case ABS_Y:
            touch.y[ev->code == ABS_Y ? 0 : touch.slot] = ev->value;
            break;
        }
    } else if (ev->type == EV_KEY && ev->code == BTN_TOUCH) {
        touch.legacy_down = ev->value != 0;
    } else if (ev->type == EV_SYN && ev->code == SYN_REPORT) {
        touch.time = ev->input_event_sec * 1e3 + ev->input_event_usec / 1e3;
        touch_sync(d);
    }
}

static void key_event(const struct input_event *ev)
{
    if (ev->type != EV_KEY)
        return;
    struct key_event key = {.down = ev->value != 0, .repeat = ev->value == 2};
    switch (ev->code) {
    case KEY_POWER: key.kind = KEY_KIND_POWER; break;
    case KEY_VOLUMEUP: key.kind = KEY_KIND_VOLUP; break;
    case KEY_VOLUMEDOWN: key.kind = KEY_KIND_VOLDOWN; break;
    case KEY_BACK:
    case KEY_BACKSPACE:
    case KEY_ESC: key.kind = KEY_KIND_BACK; break;
    case KEY_MENU: key.kind = KEY_KIND_MENU; break;
    case KEY_PHONE: key.kind = KEY_KIND_CALL; break;
    case KEY_F1: key.kind = KEY_KIND_HOTSPOT; break;
    case KEY_ENTER:
    case KEY_KPENTER:
    case KEY_SELECT:
    case KEY_OK: key.kind = KEY_KIND_OK; break;
    case KEY_UP: key.kind = KEY_KIND_UP; break;
    case KEY_DOWN: key.kind = KEY_KIND_DOWN; break;
    case KEY_LEFT: key.kind = KEY_KIND_LEFT; break;
    case KEY_RIGHT: key.kind = KEY_KIND_RIGHT; break;
    case KEY_0: key.kind = KEY_KIND_CHAR; key.ch = '0'; break;
    case KEY_NUMERIC_STAR:
    case KEY_KPASTERISK: key.kind = KEY_KIND_CHAR; key.ch = '*'; break;
    case KEY_NUMERIC_POUND: key.kind = KEY_KIND_CHAR; key.ch = '#'; break;
    default:
        if (ev->code >= KEY_1 && ev->code <= KEY_9) {
            key.kind = KEY_KIND_CHAR;
            key.ch = (char)('1' + ev->code - KEY_1);
        } else if (ev->code >= KEY_NUMERIC_0 && ev->code <= KEY_NUMERIC_9) {
            key.kind = KEY_KIND_CHAR;
            key.ch = (char)('0' + ev->code - KEY_NUMERIC_0);
        } else {
            return;
        }
    }
    shell_key(&key);
}

void input_dispatch(const struct pollfd *fds, int count)
{
    for (int i = 0; i < count && i < device_count; i++) {
        if (!(fds[i].revents & POLLIN))
            continue;
        struct input_event events[64];
        ssize_t n;
        while ((n = read(devices[i].fd, events, sizeof events)) > 0)
            for (size_t e = 0; e < (size_t)n / sizeof *events; e++) {
                if (devices[i].kind == DEVICE_TOUCH)
                    touch_event(&devices[i], &events[e]);
                else
                    key_event(&events[e]);
            }
    }
}
