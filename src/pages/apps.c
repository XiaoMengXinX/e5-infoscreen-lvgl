#include "app.h"
#include "speedtest.h"
#include "stk.h"
#include "theme.h"
#include "ui.h"

static lv_obj_t *list, *speed, *toolkit;

static void on_phone(lv_event_t *e)
{
    phone_open();
}

static void on_phone_settings(lv_event_t *e)
{
    settings_open_category("plugin:phone", L("电话设置", "Phone settings"), NULL);
}

static void show(lv_obj_t *view)
{
    if (view != speed)
        speedtest_stop();
    if (view != toolkit)
        stk_close();
    lv_obj_set_hidden(list, view != list);
    lv_obj_set_hidden(speed, view != speed);
    lv_obj_set_hidden(toolkit, view != toolkit);
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

static void create(lv_obj_t *page)
{
    list = ui_box(page, LV_FLEX_FLOW_COLUMN, ui_stack_gap());
    ui_row(list, "phone", L("电话", "Phone"), L("双卡拨号和来电", "Dual-SIM dialer and calls"), ROW_CHEVRON, on_phone,
           NULL);
    ui_row(list, "traffic", L("网速测试", "Speed test"), NULL, ROW_CHEVRON, on_speedtest, NULL);
    ui_row(list, "simcard", L("SIM 卡应用", "SIM toolkit"), NULL, ROW_CHEVRON, on_toolkit, NULL);
    ui_row(list, "settings", L("电话设置", "Phone settings"), NULL, ROW_CHEVRON, on_phone_settings, NULL);
    speed = ui_box(page, LV_FLEX_FLOW_COLUMN, 0);
    /* The dial is sized for the panel; larger display sizes would push it off. */
    float scale = theme_cap(1);
    speedtest_create(speed);
    theme.scale = scale;
    toolkit = ui_box(page, LV_FLEX_FLOW_COLUMN, 0);
    stk_create(toolkit);
    lv_obj_set_hidden(toolkit, true);
    lv_obj_set_hidden(speed, !speedtest_running());
    lv_obj_set_hidden(list, speedtest_running());
}

/* Back and leaving the page close the open app, stopping a speed test in progress. */
static bool back(void)
{
    if (!lv_obj_is_hidden(list))
        return false;
    if (lv_obj_is_hidden(toolkit) || !stk_back())
        show(list);
    return true;
}

static void hidden(void)
{
    show(list);
}

const struct page_ops apps_page = {
    .icon = "apps", .zh = "应用", .en = "Apps",
    .create = create, .hidden = hidden, .back = back,
};
