#include "app.h"
#include "ui.h"

static void on_phone(lv_event_t *e)
{
    phone_open();
}

static void on_phone_settings(lv_event_t *e)
{
    settings_open_category("plugin:phone", L("电话设置", "Phone settings"), NULL);
}

static void create(lv_obj_t *page)
{
    ui_row(page, "phone", L("电话", "Phone"), L("双卡拨号和来电", "Dual-SIM dialer and calls"),
           ROW_CHEVRON, on_phone, NULL);
    ui_row(page, "settings", L("电话设置", "Phone settings"), NULL, ROW_CHEVRON, on_phone_settings, NULL);
}

const struct page_ops apps_page = {
    .icon = "apps", .zh = "应用", .en = "Apps",
    .create = create,
};
