#include "wallpaper.h"

#include <stdbool.h>

#include "src/image/lv_image_decoder_private.h"
#include "src/misc/cache/instance/lv_image_cache.h"

#include "theme.h"

static const char *const paths[] = {"A:/etc/e5-infoscreen/wallpapers/desktop.png",
                                    "A:/etc/e5-infoscreen/wallpapers/lock.png"};

/* The wash by height: strongest under the status bar and title, and on the lock
 * screen under its clock and its hint at the foot. Opacity out of 255. */
static const struct {
    float at;
    int opa;
} washes[][4] = {
    {{0, 160}, {0.14f, 160}, {0.30f, 100}, {1, 100}},
    {{0, 150}, {0.45f, 100}, {0.78f, 100}, {1, 150}},
};

static lv_draw_buf_t *bufs[2];
static bool tried[2];

static int wash(enum wallpaper which, float at)
{
    int i = 1;
    while (i < 3 && at > washes[which][i].at)
        i++;
    float a = washes[which][i - 1].at, b = washes[which][i].at;
    float t = (at - a) / (b - a);
    return (int)(washes[which][i - 1].opa + (washes[which][i].opa - washes[which][i - 1].opa) * t);
}

static lv_draw_buf_t *load(enum wallpaper which)
{
    lv_image_decoder_dsc_t dsc;
    if (lv_image_decoder_open(&dsc, paths[which], NULL) != LV_RESULT_OK)
        return NULL;
    const lv_draw_buf_t *src = dsc.decoded;
    lv_display_t *display = lv_display_get_default();
    int32_t w = lv_display_get_horizontal_resolution(display), h = lv_display_get_vertical_resolution(display);
    lv_draw_buf_t *out = NULL;
    if (src && (src->header.cf == LV_COLOR_FORMAT_ARGB8888 || src->header.cf == LV_COLOR_FORMAT_XRGB8888))
        out = lv_draw_buf_create(w, h, LV_COLOR_FORMAT_XRGB8888, LV_STRIDE_AUTO);
    if (out) {
        /* Covering the screen, centred. */
        int32_t sw = src->header.w, sh = src->header.h;
        float scale = (float)sw / w < (float)sh / h ? (float)sw / w : (float)sh / h;
        float x0 = (sw - w * scale) / 2, y0 = (sh - h * scale) / 2;
        bool alpha = src->header.cf == LV_COLOR_FORMAT_ARGB8888;
        uint8_t bg[3] = {theme.bg.blue, theme.bg.green, theme.bg.red};
        for (int32_t y = 0; y < h; y++) {
            const uint8_t *row = src->data + src->header.stride * (int32_t)(y0 + y * scale);
            uint8_t *px = out->data + out->header.stride * y;
            int k = wash(which, (float)y / h);
            for (int32_t x = 0; x < w; x++, px += 4) {
                const uint8_t *s = row + 4 * (int32_t)(x0 + x * scale);
                int a = alpha ? s[3] : 255;
                for (int c = 0; c < 3; c++) {
                    int v = (s[c] * a + bg[c] * (255 - a)) / 255;
                    px[c] = (uint8_t)((v * (255 - k) + bg[c] * k) / 255);
                }
                px[3] = 0xff;
            }
        }
    }
    lv_image_decoder_close(&dsc);
    lv_image_cache_drop(paths[which]);
    return out;
}

const lv_draw_buf_t *wallpaper_get(enum wallpaper which)
{
    if (!(which == WALLPAPER_DESKTOP ? theme.desktop_wallpaper : theme.lock_wallpaper))
        return NULL;
    if (!tried[which]) {
        tried[which] = true;
        bufs[which] = load(which);
    }
    return bufs[which];
}

void wallpaper_background(lv_obj_t *obj)
{
    const lv_draw_buf_t *pixels = wallpaper_get(WALLPAPER_DESKTOP);
    if (!pixels)
        return;
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_update_layout(obj);
    lv_area_t content;
    lv_obj_get_content_coords(obj, &content);
    lv_obj_t *image = lv_image_create(obj);
    lv_image_set_src(image, pixels);
    lv_obj_set_floating(image, true);
    /* Keep the wallpaper aligned to the panel across views with different padding. */
    lv_obj_set_pos(image, -content.x1, -content.y1);
    lv_obj_move_background(image);
}

void wallpaper_reset(void)
{
    for (int i = 0; i < 2; i++) {
        if (bufs[i])
            lv_draw_buf_destroy(bufs[i]);
        bufs[i] = NULL;
        tried[i] = false;
    }
}
