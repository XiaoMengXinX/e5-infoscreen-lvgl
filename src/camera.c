#include "camera.h"
#include "camera_capture.h"
#include "camera_driver.h"
#include "camera_driver_ui.h"
#include "theme.h"
#include "torch.h"
#include "ui.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static struct {
    lv_obj_t *root, *image, *status, *panel, *hint, *scan_frame;
    lv_obj_t *scan_shade[4];
    lv_timer_t *timer;
    enum { CLOSED, INSTALLING, CAPTURING } phase;
    unsigned photos, zoom;
    bool scan_mode, torch_checked, torch_available;
    char last_qr[256];
    uint32_t saved_at;
    void (*on_code)(const char *activation);
    uint32_t pixels[CAMERA_PREVIEW_WIDTH * CAMERA_PREVIEW_HEIGHT];
    lv_image_dsc_t frame;
} camera;
static void set_zoom(unsigned zoom);

static void refresh(lv_timer_t *timer)
{
    (void)timer;
    if (camera.phase == INSTALLING) return;
    struct camera_status status;
    bool fresh = camera_capture_update(camera.phase == CAPTURING ? camera.pixels : NULL, &status);
    if (camera.phase == CLOSED) {
        if (!camera_capture_busy()) {
            lv_timer_delete(camera.timer);
            camera.timer = NULL;
        }
        return;
    }
    if (fresh) {
        lv_image_cache_drop(&camera.frame);
        lv_image_set_src(camera.image, &camera.frame);
        lv_obj_invalidate(camera.image);
    }
    if (camera.on_code && *status.activation) {
        void (*callback)(const char *) = camera.on_code;
        camera_close();
        callback(status.activation);
        return;
    }
    if (status.photos != camera.photos) camera.saved_at = lv_tick_get();
    camera.photos = status.photos;
    bool show_saved = status.saved && lv_tick_elaps(camera.saved_at) < 2000;
    bool other_qr = camera.on_code && status.other_qr;
    bool debug_qr = camera.scan_mode && status.qr[0];
    if (debug_qr && strcmp(status.qr, camera.last_qr)) {
        snprintf(camera.last_qr, sizeof camera.last_qr, "%s", status.qr);
        if (!dialog_is_open()) {
            dialog_open(L("扫码结果", "QR scan result"), NULL);
            lv_obj_t *payload = ui_note(dialog_sheet(), status.qr);
            lv_obj_set_width(payload, LV_PCT(100));
            lv_label_set_long_mode(payload, LV_LABEL_LONG_MODE_WRAP);
            dialog_cancel();
        }
    } else if (!debug_qr && !dialog_is_open()) {
        camera.last_qr[0] = 0;
    }
    lv_obj_set_hidden(camera.status, status.ready && !status.saving && !show_saved && !status.failed && !other_qr);
    if (status.failed)
        ui_set_fmt(camera.status, "%s\n%s", L("相机不可用", "Camera unavailable"), status.error);
    else if (status.saving)
        ui_set_text(camera.status, L("正在保存…", "Saving…"));
    else if (show_saved)
        ui_set_text(camera.status, L("已保存到 /tmp/camera.jpg", "Saved to /tmp/camera.jpg"));
    else if (other_qr)
        ui_set_text(camera.status, L("请对准eSIM激活码二维码", "Point at an eSIM activation QR code"));
    else
        ui_set_text(camera.status, L("正在打开相机…", "Opening camera…"));
    if (status.ready && !status.failed) {
        idle_reset();
        if (!camera.torch_checked) {
            camera.torch_checked = true;
            camera.torch_available = torch_available();
            set_zoom(camera.zoom);
        }
    }
}

static void fit_preview(void)
{
    int32_t width = lv_obj_get_content_width(camera.panel);
    int32_t height = lv_obj_get_content_height(camera.panel);
    uint32_t fit = LV_MIN(width * 256 / CAMERA_PREVIEW_WIDTH,
                          height * 256 / CAMERA_PREVIEW_HEIGHT);
    lv_image_set_scale(camera.image, fit);
    lv_obj_invalidate(camera.image);
}

