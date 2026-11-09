#include "../src/install.c"
#include <assert.h>
#include <sys/prctl.h>

struct app app = {.en = true};
struct theme theme = {.scale = 1, .surface_opa = LV_OPA_COVER};
static lv_group_t *group;
static bool completed;
static unsigned wakeups;
int S(int px) { return px; }
const lv_font_t *F(int px) { return LV_FONT_DEFAULT; }
const lv_font_t *FB(int px) { return LV_FONT_DEFAULT; }
void toast(const char *fmt, ...) {}
void idle_reset(void) { wakeups++; }
lv_group_t *input_group(void) { return group; }
void input_set_group(lv_group_t *next) { group = next; }
void input_focus_key(uint32_t key) {}
void input_focus_move(lv_group_t *g, int dir) {}
static void installed(void) { completed = true; }

static void wait_script(void)
{
    for (int i = 0; install.pid >= 0 && i < 1000; i++) {
        poll_output(reader);
        usleep(1000);
    }
    assert(install.pid < 0);
}

int main(void)
{
    lv_init();
    lv_display_create(320, 480);
    group = lv_group_create();
    lv_group_set_default(group);

    install_run("Install", "printf '@add\\r10/40\\r'; read step; printf '40/40'", installed);
    unsigned before = wakeups;
    for (int i = 0; strcmp(lv_label_get_text(install.meter.value), "25%") && i < 500; i++) {
        poll_output(reader);
        usleep(1000);
    }
    assert(!strcmp(lv_label_get_text(install.meter.value), "25%"));
    assert(lv_bar_get_value(install.meter.bar) == 25 && wakeups > before);
    assert(write(install.fd, "\n", 1) == 1);
    wait_script();
    assert(completed && !dialog_is_open() && !install.open);

    completed = false;
    install_run("Install", "printf '@add\\r'; exit 1", installed);
    wait_script();
    assert(!completed && dialog_is_open() && !install_running());
    assert(!strcmp(lv_label_get_text(install.meter.value), "Incomplete"));
    assert(!strcmp(lv_label_get_text(lv_obj_get_child(install.button, 0)), "Close"));
    install_line("################ 42.3%");
    assert(!strcmp(lv_label_get_text(install.meter.value), "42%") && lv_bar_get_value(install.meter.bar) == 42);
    dialog_close();

    assert(prctl(PR_SET_CHILD_SUBREAPER, 1) == 0);
    unlink("/out/tests/install-child.pid");
    install_run("Install", "sleep 30 & echo $! > /out/tests/install-child.tmp; mv /out/tests/install-child.tmp /out/tests/install-child.pid; wait", installed);
    FILE *file = NULL;
    for (int i = 0; !file && i < 500; i++) {
        file = fopen("/out/tests/install-child.pid", "r");
        if (!file)
            usleep(1000);
    }
    assert(file);
    int child;
    assert(fscanf(file, "%d", &child) == 1);
    fclose(file);
    dialog_close();
    assert(!install_running() && !install.open);
    int status;
    pid_t reaped = 0;
    for (int i = 0; !reaped && i < 1000; i++) {
        reaped = waitpid(child, &status, WNOHANG);
        if (!reaped)
            usleep(1000);
    }
    if (!reaped)
        kill(child, SIGKILL);
    if (reaped == -1 && errno == ECHILD)
        assert(kill(child, 0) < 0 && errno == ESRCH);
    else
        assert(reaped == child && WIFSIGNALED(status));
    unlink("/out/tests/install-child.pid");
    puts("Install: real progress, EOF, success, failure and process-group cancellation passed");
}
