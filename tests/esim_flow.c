#include "../src/esim.c"
#include <assert.h>
#include <stdarg.h>

struct app app = {.en = true};
struct theme theme = {.scale = 1, .surface_opa = LV_OPA_COVER};
static lv_group_t *group;
static int stops, cancels;
static bool accepted;
static char command[128];
static lpac_done_fn completion;
int S(int px) { return px; }
const lv_font_t *F(int px) { return LV_FONT_DEFAULT; }
const lv_font_t *FB(int px) { return LV_FONT_DEFAULT; }
lv_obj_t *icon_create(lv_obj_t *parent, const char *name, int size) { return lv_obj_create(parent); }
void keyboard_bind(lv_obj_t *obj) {}
void keyboard_close(void) {}
void install_cancel(void) {}
void install_run(const char *title, const char *script, void (*done)(void)) {}
void toast(const char *fmt, ...) {}
void idle_reset(void) {}
lv_group_t *input_group(void) { return group; }
void input_set_group(lv_group_t *next) { group = next; }
void input_defocus(void) {}
void input_focus_key(uint32_t key) {}
void input_focus_move(lv_group_t *g, int dir) {}
lv_obj_t *input_key_focus(lv_group_t *g) { return lv_group_get_focused(g); }
bool lpac_present(void) { return true; }
void lpac_stop(void) { stops++; }
void lpac_cancel(void) { cancels++; }
bool lpac_download_confirm(bool accept) { accepted = accept; return true; }
void lpac_sim_refresh(int card, lpac_done_fn done) { completion = done; }
void lpac_call(int card, char *const argv[], lpac_progress_fn progress, lpac_done_fn done)
{
    snprintf(command, sizeof command, "%s %s %s %s", argv[0], argv[1], argv[2] ? argv[2] : "", argv[2] && argv[3] ? argv[3] : "");
    completion = done;
}

static void reply(const char *json)
{
    cJSON *data = cJSON_Parse(json);
    lpac_done_fn done = completion;
    done(true, data, NULL, "");
    cJSON_Delete(data);
}

static bool has_text(lv_obj_t *obj, const char *text)
{
    if (lv_obj_check_type(obj, &lv_label_class) && !strcmp(lv_label_get_text(obj), text))
        return true;
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++)
        if (has_text(lv_obj_get_child(obj, (int32_t)i), text))
            return true;
    return false;
}

int main(void)
{
    lv_init();
    lv_display_create(320, 480);
    group = lv_group_create();
    lv_group_set_default(group);
    esim_create(lv_screen_active());
    open_now = true;
    state.current = 1;

    state.view = V_WAIT;
    state.action = A_SWITCH;
    reload_timer = lv_timer_create(reload_switch, RETRY_MS, NULL);
    lv_timer_t *pending_reload = reload_timer;
    esim_close();
    assert(!open_now && stops == 0 && reload_timer == pending_reload);
    esim_open();
    assert(open_now && state.current == 1 && reload_timer == pending_reload);
    cancel_reload();

    state.view = V_DOWNLOAD;
    state.action = A_DOWNLOAD;
    state.downloading = true;
    download_dialog();
    cJSON *metadata = cJSON_Parse("{\"serviceProviderName\":\"Test operator\",\"profileName\":\"Test profile\",\"iccid\":\"12345\",\"profileClass\":\"operational\"}");
    on_download_progress("es8p_meatadata_parse", metadata);
    cJSON_Delete(metadata);
    on_download_progress("preview", NULL);
    assert(dialog_is_open() && !state.step);
    assert(has_text(dialog_sheet(), "Test operator") && has_text(dialog_sheet(), "Test profile"));
    on_accept_download(NULL);
    assert(accepted && state.step);
    dialog_close();
    assert(cancels == 1 && state.cancelling && state.view == V_WAIT && !state.step);
    on_downloaded(false, NULL, "cancelled", "");
    assert(!state.downloading && state.view == V_DOWNLOAD);

    state.action = A_DELETE;
    state.view = V_WAIT;
    state.before = cJSON_Parse("[{\"seqNumber\":10}]");
    cJSON *empty = cJSON_CreateArray();
    on_auto_list(true, empty, NULL, "");
    cJSON_Delete(empty);
    assert(!bulk.running && state.view == V_LOADING);

    state.action = A_DELETE;
    state.before = cJSON_Parse("[{\"seqNumber\":10}]");
    cJSON *notifications = cJSON_Parse("[{\"seqNumber\":10},{\"seqNumber\":12},{\"seqNumber\":11}]");
    on_auto_list(true, notifications, NULL, "");
    cJSON_Delete(notifications);
    assert(bulk.total == 2 && !strcmp(command, "notification process -r 11"));
    completion(false, NULL, "notification", "Network error");
    assert(bulk.failed == 1 && !strcmp(command, "notification process -r 12"));
    reply("null");
    assert(!bulk.running && !bulk.seqs && state.view == V_LOADING);

    state.slots[1].chip = cJSON_Parse("{\"EUICCInfo2\":{\"svn\":\"2.2.0\",\"profileVersion\":\"2.1.0\",\"extCardResource\":{\"freeNonVolatileMemory\":280222,\"freeVolatileMemory\":5970},\"euiccCiPKIdListForSigning\":[\"test-key-id\"]}}");
    on_info(NULL);
    assert(has_text(ui.body, "2.2.0") && has_text(ui.body, "274 KB") && has_text(ui.body, "5.8 KB") && has_text(ui.body, "test-key-id"));
    state.view = V_ROOT;
    dialog_open("Profile", NULL);
    on_rename(NULL);
    assert(!dialog_is_open() && state.view == V_RENAME);
    on_switcher = true;
    assert(!esim_key(&(struct key_event){.kind = KEY_KIND_OK}));
    state.view = V_ROOT;
    render();
    lv_obj_t *body_entry = lv_obj_get_child(ui.body, 0);
    assert(esim_key(&(struct key_event){.kind = KEY_KIND_DOWN}));
    assert(lv_obj_get_child(ui.body, 0) == body_entry);
    puts("eSIM: background switching, preview, cancellation, automatic notifications and rename passed");
}
