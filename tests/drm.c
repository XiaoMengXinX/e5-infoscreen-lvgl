#include <assert.h>
#include <stdarg.h>
#include <math.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "../src/drm.h"

ssize_t mock_read(int fd, void *data, size_t size);
int mock_ioctl(int fd, int request, ...);
uint32_t mock_tick(void);
uint32_t mock_elaps(uint32_t previous);
bool mock_is_last(lv_display_t *display);
void mock_ready(lv_display_t *display);

#define read mock_read
#define ioctl mock_ioctl
#define lv_tick_get mock_tick
#define lv_tick_elaps mock_elaps
#define lv_display_flush_is_last mock_is_last
#define lv_display_flush_ready mock_ready
#include "../src/drm.c"
#undef read
#undef ioctl
#undef lv_tick_get
#undef lv_tick_elaps
#undef lv_display_flush_is_last
#undef lv_display_flush_ready

static uint32_t now;
static int ready, ioctl_error, interrupt_read, reads;
static struct drm_mode_crtc_page_flip submitted;
static struct drm_event_vblank event;
static bool readable;

uint32_t mock_tick(void) { return now; }
uint32_t mock_elaps(uint32_t previous) { return now - previous; }
static bool last = true;
bool mock_is_last(lv_display_t *display) { return last; }
void mock_ready(lv_display_t *display) { ready++; }

int mock_ioctl(int fd, int request, ...)
{
    assert(fd == 7 && request == (int)DRM_IOCTL_MODE_PAGE_FLIP);
    va_list args;
    va_start(args, request);
    submitted = *va_arg(args, struct drm_mode_crtc_page_flip *);
    va_end(args);
    if (ioctl_error) {
        errno = ioctl_error;
        return -1;
    }
    return 0;
}

ssize_t mock_read(int fd, void *data, size_t size)
{
    reads++;
    assert(fd == 7 && size >= sizeof event);
    if (interrupt_read) {
        interrupt_read = 0;
        errno = EINTR;
        return -1;
    }
    if (!readable) {
        errno = EAGAIN;
        return -1;
    }
    memcpy(data, &event, sizeof event);
    readable = false;
    return sizeof event;
}

static void complete(uint64_t serial)
{
    event = (struct drm_event_vblank){
        .base = {.type = DRM_EVENT_FLIP_COMPLETE, .length = sizeof event}, .user_data = serial,
    };
    readable = true;
    drm_display_dispatch();
}

static const lv_area_t full = {0, 0, 3, 0};

static void draw_frame(uint8_t *pixels)
{
    drm.draw_at = now;
    flush(NULL, &full, pixels);
}

