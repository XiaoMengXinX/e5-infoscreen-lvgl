#include "app.h"
#include "clash.h"
#include "camera.h"
#include "camera_driver.h"
#include "camera_driver_ui.h"
#include "esim.h"
#include "input.h"
#include "icons.h"
#include "speedtest.h"
#include "stk.h"
#include "theme.h"
#include "torch.h"
#include "ui.h"
#include <errno.h>
#include <string.h>

static lv_obj_t *list, *speed, *toolkit, *toolkit_row, *esim, *clash, *camera, *flashlight;
static lv_obj_t *light_status, *light_button, *light_icon;
static bool light_installing;
/* The row the keys were on when an app opened over the list. */
static lv_obj_t *list_focus;

static void on_phone(lv_event_t *e)
{
    phone_open();
}

static void show(lv_obj_t *view)
{
    if (view != list && !lv_obj_is_hidden(list)) {
        lv_obj_t *focused = input_key_focus(input_group());
        list_focus = focused && lv_obj_get_parent(focused) == list ? focused : NULL;
    }
    if (view != camera)
        camera_close();
    if (view != flashlight && light_installing) {
        camera_driver_ui_cancel();
        light_installing = false;
    }
    if (view != speed)
        speedtest_stop();
    if (view != toolkit)
        stk_close();
    if (view != esim)
        esim_close();
    if (view != clash)
        clash_close();
    lv_obj_set_hidden(list, view != list);
    lv_obj_set_hidden(speed, view != speed);
    lv_obj_set_hidden(toolkit, view != toolkit);
    lv_obj_set_hidden(esim, view != esim);
    lv_obj_set_hidden(clash, view != clash);
    lv_obj_set_hidden(camera, view != camera);
    lv_obj_set_hidden(flashlight, view != flashlight);
    if (view == list && list_focus) {
        ui_focus_key(list_focus);
        list_focus = NULL;
    }
}

static void on_speedtest(lv_event_t *e)
{
    speedtest_reset();
    show(speed);
}

static void on_toolkit(lv_event_t *e)
{
    show(toolkit);
    stk_open();
}

static void on_esim(lv_event_t *e)
{
    show(esim);
    esim_open();
}

static void on_clash(lv_event_t *e)
{
    show(clash);
    clash_open();
}

static void on_camera(lv_event_t *e)
{
    show(camera);
    camera_open();
}

