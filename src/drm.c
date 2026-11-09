#include "drm.h"

#include <drm.h>
#include <drm_mode.h>
#include <errno.h>
#include <stdbool.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "src/libs/lodepng/lodepng.h"

struct buffer {
    uint32_t handle, fb, pitch;
    uint64_t size;
    uint8_t *map;
};

static struct {
    int fd;
    uint32_t crtc, connector;
    struct drm_mode_modeinfo mode;
    struct buffer buffers[3];
    uint8_t *render[2];
    const uint8_t *shown;
    uint32_t shown_pitch;
    int front;
    int ready;
    const uint8_t *ready_pixels;
    bool pending;
    bool slow;
    uint32_t draw_at;
    uint32_t deadline;
    uint64_t serial;
    double period_ms;
    uint32_t frames;
} drm = {.fd = -1, .ready = -1};

static void present_ready(void);

static int io(unsigned long request, void *arg)
{
    int r;
    do
        r = ioctl(drm.fd, request, arg);
    while (r < 0 && errno == EINTR);
    return r;
}

static bool find_output(void)
{
    struct drm_mode_card_res res = {0};
    if (io(DRM_IOCTL_MODE_GETRESOURCES, &res) < 0 || !res.count_connectors || !res.count_crtcs)
        return false;
    uint32_t connectors[res.count_connectors], crtcs[res.count_crtcs], encoders[res.count_encoders ? res.count_encoders : 1];
    struct drm_mode_card_res list = {
        .connector_id_ptr = (uintptr_t)connectors, .count_connectors = res.count_connectors,
        .crtc_id_ptr = (uintptr_t)crtcs, .count_crtcs = res.count_crtcs,
        .encoder_id_ptr = (uintptr_t)encoders, .count_encoders = res.count_encoders,
    };
    if (io(DRM_IOCTL_MODE_GETRESOURCES, &list) < 0)
        return false;
    for (uint32_t i = 0; i < list.count_connectors; i++) {
        struct drm_mode_get_connector c = {.connector_id = connectors[i]};
        if (io(DRM_IOCTL_MODE_GETCONNECTOR, &c) < 0 || c.connection != 1 || !c.count_modes)
            continue;
        struct drm_mode_modeinfo modes[c.count_modes];
        uint32_t encs[c.count_encoders ? c.count_encoders : 1];
        struct drm_mode_get_connector full = {
            .connector_id = connectors[i], .modes_ptr = (uintptr_t)modes, .count_modes = c.count_modes,
            .encoders_ptr = (uintptr_t)encs, .count_encoders = c.count_encoders,
        };
        if (io(DRM_IOCTL_MODE_GETCONNECTOR, &full) < 0)
            continue;
        drm.mode = modes[0];
        for (uint32_t m = 0; m < full.count_modes; m++)
            if (modes[m].type & DRM_MODE_TYPE_PREFERRED)
                drm.mode = modes[m];
        drm.connector = connectors[i];
        for (uint32_t e = 0; e < full.count_encoders; e++) {
            struct drm_mode_get_encoder enc = {.encoder_id = encs[e]};
            if (io(DRM_IOCTL_MODE_GETENCODER, &enc) < 0)
                continue;
            if (enc.crtc_id) {
                drm.crtc = enc.crtc_id;
                return true;
            }
            for (uint32_t k = 0; k < list.count_crtcs; k++)
                if (enc.possible_crtcs & (1u << k)) {
                    drm.crtc = crtcs[k];
                    return true;
                }
        }
    }
    return false;
}

static bool create_buffer(struct buffer *b)
{
    struct drm_mode_create_dumb create = {.width = drm.mode.hdisplay, .height = drm.mode.vdisplay, .bpp = 32};
    if (io(DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0)
        return false;
    b->handle = create.handle;
    b->pitch = create.pitch;
    b->size = create.size;
    struct drm_mode_fb_cmd fb = {
        .width = create.width, .height = create.height, .pitch = create.pitch,
        .bpp = 32, .depth = 24, .handle = create.handle,
    };
    if (io(DRM_IOCTL_MODE_ADDFB, &fb) < 0)
        return false;
    b->fb = fb.fb_id;
    struct drm_mode_map_dumb map = {.handle = create.handle};
    if (io(DRM_IOCTL_MODE_MAP_DUMB, &map) < 0)
        return false;
    b->map = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, drm.fd, (off_t)map.offset);
    if (b->map == MAP_FAILED)
        return false;
    memset(b->map, 0, b->size);
    return true;
}

int drm_display_fd(void)
{
    return drm.fd;
}

bool drm_display_can_draw(void)
{
    if (drm.pending && (int32_t)(drm.deadline - lv_tick_get()) <= 0) {
        drm_display_dispatch();
        if (drm.pending && (int32_t)(drm.deadline - lv_tick_get()) <= 0) {
            fputs("DRM page flip completion timed out\n", stderr);
            drm.pending = false;
            present_ready();
        }
    }
    /* Rendering that exceeds a panel period can overlap the outstanding flip. */
    return !drm.pending || (drm.slow && drm.ready < 0);
}

int drm_display_wait(void)
{
    if (!drm.pending)
        return -1;
    int32_t left = (int32_t)(drm.deadline - lv_tick_get());
    return LV_MAX(0, left);
}

