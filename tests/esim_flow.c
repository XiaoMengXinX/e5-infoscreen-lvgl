#define screen_option test_screen_option
#define screen_option_set test_screen_option_set
#include "../src/esim.c"
#undef screen_option
#undef screen_option_set
#include <assert.h>
#include <stdarg.h>

struct app app = {.en = true};
struct theme theme = {.scale = 1, .surface_opa = LV_OPA_COVER};
static lv_group_t *group;
static int stops, cancels;
static bool accepted;
static char command[128];
static char installation[512];
static char interface_option[16], backend_option[16], configured_backend[16];
static bool configured_csim;
static const char *command_aid;
static lpac_done_fn completion;
int S(int px) { return px; }
const lv_font_t *F(int px) { return LV_FONT_DEFAULT; }
const lv_font_t *FB(int px) { return LV_FONT_DEFAULT; }
lv_obj_t *icon_create(lv_obj_t *parent, const char *name, int size) { return lv_obj_create(parent); }
void keyboard_bind(lv_obj_t *obj) {}
void keyboard_close(void) {}
void install_cancel(void) {}
void install_run(const char *title, const char *script, void (*done)(void))
{
    snprintf(installation, sizeof installation, "%s", script);
}
void toast(const char *fmt, ...) {}
void idle_reset(void) {}
lv_group_t *input_group(void) { return group; }
void input_set_group(lv_group_t *next) { group = next; }
void input_defocus(void) {}
void input_focus_key(uint32_t key) {}
void input_focus_move(lv_group_t *g, int dir) {}
lv_obj_t *input_key_focus(lv_group_t *g) { return lv_group_get_focused(g); }
bool lpac_present(void) { return true; }
void lpac_set_backend(const char *mode) { snprintf(configured_backend, sizeof configured_backend, "%s", mode); }
void lpac_set_interface(bool csim) { configured_csim = csim; }
void test_screen_option(const char *name, char *out, size_t size)
{
    snprintf(out, size, "%s", !strcmp(name, "esim_interface") ? interface_option : backend_option);
}
bool test_screen_option_set(const char *name, const char *value)
{
    if (!strcmp(name, "esim_interface"))
        snprintf(interface_option, sizeof interface_option, "%s", value);
    else
        snprintf(backend_option, sizeof backend_option, "%s", value);
    return true;
}
void lpac_stop(void) { stops++; }
void lpac_cancel(void) { cancels++; }
bool lpac_download_confirm(bool accept) { accepted = accept; return true; }
void lpac_sim_refresh(int card, lpac_done_fn done) { completion = done; }
void lpac_probe_estk(int card, lpac_done_fn done) { completion = done; }
void lpac_call(int card, const char *aid, char *const argv[], lpac_progress_fn progress, lpac_done_fn done)
{
    command_aid = aid;
    snprintf(command, sizeof command, "%s %s %s %s", argv[0], argv[1] ? argv[1] : "", argv[1] && argv[2] ? argv[2] : "", argv[1] && argv[2] && argv[3] ? argv[3] : "");
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
    on_download_progress("es9p_get_bound_profile_package", NULL);
    assert(has_text(dialog_sheet(), "Fetching the profile…"));
    assert(!state.write.bar);
    cJSON *writing = cJSON_Parse("{\"written\":1024,\"total\":2048,\"percent\":50}");
    on_download_progress("es10b_load_bound_profile_package", writing);
    assert(lv_bar_get_value(state.write.bar) == 50);
    assert(!lv_obj_is_hidden(state.write.bar));
    assert(strstr(lv_label_get_text(state.write.value), "50%"));
    on_download_progress("es10b_load_bound_profile_package:progress", writing);
    assert(lv_bar_get_value(state.write.bar) == 50);
    cJSON_Delete(writing);
    dialog_close();
    assert(cancels == 1 && state.cancelling && state.view == V_WAIT && !state.step);
    assert(!state.write.bar);
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

    reply("{\"eidValue\":\"SE1-EID\",\"EUICCInfo2\":{}}");
    cJSON *downloaded = cJSON_Parse("[{\"iccid\":\"new-profile\",\"profileName\":\"Downloaded profile\",\"profileState\":\"disabled\"}]");
    on_loaded(true, downloaded, NULL, "");
    cJSON_Delete(downloaded);
    assert(state.view == V_LOADING);
    on_notif_count(true, NULL, NULL, "");
    assert(state.view == V_ROOT && has_text(ui.body, "Downloaded profile"));
    on_save(NULL);
    reply("null");
    assert(!strncmp(command, "profile list", 12));
    reply("[{\"iccid\":\"new-profile\",\"profileNickname\":\"Renamed profile\",\"profileState\":\"disabled\"}]");
    assert(state.view == V_ROOT && has_text(ui.body, "Renamed profile"));

    cJSON_Delete(state.slots[1].notifs);
    state.slots[1].notifs = cJSON_Parse("[{\"seqNumber\":1},{\"seqNumber\":2}]");
    state.view = V_NOTIFS;
    on_switcher = false;
    render();
    lv_obj_t *process_all = lv_obj_get_child(ui.notif_actions, 0);
    lv_obj_t *delete_all = lv_obj_get_child(ui.notif_actions, 1);
    lv_obj_t *first_notification = end_entry(ui.notif_list, false);
    focus_entry(process_all);
    assert(esim_key(&(struct key_event){.kind = KEY_KIND_RIGHT}));
    assert(input_key_focus(input_group()) == delete_all);
    assert(esim_key(&(struct key_event){.kind = KEY_KIND_DOWN}));
    assert(input_key_focus(input_group()) == first_notification);
    assert(esim_key(&(struct key_event){.kind = KEY_KIND_UP}));
    assert(input_key_focus(input_group()) == process_all);
    assert(esim_key(&(struct key_event){.kind = KEY_KIND_RIGHT}));
    assert(esim_key(&(struct key_event){.kind = KEY_KIND_LEFT}));
    assert(input_key_focus(input_group()) == process_all);
    assert(esim_key(&(struct key_event){.kind = KEY_KIND_UP}) && on_switcher);

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
    pick = true;
    int slot = state.current;
    on_chip(false, NULL, "python_ssl", "");
    assert(state.view == V_MISSING && state.current == slot);
    on_install(NULL);
    assert(strstr(installation, "add --progress-fd 3 python3-openssl "));
    assert(!strstr(installation, "gnutls") && !strstr(installation, " lpac"));
    dialog_close();
    state.csim = true;
    on_interface(NULL);
    assert(!strncmp(command, "__backends", 10));
    reply("{\"system\":true,\"at:curl\":false,\"stdio\":true}");
    assert(state.backends_ready && state.backends == ((1u << 1) | (1u << 3)));
    assert(lv_obj_has_state(lv_obj_get_child(ui.body, 4), LV_STATE_DISABLED));
    assert(state.view == V_INTERFACE && has_text(ui.body, "AT+CSIM") && has_text(ui.body, "AT+CCHO / CGLA"));
    lv_group_focus_obj(ui.interfaces[1]);
    assert(esim_key(&(struct key_event){.kind = KEY_KIND_RIGHT}));
    assert(!state.csim && !configured_csim && !strcmp(interface_option, "logical"));
    assert(!state.slots[0].scanned && !state.slots[1].scanned);
    on_interface(NULL);
    int previous_stops = stops;
    choose_interface(false);
    assert(stops == previous_stops && state.view == V_ROOT);
    on_interface(NULL);
    lv_group_focus_obj(ui.interfaces[0]);
    assert(esim_key(&(struct key_event){.kind = KEY_KIND_LEFT}));
    assert(state.csim && configured_csim && !strcmp(interface_option, "csim"));
    on_interface(NULL);
    reply("{\"system\":true,\"at:curl\":false,\"stdio\":true}");
    lv_obj_send_event(lv_obj_get_child(ui.body, 5), LV_EVENT_CLICKED, NULL);
    assert(!strcmp(backend_option, "stdio") && !strcmp(configured_backend, "stdio"));
    assert(state.view == V_LOADING && !state.slots[1].scanned);
    puts("eSIM: background switching, preview, cancellation, notifications, rename and multi-SE routing passed");
}