static void light_refresh(void)
{
    bool on = torch_enabled();
    lv_color_t color = on ? theme.fair : theme.dim;
    lv_color_t surface = on ? lv_color_mix(theme.fair, theme.bg, 35) : theme.surface;
    ui_set_text(light_status, on ? L("已开启", "On") : L("已关闭", "Off"));
    lv_obj_set_style_text_color(light_status, color, 0);
    lv_obj_set_style_text_color(light_icon, color, 0);
    lv_obj_set_style_bg_color(light_button, surface, 0);
    lv_obj_set_style_bg_color(light_button, lv_color_mix(color, surface, 40), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(light_button, on ? theme.fair : theme.line, 0);
    lv_obj_set_style_outline_color(light_button, color, LV_STATE_FOCUS_KEY);
}

void apps_toggle_torch(void)
{
    if (!torch_set(!torch_enabled())) {
        dialog_open(L("手电筒操作失败", "Flashlight failed"), strerror(errno));
        dialog_cancel();
    }
    if (!lv_obj_is_hidden(flashlight)) light_refresh();
}

static void light_toggle(lv_event_t *event)
{
    apps_toggle_torch();
}

static void light_layout(lv_event_t *event)
{
    lv_obj_t *area = lv_event_get_target_obj(event);
    int32_t size = LV_MIN(S(148), LV_MIN(lv_obj_get_content_width(area), lv_obj_get_content_height(area)));
    lv_obj_set_size(light_button, size, size);
    lv_obj_set_size(light_icon, size * 64 / 148, size * 64 / 148);
}

static void light_installed(bool success)
{
    light_installing = false;
    if (!success) show(list);
}

static void on_flashlight(lv_event_t *event)
{
    show(flashlight);
    light_refresh();
    ui_focus_key(light_button);
    if (!camera_driver_ready()) {
        light_installing = true;
        camera_driver_ui_open(light_installed);
    }
}

static void create(lv_obj_t *page)
{
    list_focus = NULL;
    lv_obj_set_scrollable(page, false);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_set_style_pad_gap(page, 0, 0);
    list = ui_page(page);
    lv_obj_set_style_pad_gap(list, S(ui_stack_gap()), 0);
    ui_row(list, "phone", L("电话", "Phone"), NULL, ROW_CHEVRON, on_phone, NULL);
    ui_row(list, "traffic", L("网速测试", "Speed test"), NULL, ROW_CHEVRON, on_speedtest, NULL);
    toolkit_row = ui_row(list, "simcard", L("SIM 卡应用", "SIM toolkit"), NULL, ROW_CHEVRON, on_toolkit, NULL);
    ui_row(list, "chip", L("eSIM 管理", "eSIM Manager"), NULL, ROW_CHEVRON, on_esim, NULL);
    ui_row(list, "globe", L("Mihomo 面板", "Mihomo panel"), NULL, ROW_CHEVRON, on_clash, NULL);
    if (camera_supported()) ui_row(list, "camera", L("相机", "Camera"), NULL, ROW_CHEVRON, on_camera, NULL);
    if (torch_supported()) ui_row(list, "flashlight", L("手电筒", "Flashlight"), NULL, ROW_CHEVRON, on_flashlight, NULL);
    flashlight = ui_page(page);
    lv_obj_set_scrollable(flashlight, false);
    if (torch_supported()) {
        lv_obj_set_flex_align(flashlight, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(flashlight, S(12), 0);
        lv_obj_t *title = ui_text(flashlight, 22, true, theme.text);
        ui_set_text(title, L("手电筒", "Flashlight"));
        lv_obj_t *area = ui_box(flashlight, LV_FLEX_FLOW_COLUMN, 0);
        lv_obj_set_flex_grow(area, 1);
        lv_obj_set_layout(area, LV_LAYOUT_NONE);
        light_button = ui_button(area, "", light_toggle, NULL);
        lv_obj_set_size(light_button, S(148), S(148));
        lv_obj_center(light_button);
        lv_obj_set_style_radius(light_button, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(light_button, 1, 0);
        lv_obj_set_layout(light_button, LV_LAYOUT_NONE);
        lv_obj_set_hidden(lv_obj_get_child(light_button, 0), true);
        light_icon = icon_create(light_button, "flashlight", S(64));
        lv_obj_center(light_icon);
        lv_obj_add_event_cb(area, light_layout, LV_EVENT_SIZE_CHANGED, NULL);
        light_status = ui_text(flashlight, 18, true, theme.dim);
        lv_obj_t *hint = ui_text(flashlight, 12, false, theme.dim);
        ui_set_text(hint, L("轻触或按OK键开关\n长按侧键开关手电筒", "Tap or press OK to toggle\nHold side key to toggle"));
        lv_obj_set_width(hint, LV_PCT(100));
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
        light_refresh();
    }
    lv_obj_set_hidden(flashlight, true);
    camera = ui_box(page, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_set_height(camera, LV_PCT(100));
    if (camera_supported()) camera_create(camera);
    lv_obj_set_hidden(camera, true);
    speed = ui_page(page);
    /* The dial is sized for the panel; larger display sizes would push it off. */
    float scale = theme_cap(1);
    speedtest_create(speed);
    theme.scale = scale;
    toolkit = ui_page(page);
    stk_create(toolkit);
    lv_obj_set_hidden(toolkit, true);
    esim = ui_page(page);
    esim_create(esim);
    lv_obj_set_hidden(esim, true);
    clash = ui_page(page);
    clash_create(clash);
    lv_obj_set_hidden(clash, true);
    lv_obj_set_hidden(toolkit_row, !stk_enabled());
    lv_obj_set_hidden(speed, !speedtest_running());
    lv_obj_set_hidden(list, speedtest_running());
}

void apps_open_toolkit(void)
{
    if (!stk_enabled())
        return;
    shell_show(PAGE_APPS);
    show(toolkit);
    stk_open();
}

/* Back and leaving the page close the open app; back during a speed test only cancels it. */
static bool back(void)
{
    if (!lv_obj_is_hidden(list))
        return false;
    if (!lv_obj_is_hidden(speed) && speedtest_cancel())
        return true;
    if ((lv_obj_is_hidden(esim) || !esim_back()) && (lv_obj_is_hidden(toolkit) || !stk_back()) &&
        (lv_obj_is_hidden(clash) || !clash_back()))
        show(list);
    return true;
}

static void hidden(void)
{
    show(list);
}

/* The toolkit's setting may have changed since the list was built. */
static void shown(void)
{
    lv_obj_set_hidden(toolkit_row, !stk_enabled());
}

/* In an app Left and Right do not change the page. */
static bool key(const struct key_event *k)
{
    if (!lv_obj_is_hidden(flashlight)) {
        if (k->kind == KEY_KIND_OK) {
            if (k->down && !k->repeat && !light_installing) light_toggle(NULL);
            return true;
        }
        return k->kind == KEY_KIND_LEFT || k->kind == KEY_KIND_RIGHT;
    }
    if (camera_scanning())
        return camera_key(k);
    if (!lv_obj_is_hidden(camera))
        return camera_key(k);
    if (!lv_obj_is_hidden(toolkit))
        return stk_key(k);
    if (!lv_obj_is_hidden(esim))
        return esim_key(k);
    if (!lv_obj_is_hidden(clash))
        return clash_key(k);
    return !lv_obj_is_hidden(speed) && (k->kind == KEY_KIND_LEFT || k->kind == KEY_KIND_RIGHT);
}

static bool held(void)
{
    return lv_obj_is_hidden(list);
}

const struct page_ops apps_page = {
    .icon = "apps", .zh = "应用", .en = "Apps",
    .create = create, .shown = shown, .hidden = hidden, .back = back, .key = key, .held = held,
};
