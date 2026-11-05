#include "input.h"

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "src/core/lv_group_private.h"

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

static struct {
    int slot;
    int primary;
    bool active[SLOTS];
    int x[SLOTS], y[SLOTS];
    bool legacy_down;
    lv_point_t point;
    bool pressed;
} touch = {.primary = -1};

static lv_indev_t *touch_indev, *keypad_indev;
static int32_t width, height;

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

void input_set_group(lv_group_t *group)
{
    lv_indev_set_group(keypad_indev, group);
}

lv_group_t *input_group(void)
{
    return lv_indev_get_group(keypad_indev);
}

lv_indev_t *input_touch(void)
{
    return touch_indev;
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
        } else if (TEST(abs, ABS_X) && TEST(keys, BTN_TOUCH)) {
            d->kind = DEVICE_TOUCH;
            ioctl(fd, EVIOCGABS(ABS_X), &d->x);
            ioctl(fd, EVIOCGABS(ABS_Y), &d->y);
        } else if (TEST(keys, KEY_POWER) || TEST(keys, KEY_1) || TEST(keys, KEY_VOLUMEUP) || TEST(keys, KEY_BACK)) {
            d->kind = DEVICE_KEYS;
        } else {
            close(fd);
            continue;
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
    if (pressed && !was_pressed)
        shell_touch_press();
    shell_swipe(pressed, touch.point);
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