void drm_display_dispatch(void)
{
    uint8_t events[256];
    ssize_t n;
    for (;;) {
        n = read(drm.fd, events, sizeof events);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        for (size_t at = 0; at + sizeof(struct drm_event) <= (size_t)n;) {
            struct drm_event *event = (void *)(events + at);
            if (event->length < sizeof *event || event->length > (size_t)n - at)
                break;
            if (event->type == DRM_EVENT_FLIP_COMPLETE && event->length >= sizeof(struct drm_event_vblank)) {
                struct drm_event_vblank *flip = (void *)event;
                if (flip->user_data == drm.serial)
                    drm.pending = false;
            }
            at += event->length;
        }
    }
    present_ready();
}

double drm_display_period(void)
{
    return drm.period_ms;
}

static double mode_period(const struct drm_mode_modeinfo *mode)
{
    if (!mode->clock || !mode->htotal || !mode->vtotal)
        return 1000.0 / (mode->vrefresh ? mode->vrefresh : 60);
    double period = (double)mode->htotal * mode->vtotal / mode->clock;
    if (mode->flags & DRM_MODE_FLAG_INTERLACE)
        period /= 2;
    if (mode->flags & DRM_MODE_FLAG_DBLSCAN)
        period *= 2;
    if (mode->vscan > 1)
        period *= mode->vscan;
    return period;
}

static void frame_start(lv_event_t *e)
{
    drm.draw_at = lv_tick_get();
}

static void present_ready(void)
{
    if (drm.ready < 0 || drm.pending)
        return;
    int back = drm.ready;
    struct drm_mode_crtc_page_flip flip = {
        .crtc_id = drm.crtc, .fb_id = drm.buffers[back].fb, .flags = DRM_MODE_PAGE_FLIP_EVENT,
        .user_data = drm.serial + 1,
    };
    if (io(DRM_IOCTL_MODE_PAGE_FLIP, &flip) == 0) {
        drm.shown = drm.ready_pixels;
        drm.shown_pitch = drm.buffers[back].pitch;
        drm.front = back;
        drm.pending = true;
        drm.deadline = lv_tick_get() + 100;
        drm.serial++;
        drm.frames++;
    }
    drm.ready = -1;
}

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    if (lv_display_flush_is_last(display)) {
        if (drm.pending)
            drm_display_dispatch();
        /* Of the three buffers, this one is neither on screen nor on its way there. */
        int back = (drm.front + 1) % 3;
        memcpy(drm.buffers[back].map, pixels, (size_t)drm.buffers[back].pitch * drm.mode.vdisplay);
        drm.ready = back;
        drm.ready_pixels = pixels;
        drm.slow = lv_tick_elaps(drm.draw_at) >= drm.period_ms;
        present_ready();
    }
    lv_display_flush_ready(display);
}

lv_display_t *drm_display_create(const char *path)
{
    drm.fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (drm.fd < 0 || io(DRM_IOCTL_SET_MASTER, NULL) < 0 || !find_output() || !create_buffer(&drm.buffers[0]) ||
        !create_buffer(&drm.buffers[1]) || !create_buffer(&drm.buffers[2])) {
        perror(path);
        return NULL;
    }
    uint32_t connector = drm.connector;
    struct drm_mode_crtc crtc = {
        .crtc_id = drm.crtc, .fb_id = drm.buffers[0].fb, .set_connectors_ptr = (uintptr_t)&connector,
        .count_connectors = 1, .mode = drm.mode, .mode_valid = 1,
    };
    if (io(DRM_IOCTL_MODE_SETCRTC, &crtc) < 0) {
        perror("SETCRTC");
        return NULL;
    }
    /* Frames are drawn in ordinary memory: the display's buffers are write-combined,
     * and every pixel of a translucent card would cost an uncached read there. */
    size_t bytes = (size_t)drm.buffers[0].pitch * drm.mode.vdisplay;
    for (int i = 0; i < 2; i++) {
        drm.render[i] = malloc(bytes);
        if (!drm.render[i]) {
            perror("malloc");
            return NULL;
        }
        memset(drm.render[i], 0, bytes);
    }
    lv_display_t *display = lv_display_create(drm.mode.hdisplay, drm.mode.vdisplay);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_buffers_with_stride(display, drm.render[1], drm.render[0], (uint32_t)bytes, drm.buffers[0].pitch,
                                       LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, flush);
    drm.period_ms = mode_period(&drm.mode);
    lv_display_add_event_cb(display, frame_start, LV_EVENT_REFR_START, NULL);
    return display;
}

const uint8_t *drm_display_front(uint32_t *pitch)
{
    *pitch = drm.shown_pitch;
    return drm.shown;
}

uint32_t drm_display_frames(void)
{
    return drm.frames;
}

void drm_display_size(uint32_t *width, uint32_t *height)
{
    *width = drm.mode.hdisplay;
    *height = drm.mode.vdisplay;
}

bool drm_display_save(const char *path)
{
    const struct buffer *b = &drm.buffers[drm.front];
    uint32_t w = drm.mode.hdisplay, h = drm.mode.vdisplay;
    uint8_t *rgba = malloc((size_t)w * h * 4);
    if (!rgba)
        return false;
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *src = b->map + (size_t)y * b->pitch;
        uint8_t *dst = rgba + (size_t)y * w * 4;
        for (uint32_t x = 0; x < w; x++, src += 4, dst += 4) {
            dst[0] = src[2];
            dst[1] = src[1];
            dst[2] = src[0];
            dst[3] = 255;
        }
    }
    uint8_t *png = NULL;
    size_t size = 0;
    bool ok = !lodepng_encode32(&png, &size, rgba, w, h);
    free(rgba);
    if (ok) {
        FILE *f = fopen(path, "wb");
        ok = f && fwrite(png, 1, size, f) == size;
        if (f && fclose(f))
            ok = false;
    }
    free(png);
    return ok;
}