static void update_scan_region(void)
{
    int32_t panel_width = lv_obj_get_content_width(camera.panel);
    int32_t panel_height = lv_obj_get_content_height(camera.panel);
    uint32_t fit = LV_MIN(panel_width * 256 / CAMERA_PREVIEW_WIDTH,
                          panel_height * 256 / CAMERA_PREVIEW_HEIGHT);
    int32_t image_width = CAMERA_PREVIEW_WIDTH * fit / 256;
    int32_t image_height = CAMERA_PREVIEW_HEIGHT * fit / 256;
    int32_t frame_side = LV_MIN(panel_width, panel_height) * 2 / 3;
    unsigned sensor_x_fraction = (unsigned)frame_side * 10000 / image_height;
    unsigned sensor_y_fraction = (unsigned)frame_side * 10000 / image_width;
    camera_capture_set_zoom(camera.zoom, sensor_x_fraction, sensor_y_fraction);
}

static void set_zoom(unsigned zoom)
{
    camera.zoom = zoom;
    update_scan_region();
    const char *light_hint = "";
    if (camera.torch_available)
        light_hint = torch_enabled() ? L("\n*键关闭补光", "\n* to turn light off")
                                    : L("\n*键开启补光", "\n* to turn light on");
    ui_set_fmt(camera.hint, "%s  %u.%02u×%s",
               camera.on_code ? L("上下键缩放，对准二维码，返回键取消", "Up/down to zoom; align QR code; Back cancels")
               : camera.scan_mode ? L("扫码模式  左右键切换，返回键退出", "Scan mode  Left/right to switch; Back exits")
                                  : L("拍照模式  左右键切换，按OK键拍照", "Photo mode  Left/right to switch; OK to take a photo"),
               zoom / 100, zoom % 100, light_hint);
}

void camera_close(void)
{
    if (camera.phase == CLOSED) return;
    if (torch_enabled() && !torch_set(false)) perror("turn off camera light");
    if (camera.phase == INSTALLING) {
        camera_driver_ui_cancel();
    }
    camera.phase = CLOSED;
    camera.on_code = NULL;
    camera_capture_stop();
    lv_obj_set_hidden(camera.panel, true);
}

static void deleted(lv_event_t *event)
{
    (void)event;
    camera_close();
    lv_image_cache_drop(&camera.frame);
    if (camera.panel) lv_obj_delete(camera.panel);
    camera.panel = NULL;
    camera.root = NULL;
}

static void layout_scan_frame(void)
{
    int32_t width = lv_obj_get_content_width(camera.panel);
    int32_t height = lv_obj_get_content_height(camera.panel);
    int32_t side = LV_MIN(width, height) * 2 / 3;
    int32_t left = (width - side) / 2;
    int32_t top = (height - side) / 2;
    lv_obj_set_size(camera.scan_frame, side, side);
    lv_obj_set_pos(camera.scan_frame, left, top);
    lv_obj_set_pos(camera.scan_shade[0], 0, 0);
    lv_obj_set_size(camera.scan_shade[0], width, top);
    lv_obj_set_pos(camera.scan_shade[1], 0, top + side);
    lv_obj_set_size(camera.scan_shade[1], width, height - top - side);
    lv_obj_set_pos(camera.scan_shade[2], 0, top);
    lv_obj_set_size(camera.scan_shade[2], left, side);
    lv_obj_set_pos(camera.scan_shade[3], left + side, top);
    lv_obj_set_size(camera.scan_shade[3], width - left - side, side);
}

static void resize_scan_frame(lv_event_t *event)
{
    (void)event;
    fit_preview();
    layout_scan_frame();
    update_scan_region();
}

