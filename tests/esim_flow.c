#include "../src/esim.c"
#include <assert.h>
#include <stdarg.h>

struct app app = {.en = true};
struct theme theme = {.scale = 1, .surface_opa = LV_OPA_COVER};
static lv_group_t *group;
static int stops, cancels;
static bool accepted;
static char command[128];
static const char *command_aid;
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
void lpac_probe_estk(int card, lpac_done_fn done) { completion = done; }
void lpac_call(int card, const char *aid, char *const argv[], lpac_progress_fn progress, lpac_done_fn done)
{
    command_aid = aid;
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

    copy_text(state.iccid, sizeof state.iccid, "12345");
    state.enabled = false;
    state.reloads = 2;
    cJSON *old_state = cJSON_Parse("[{\"iccid\":\"12345\",\"profileState\":\"disabled\"}]");
    on_switch_list(true, old_state, NULL, "");
    cJSON_Delete(old_state);
    assert(state.view == V_WAIT && reload_timer && state.reloads == 1);
    cancel_reload();
    cJSON *new_state = cJSON_Parse("[{\"iccid\":\"12345\",\"profileState\":\"enabled\"}]");
    on_switch_list(true, new_state, NULL, "");
    cJSON_Delete(new_state);
    assert(state.view == V_ROOT && !reload_timer);

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

    state.pending = 1;
    state.view = V_LOADING;
    on_vendor(true, NULL, NULL, "");
    assert(command_aid == se_aids[0]);
    reply("{\"eidValue\":\"SE0-EID\",\"EUICCInfo2\":{}}");
    assert(command_aid == se_aids[1]);
    reply("{\"eidValue\":\"SE1-EID\",\"EUICCInfo2\":{}}");
    assert(state.slots[1].ses == 3 && command_aid == se_aids[0]);
    assert(jstr_is(state.slots[1].chip, "eidValue", "SE0-EID"));
    state.view = V_ROOT;
    render();
    assert(lv_obj_get_group(ui.se_switcher) == input_group());
    assert(!lv_obj_get_group(lv_obj_get_child(ui.se_switcher, 0)));
    assert(!lv_obj_get_group(lv_obj_get_child(ui.se_switcher, 1)));
    on_switcher = true;
    assert(esim_key(&(struct key_event){.kind = KEY_KIND_DOWN}));
    assert(input_key_focus(input_group()) == ui.se_switcher);
    assert(esim_key(&(struct key_event){.kind = KEY_KIND_RIGHT}));
    assert(command_aid == se_aids[1] && restore_se_focus);
    reply("{\"eidValue\":\"SE1-EID\",\"EUICCInfo2\":{}}");
    on_loaded(true, NULL, NULL, "");
    on_notif_count(true, NULL, NULL, "");
    assert(input_key_focus(input_group()) == ui.se_switcher && !restore_se_focus);
    assert(esim_key(&(struct key_event){.kind = KEY_KIND_LEFT}));
    assert(command_aid == se_aids[0]);
    reply("{\"eidValue\":\"SE0-EID\",\"EUICCInfo2\":{}}");
    on_loaded(true, NULL, NULL, "");
    on_notif_count(true, NULL, NULL, "");
    lv_obj_send_event(lv_obj_get_child(ui.se_switcher, 1), LV_EVENT_CLICKED, NULL);
    assert(command_aid == se_aids[1] && !state.slots[1].profiles && !state.slots[1].notifs);
    reply("{\"eidValue\":\"SE1-EID\",\"EUICCInfo2\":{}}");
    state.before = cJSON_Parse("[]");
    cJSON *new_notification = cJSON_Parse("[{\"seqNumber\":20}]");
    on_auto_list(true, new_notification, NULL, "");
    cJSON_Delete(new_notification);
    assert(!strcmp(command, "notification process -r 20"));
    assert(command_aid == se_aids[1]);
    reply("null");
    on_delete(NULL);
    assert(command_aid == se_aids[1] && !strncmp(command, "notification list", 17));
    reply("[]");
    assert(command_aid == se_aids[1] && !strncmp(command, "profile delete", 14));
    reply("null");
    assert(command_aid == se_aids[1] && !strncmp(command, "notification list", 17));
    reply("[]");
    assert(command_aid == se_aids[1] && !strncmp(command, "chip info", 9));
    state.activation = strdup("LPA:1$test.example$test");
    on_start_download(NULL);
    assert(command_aid == se_aids[1] && !strncmp(command, "notification list", 17));
    reply("[]");
    assert(command_aid == se_aids[1] && !strncmp(command, "profile download", 16));
    reply("null");
    assert(command_aid == se_aids[1] && !strncmp(command, "notification list", 17));
    reply("[]");
    assert(command_aid == se_aids[1] && !strncmp(command, "chip info", 9));

    state.slots[1].ses = 0;
    state.slots[1].aid = NULL;
    state.pending = 1;
    on_vendor(true, NULL, NULL, "");
    completion(false, NULL, "euicc_init", "");
    reply("{\"eidValue\":\"SE1-EID\",\"EUICCInfo2\":{}}");
    assert(state.slots[1].ses == 2 && command_aid == se_aids[1]);
    state.slots[1].ses = 0;
    state.slots[1].aid = NULL;
    state.pending = 1;
    on_vendor(true, NULL, NULL, "");
    completion(false, NULL, "euicc_init", "");
    completion(false, NULL, "euicc_init", "");
    assert(!state.slots[1].ses && !command_aid);
    state.slots[1].ses = 0;
    state.slots[1].aid = NULL;
    state.pending = 1;
    on_vendor(false, NULL, "refresh", "");
    assert(!state.slots[1].ses && !command_aid);
    puts("eSIM: background switching, preview, cancellation, notifications, rename and multi-SE routing passed");
}