int main(void)
{
    uint8_t maps[3][16] = {{0}}, pixels[16];
    memset(pixels, 0x5a, sizeof pixels);
    drm.fd = 7;
    drm.period_ms = 1000.0 / 60;
    drm.mode.vdisplay = 1;
    for (int i = 0; i < 3; i++)
        drm.buffers[i] = (struct buffer){.map = maps[i], .pitch = 16, .fb = 100 + i};

    assert(drm_display_fd() == 7 && drm_display_wait() == -1 && drm_display_can_draw());
    draw_frame(pixels);
    assert(reads == 0);
    assert(ready == 1 && drm.front == 1 && drm.frames == 1 && drm.pending);
    assert(submitted.fb_id == 101 && submitted.user_data == 1);
    assert(!memcmp(maps[1], pixels, sizeof pixels) && drm_display_wait() == 100);
    assert(!drm_display_can_draw());
    complete(0);
    assert(drm.pending);
    interrupt_read = 1;
    complete(1);
    assert(!drm.pending && drm_display_wait() == -1);

    now = UINT32_MAX - 20;
    draw_frame(pixels);
    assert(drm.front == 2 && submitted.user_data == 2);
    now += 30;
    assert(drm_display_wait() == 70);
    now += 70;
    assert(drm_display_can_draw() && drm_display_wait() == -1 && !drm.pending);
    draw_frame(pixels);
    assert(drm.front == 0 && submitted.user_data == 3);
    complete(2);
    assert(drm.pending);
    complete(3);
    assert(!drm.pending);

    ioctl_error = EBUSY;
    draw_frame(pixels);
    assert(drm.front == 0 && drm.frames == 3 && !drm.pending && ready == 4);
    ioctl_error = EAGAIN;
    draw_frame(pixels);
    assert(drm.front == 0 && drm.frames == 3 && !drm.pending && ready == 5);
    ioctl_error = 0;
    draw_frame(pixels);
    event = (struct drm_event_vblank){.base = {.length = 0}};
    readable = true;
    drm_display_dispatch();
    assert(drm.pending);
    now = drm.deadline;
    event = (struct drm_event_vblank){
        .base = {.type = DRM_EVENT_FLIP_COMPLETE, .length = sizeof event}, .user_data = 4,
    };
    readable = true;
    assert(drm_display_can_draw() && drm_display_wait() == -1 && !drm.pending);

    drm.draw_at = now;
    now += 25;
    flush(NULL, &full, pixels);
    assert(drm.slow && drm.frames == 5 && drm_display_can_draw() && drm_display_wait() == 100);
    int queued = drm.front;
    drm.draw_at = now;
    now += 25;
    pixels[0] = 0x11;
    flush(NULL, &full, pixels);
    assert(drm.front == queued && drm.ready != queued && drm.ready >= 0 && drm.frames == 5);
    assert(!drm_display_can_draw() && drm_display_wait() > 0);
    complete(5);
    assert(drm.pending && drm.ready == -1 && drm.frames == 6 && drm_display_can_draw());
    complete(6);
    draw_frame(pixels);
    assert(!drm.slow && drm_display_wait() > 0);
    complete(7);

    /* Partial frames: a buffer catches up on what the frames before it changed. */
    uint8_t frame[16];
    memcpy(frame, pixels, sizeof frame);
    const lv_area_t px0 = {0, 0, 0, 0}, px1 = {1, 0, 1, 0}, px2 = {2, 0, 2, 0}, px3 = {3, 0, 3, 0};
    memset(frame, 0xa0, 4);
    flush(NULL, &px0, frame);
    int w0 = drm.front;
    complete(8);
    memset(frame + 4, 0xb0, 4);
    flush(NULL, &px1, frame);
    int w1 = drm.front;
    complete(9);
    /* Two areas in one frame: only the last presents. */
    memset(frame + 8, 0xc0, 4);
    last = false;
    flush(NULL, &px2, frame);
    assert(drm.front == w1 && drm.ready < 0 && drm.writing >= 0);
    last = true;
    memset(frame + 12, 0xd0, 4);
    flush(NULL, &px3, frame);
    int w2 = drm.front;
    assert(w2 != w1 && w2 != w0 && drm.writing < 0 && drm.pending);
    assert(!memcmp(maps[w2], frame, 16));
    assert(!memcmp(maps[w1], frame, 8) && !memcmp(maps[w1] + 8, pixels + 8, 8));
    assert(!memcmp(maps[w0], frame, 4) && !memcmp(maps[w0] + 4, pixels + 4, 12));
    complete(10);
    memset(frame, 0xe0, 4);
    flush(NULL, &px0, frame);
    assert(drm.front == w0 && !memcmp(maps[w0], frame, 16));
    complete(11);

    struct drm_mode_modeinfo mode = {.clock = 9000, .htotal = 300, .vtotal = 500};
    assert(fabs(mode_period(&mode) - 1000.0 / 60) < .000001);
    mode.clock = 13500;
    assert(fabs(mode_period(&mode) - 1000.0 / 90) < .000001);
    mode.flags = DRM_MODE_FLAG_INTERLACE;
    assert(fabs(mode_period(&mode) - 1000.0 / 180) < .000001);
    mode.flags = DRM_MODE_FLAG_DBLSCAN;
    mode.vscan = 2;
    assert(fabs(mode_period(&mode) - 4000.0 / 90) < .000001);
    mode.clock = 0;
    mode.vrefresh = 50;
    assert(mode_period(&mode) == 20);
    puts("DRM: completion ordering, stale events, timeout rollover, buffer ownership, partial frames and mode timing "
         "passed");
}