static void shade_scan_corners(lv_event_t *event)
{
    lv_obj_t *frame = lv_event_get_target_obj(event);
    lv_area_t bounds;
    lv_obj_get_coords(frame, &bounds);
    int32_t radius = lv_obj_get_style_radius(frame, LV_PART_MAIN);
    lv_draw_rect_dsc_t shade;
    lv_draw_rect_dsc_init(&shade);
    shade.bg_color = lv_color_black();
    shade.bg_opa = LV_OPA_60;
    /* Fill only outside the rounded opening; the border draws over its edge. */
    for (int32_t y = 0; y < radius; y++) {
        float dy = radius - y - 0.5f;
        int32_t inset = (int32_t)ceilf(radius - sqrtf(radius * radius - dy * dy));
        lv_area_t corners[] = {
            {bounds.x1, bounds.y1 + y, bounds.x1 + inset - 1, bounds.y1 + y},
            {bounds.x2 - inset + 1, bounds.y1 + y, bounds.x2, bounds.y1 + y},
            {bounds.x1, bounds.y2 - y, bounds.x1 + inset - 1, bounds.y2 - y},
            {bounds.x2 - inset + 1, bounds.y2 - y, bounds.x2, bounds.y2 - y},
        };
        for (int i = 0; i < 4; i++) lv_draw_rect(lv_event_get_layer(event), &shade, &corners[i]);
    }
}

void camera_create(lv_obj_t *parent)
{
    camera.root = parent;
    lv_obj_add_event_cb(parent, deleted, LV_EVENT_DELETE, NULL);
    camera.panel = lv_obj_create(shell_content());
    lv_obj_remove_style_all(camera.panel);
    lv_obj_set_floating(camera.panel, true);
    lv_obj_set_size(camera.panel, LV_PCT(100), LV_PCT(100));
    lv_obj_set_scrollable(camera.panel, false);
    lv_obj_set_style_bg_color(camera.panel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(camera.panel, LV_OPA_COVER, 0);
    lv_obj_set_hidden(camera.panel, true);
    camera.frame = (lv_image_dsc_t){
        .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_XRGB8888,
                   .w = CAMERA_PREVIEW_WIDTH, .h = CAMERA_PREVIEW_HEIGHT, .stride = CAMERA_PREVIEW_WIDTH * 4},
        .data_size = sizeof camera.pixels, .data = (const uint8_t *)camera.pixels,
    };
    camera.image = lv_image_create(camera.panel);
    lv_obj_set_size(camera.image, LV_PCT(100), LV_PCT(100));
    lv_image_set_inner_align(camera.image, LV_IMAGE_ALIGN_CENTER);
    lv_image_set_pivot(camera.image, CAMERA_PREVIEW_WIDTH / 2, CAMERA_PREVIEW_HEIGHT / 2);
    lv_image_set_src(camera.image, &camera.frame);
    for (int i = 0; i < 4; i++) {
        camera.scan_shade[i] = lv_obj_create(camera.panel);
        lv_obj_remove_style_all(camera.scan_shade[i]);
        lv_obj_set_clickable(camera.scan_shade[i], false);
        lv_obj_set_scrollable(camera.scan_shade[i], false);
        lv_obj_set_style_bg_color(camera.scan_shade[i], lv_color_black(), 0);
        lv_obj_set_style_bg_opa(camera.scan_shade[i], LV_OPA_60, 0);
        lv_obj_set_hidden(camera.scan_shade[i], true);
    }
    camera.scan_frame = lv_obj_create(camera.panel);
    lv_obj_remove_style_all(camera.scan_frame);
    lv_obj_set_clickable(camera.scan_frame, false);
    lv_obj_set_scrollable(camera.scan_frame, false);
    lv_obj_set_style_border_color(camera.scan_frame, theme.accent, 0);
    lv_obj_set_style_border_width(camera.scan_frame, 2, 0);
    lv_obj_set_style_radius(camera.scan_frame, 8, 0);
    lv_obj_add_event_cb(camera.scan_frame, shade_scan_corners, LV_EVENT_DRAW_MAIN_BEGIN, NULL);
    lv_obj_set_hidden(camera.scan_frame, true);
    lv_obj_add_event_cb(camera.panel, resize_scan_frame, LV_EVENT_SIZE_CHANGED, NULL);
    camera.status = ui_note(camera.panel, L("正在打开相机…", "Opening camera…"));
    lv_obj_set_style_text_color(camera.status, lv_color_white(), 0);
    lv_obj_set_style_text_align(camera.status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(camera.status, LV_PCT(100));
    lv_obj_set_style_bg_color(camera.status, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(camera.status, LV_OPA_60, 0);
    lv_obj_set_style_pad_all(camera.status, 4, 0);
    lv_obj_align(camera.status, LV_ALIGN_TOP_MID, 0, 0);
    camera.hint = ui_text(camera.panel, 10, false, lv_color_white());
    lv_obj_t *hint = camera.hint;
    ui_set_text(hint, L("按OK键拍照", "Press OK to take a photo"));
    lv_obj_set_width(hint, LV_PCT(100));
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(hint, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(hint, LV_OPA_60, 0);
    lv_obj_set_style_pad_all(hint, 4, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, 0);
}

bool camera_supported(void)
{
    return camera_driver_supported();
}

static void installation_done(bool success)
{
    if (success) {
        camera.phase = CAPTURING;
        camera_capture_start(camera.scan_mode);
    } else {
        bool scanning = camera.on_code != NULL;
        camera.phase = CLOSED;
        camera.on_code = NULL;
        lv_obj_set_hidden(camera.panel, true);
        if (!scanning) apps_page.back();
    }
}

static void open_camera(void (*on_code)(const char *activation))
{
    if (camera.phase != CLOSED || !camera_supported()) return;
    camera.phase = CAPTURING;
    camera.on_code = on_code;
    camera.photos = 0;
    camera.last_qr[0] = 0;
    camera.zoom = 100;
    camera.scan_mode = on_code != NULL;
    camera.torch_checked = false;
    camera.torch_available = false;
    lv_obj_set_hidden(camera.scan_frame, on_code == NULL);
    for (int i = 0; i < 4; i++) lv_obj_set_hidden(camera.scan_shade[i], on_code == NULL);
    lv_obj_set_hidden(camera.panel, false);
    lv_obj_move_foreground(camera.panel);
    lv_obj_update_layout(camera.panel);
    fit_preview();
    layout_scan_frame();
    update_scan_region();
    set_zoom(camera.zoom);
    lv_obj_set_hidden(camera.status, false);
    if (camera_driver_ready()) {
        camera_capture_start(camera.scan_mode);
    } else {
        camera.phase = INSTALLING;
        camera_driver_ui_open(installation_done);
    }
    if (!camera.timer) camera.timer = lv_timer_create(refresh, 33, NULL);
    refresh(NULL);
}

void camera_open(void)
{
    open_camera(NULL);
}

void camera_scan(void (*on_code)(const char *activation))
{
    open_camera(on_code);
}

bool camera_scanning(void)
{
    return camera.phase != CLOSED && camera.on_code;
}

bool camera_key(const struct key_event *key)
{
    if (key->kind == KEY_KIND_CHAR && key->ch == '*' && camera.torch_available) {
        if (camera.phase == CAPTURING && key->down && !key->repeat) {
            if (torch_set(!torch_enabled())) set_zoom(camera.zoom);
            else {
                dialog_open(L("补光灯操作失败", "Camera light failed"), strerror(errno));
                dialog_cancel();
            }
        }
        return true;
    }
    if (key->kind == KEY_KIND_LEFT || key->kind == KEY_KIND_RIGHT) {
        if (camera.phase == CAPTURING && !camera.on_code && key->down && !key->repeat) {
            if (torch_enabled() && !torch_set(false)) perror("turn off camera light");
            camera.scan_mode = !camera.scan_mode;
            camera.last_qr[0] = 0;
            lv_obj_set_hidden(camera.scan_frame, !camera.scan_mode);
            for (int i = 0; i < 4; i++) lv_obj_set_hidden(camera.scan_shade[i], !camera.scan_mode);
            camera_capture_start(camera.scan_mode);
            set_zoom(camera.zoom);
        }
        return true;
    }
    if (key->kind == KEY_KIND_UP || key->kind == KEY_KIND_DOWN) {
        if (key->down && !key->repeat) {
            if (key->kind == KEY_KIND_UP && camera.zoom < 300)
                set_zoom(camera.zoom + 25);
            else if (key->kind == KEY_KIND_DOWN && camera.zoom > 100)
                set_zoom(camera.zoom - 25);
        }
        return true;
    }
    if (key->kind != KEY_KIND_OK) return false;
    if (camera.phase == CAPTURING && !camera.on_code && !camera.scan_mode && key->down && !key->repeat)
        camera_capture_photo();
    return true;
}
