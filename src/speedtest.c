/* The test programs print their progress only to a terminal, so they run on a
 * pseudo-terminal and the lines they redraw are parsed. */
#include "speedtest.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "src/misc/lv_text_private.h"

#include "app.h"
#include "icons.h"
#include "theme.h"
#include "ui.h"
#include "util.h"

/* The version comes from jsDelivr: the proxies answer releases/latest with a
 * relative redirect that uclient-fetch cannot follow. */
static const char global_install[] =
    "fetch() {\n"
    "  if command -v curl >/dev/null; then curl -fsSL --connect-timeout 10 --max-time 300 -o \"$2\" \"$1\"\n"
    "  else timeout 300 uclient-fetch -q -T 10 -O \"$2\" \"$1\"; fi\n"
    "}\n"
    "dir=$(mktemp -d) && trap 'rm -rf \"$dir\"' EXIT && cd \"$dir\" || exit 1\n"
    "fetch https://data.jsdelivr.com/v1/packages/gh/XiaoMengXinX/globalspeedtest-go/resolved latest &&\n"
    "  version=$(jsonfilter -i latest -e @.version) || exit 1\n"
    "release=https://github.com/XiaoMengXinX/globalspeedtest-go/releases/download/v$version\n"
    "name=globalspeedtest-go_${version}_Linux_arm64.tar.gz\n"
    "for mirror in https://gh-proxy.org/ https://cdn.gh-proxy.org/ https://ghproxy.net/ ''; do\n"
    "  fetch \"$mirror$release/checksums.txt\" sums && fetch \"$mirror$release/$name\" \"$name\" &&\n"
    "    grep \" $name\\$\" sums | sha256sum -c -s || continue\n"
    "  tar -xzOf \"$name\" globalspeedtest-go > /usr/bin/globalspeedtest-go.part &&\n"
    "    chmod 755 /usr/bin/globalspeedtest-go.part &&\n"
    "    mv /usr/bin/globalspeedtest-go.part /usr/bin/globalspeedtest-go\n"
    "  exit\n"
    "done\n"
    "exit 1\n";

struct backend {
    const char *id, *zh, *en;
    const char *program;
    const char *install;
};

static const struct backend backends[] = {
    {"global", "全球网测", "CAICT", "/usr/bin/globalspeedtest-go", global_install},
    {"speedtest", "speedtest.net", "speedtest.net", "/usr/bin/speedtest-go", NULL},
};
#define BACKENDS ((int)(sizeof backends / sizeof *backends))
static const struct backend *backend = &backends[0];

static const double marks[] = {0, 5, 10, 50, 100, 250, 500, 1000};
#define SEGMENTS ((int)(sizeof marks / sizeof *marks) - 1)
#define SWEEP 270

enum phase { IDLE, INSTALLING, CONNECTING, PING, DOWNLOAD, UPLOAD, DONE, FAILED };

static struct {
    pid_t pid;
    int fd;
    char line[512];
    size_t len;
    enum phase phase;
    double ping, jitter, down, up, shown;
    char isp[64], ip[48], sponsor[64], place[64];
} test = {.pid = -1, .fd = -1};

static struct {
    lv_obj_t *root, *arc, *needle, *readout, *value, *unit, *go;
    lv_obj_t *down, *up, *ping, *jitter, *sponsor, *place, *isp, *ip;
    lv_obj_t *cells[2];
    lv_obj_t *marks[SEGMENTS + 1];
} ui;
static lv_timer_t *reader;

static float gauge_position(double mbps)
{
    if (!(mbps > 0))
        return 0;
    for (int i = 0; i < SEGMENTS; i++)
        if (mbps < marks[i + 1])
            return (float)(i + (mbps - marks[i]) / (marks[i + 1] - marks[i])) / SEGMENTS;
    return 1;
}

static lv_color_t phase_color(void)
{
    return test.phase == UPLOAD ? lv_color_hex(0xc06bff) : theme.accent;
}

static void draw_needle(lv_event_t *e)
{
    if (test.phase != DOWNLOAD && test.phase != UPLOAD)
        return;
    lv_obj_t *obj = lv_event_get_target_obj(e);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    float cx = (a.x1 + a.x2 + 1) / 2.0f, cy = (a.y1 + a.y2 + 1) / 2.0f;
    float r = lv_area_get_width(&a) / 2.0f;
    float angle = (135 + SWEEP * gauge_position(test.shown)) * (float)M_PI / 180;
    float dx = cosf(angle), dy = sinf(angle), half = S(4);
    lv_draw_triangle_dsc_t needle;
    lv_draw_triangle_dsc_init(&needle);
    needle.color = theme.text;
    needle.opa = LV_OPA_90;
    needle.p[0] = (lv_point_precise_t){cx + dx * r * 0.66f, cy + dy * r * 0.66f};
    needle.p[1] = (lv_point_precise_t){cx - dy * half, cy + dx * half};
    needle.p[2] = (lv_point_precise_t){cx + dy * half, cy - dx * half};
    lv_draw_triangle(lv_event_get_layer(e), &needle);
    lv_draw_rect_dsc_t hub;
    lv_draw_rect_dsc_init(&hub);
    hub.radius = LV_RADIUS_CIRCLE;
    hub.bg_color = theme.bg;
    hub.border_color = phase_color();
    hub.border_width = S(3);
    int32_t h = S(7);
    lv_area_t at = {(int32_t)cx - h, (int32_t)cy - h, (int32_t)cx + h, (int32_t)cy + h};
    lv_draw_rect(lv_event_get_layer(e), &hub, &at);
}

static void paint_gauge(void)
{
    if (!ui.root)
        return;
    bool busy = test.phase >= INSTALLING && test.phase <= UPLOAD;
    bool running = test.phase >= CONNECTING && test.phase <= UPLOAD;
    float at = gauge_position(test.shown);
    lv_arc_set_value(ui.arc, (int32_t)lroundf(at * 1000));
    lv_obj_set_style_arc_color(ui.arc, phase_color(), LV_PART_INDICATOR);
    for (int i = 0; i <= SEGMENTS; i++) {
        lv_obj_set_style_text_color(ui.marks[i], (float)i / SEGMENTS <= at ? theme.text : theme.dim, 0);
        lv_obj_set_hidden(ui.marks[i], !running);
    }
    lv_obj_set_hidden(ui.go, busy);
    lv_obj_set_hidden(ui.arc, !running);
    lv_obj_set_hidden(ui.readout, !busy);
    if (test.phase == DOWNLOAD || test.phase == UPLOAD) {
        ui_set_fmt(ui.value, test.shown >= 100 ? "%.0f" : "%.1f", test.shown);
        ui_set_fmt(ui.unit, "%s Mbps", test.phase == UPLOAD ? "↑" : "↓");
    } else if (test.phase == PING && test.ping > 0) {
        ui_set_fmt(ui.value, "%.0f", test.ping);
        ui_set_text(ui.unit, L("延迟 ms", "Ping ms"));
    } else {
        ui_set_text(ui.value, "···");
        ui_set_text(ui.unit, test.phase == INSTALLING ? L("正在下载安装", "Installing") : L("连接服务器", "Connecting"));
    }
    lv_obj_set_style_text_color(ui.unit, busy ? phase_color() : theme.dim, 0);
    lv_obj_invalidate(ui.needle);
}

static void shown_anim(void *var, int32_t v)
{
    test.shown = v / 100.0;
    paint_gauge();
}

static void show_rate(double mbps)
{
    lv_anim_delete(&test, shown_anim);
    if (!theme.motion || !ui.root) {
        test.shown = mbps;
        paint_gauge();
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &test);
    lv_anim_set_exec_cb(&a, shown_anim);
    lv_anim_set_values(&a, (int32_t)lround(test.shown * 100), (int32_t)lround(mbps * 100));
    lv_anim_set_duration(&a, 300);
    lv_anim_set_path_cb(&a, ui_ease_out);
    lv_anim_start(&a);
}

static void paint_result(lv_obj_t *label, double value, const char *fmt)
{
    if (value > 0)
        ui_set_fmt(label, fmt, value);
    else
        ui_set_text(label, "—");
}

static void fit_info(lv_obj_t *cell);

static void paint(void)
{
    if (!ui.root)
        return;
    paint_result(ui.down, test.down, "%.1f");
    paint_result(ui.up, test.up, "%.1f");
    paint_result(ui.ping, test.ping, "%.0f");
    paint_result(ui.jitter, test.jitter, "%.0f");
    ui_set_text(ui.sponsor, *test.sponsor ? test.sponsor : L("切换后端", "Switch backend"));
    ui_set_text(ui.place, *test.place ? test.place : L(backend->zh, backend->en));
    ui_set_text(ui.isp, *test.isp ? test.isp : L("本机网络", "This network"));
    ui_set_text(ui.ip, *test.ip ? test.ip : "—");
    for (int i = 0; i < 2; i++)
        fit_info(ui.cells[i]);
    /* The backend can be switched only until the first test after the view opens. */
    lv_obj_set_state(ui.cells[1], LV_STATE_DISABLED, test.phase != IDLE);
    paint_gauge();
}

static double number_after(const char *line, const char *key)
{
    const char *at = strstr(line, key);
    return at ? strtod(at + strlen(key), NULL) : 0;
}

/* A rate line with its figure; the bare "Upload" line that opens the stage has none. */
static bool rate_after(const char *line, const char *key, double *mbps)
{
    const char *at = strstr(line, key);
    char *end;
    if (!at)
        return false;
    *mbps = strtod(at + strlen(key), &end);
    return end != at + strlen(key);
}

/* "[12687] 6.56km Singapore by SIMBA Telecom" -> "SIMBA Telecom", "Singapore";
 * "[2104002] 大连联通 (中国) by 联通" -> "联通", "大连联通" */
static void parse_server(const char *s)
{
    const char *place = strstr(s, "] ");
    const char *by = strstr(s, " by ");
    if (!place || !by || by < place) {
        snprintf(test.sponsor, sizeof test.sponsor, "%s", s);
        return;
    }
    place += 2;
    const char *km = strstr(place, "km ");
    if (km && km < by)
        place = km + 3;
    const char *country = strstr(place, " (");
    const char *end = country && country < by ? country : by;
    snprintf(test.sponsor, sizeof test.sponsor, "%s", by + 4);
    snprintf(test.place, sizeof test.place, "%.*s", (int)(end - place), place);
}

/* False when the test is already past the stage: the screen is redrawn on
 * every tick, so finished stages keep reappearing. */
static bool reach(enum phase phase)
{
    if (test.phase > phase)
        return false;
    if (test.phase < phase)
        test.shown = 0;
    test.phase = phase;
    return true;
}

static void parse(const char *line)
{
    idle_reset();
    const char *s;
    double v;
    bool final = strstr(line, "Used:") != NULL;
    if ((s = strstr(line, "ISP: "))) {
        /* "ISP: 116.86.135.131 (StarHub) [lat, lon]" */
        const char *open = strchr(s, '('), *close = open ? strchr(open, ')') : NULL;
        if (open && close) {
            snprintf(test.isp, sizeof test.isp, "%.*s", (int)(close - open - 1), open + 1);
            snprintf(test.ip, sizeof test.ip, "%.*s", (int)(open - s - 6), s + 5);
        }
    } else if (strstr(line, "Switching to the next server")) {
        lv_anim_delete(&test, shown_anim);
        test.phase = CONNECTING;
        test.ping = test.jitter = test.down = test.up = test.shown = 0;
    } else if ((s = strstr(line, "Test Server: "))) {
        parse_server(s + 13);
        reach(PING);
    } else if (strstr(line, "Download")) {
        if (!reach(DOWNLOAD) || !rate_after(line, "Download:", &v))
            return;
        if (final)
            test.down = v;
        show_rate(v);
    } else if (strstr(line, "Upload")) {
        if (!reach(UPLOAD))
            return;
        if (!rate_after(line, "Upload:", &v)) {
            paint();
            return;
        }
        if (final)
            test.up = v;
        show_rate(v);
    } else if ((s = strstr(line, "Latency: ")) && reach(PING)) {
        test.ping = number_after(s, "Latency: ");
        if (strstr(s, "Jitter: "))
            test.jitter = number_after(s, "Jitter: ");
    } else {
        return;
    }
    paint();
}

static void take_line(void)
{
    char clean[sizeof test.line];
    size_t n = 0;
    for (size_t i = 0; i < test.len; i++) {
        if (test.line[i] == 0x1b) {
            if (i + 1 < test.len && test.line[i + 1] == '[') {
                i += 2;
                while (i < test.len && !((test.line[i] >= 'A' && test.line[i] <= 'Z') ||
                                         (test.line[i] >= 'a' && test.line[i] <= 'z')))
                    i++;
            } else {
                i++;
            }
            continue;
        }
        clean[n++] = test.line[i];
    }
    clean[n] = 0;
    test.len = 0;
    if (n)
        parse(clean);
}

static void run(enum phase phase, char *const argv[]);

static void run_test(void)
{
    run(CONNECTING, (char *const[]){(char *)backend->program, NULL});
}

static int reap(void)
{
    int status = 0;
    close(test.fd);
    test.fd = -1;
    waitpid(test.pid, &status, 0);
    test.pid = -1;
    lv_anim_delete(&test, shown_anim);
    test.shown = 0;
    return status;
}

static void finish(void)
{
    int status = reap();
    bool exited = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (test.phase == INSTALLING) {
        if (exited) {
            run_test();
            return;
        }
        test.phase = IDLE;
        toast("%s", L("下载安装失败，请检查网络后重试", "The download failed; check the network and retry"));
        paint();
        return;
    }
    bool ok = exited && test.down > 0;
    test.phase = ok ? DONE : FAILED;
    if (!ok)
        toast("%s", L("测速失败，请检查网络后重试", "Speed test failed; check the network and retry"));
    paint();
}

static void poll_output(lv_timer_t *t)
{
    char buf[1024];
    for (;;) {
        ssize_t n = read(test.fd, buf, sizeof buf);
        if (n > 0) {
            for (ssize_t i = 0; i < n; i++) {
                if (buf[i] == '\r' || buf[i] == '\n')
                    take_line();
                else if (test.len < sizeof test.line - 1)
                    test.line[test.len++] = buf[i];
            }
            continue;
        }
        if (n < 0 && errno == EAGAIN)
            return;
        /* EOF, or EIO once the program has exited and closed the terminal. */
        lv_timer_delete(t);
        reader = NULL;
        finish();
        return;
    }
}

static void run(enum phase phase, char *const argv[])
{
    int fd;
    pid_t pid = forkpty(&fd, NULL, NULL, NULL);
    if (pid < 0) {
        toast("%s", L("无法启动测速", "Cannot start the speed test"));
        return;
    }
    if (pid == 0) {
        execv(argv[0], argv);
        _exit(127);
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    test = (typeof(test)){.pid = pid, .fd = fd, .phase = phase};
    reader = lv_timer_create(poll_output, 50, NULL);
    paint();
}

static void on_install(lv_event_t *e)
{
    dialog_close();
    run(INSTALLING, (char *const[]){"/bin/sh", "-c", (char *)backend->install, NULL});
}

static void start(void)
{
    if (test.pid >= 0)
        return;
    if (!access(backend->program, X_OK)) {
        run_test();
        return;
    }
    const char *name = strrchr(backend->program, '/') + 1;
    if (!backend->install) {
        toast(L("未安装 %s", "%s is not installed"), name);
        return;
    }
    char note[160];
    snprintf(note, sizeof note,
             L("%s需要 %s，是否从 GitHub 下载最新版本并安装？", "%s needs %s. Download the latest release from GitHub and install it?"),
             L(backend->zh, backend->en), name);
    dialog_open(L("安装测速程序", "Install the speed test"), note);
    dialog_button(L("下载安装", "Install"), on_install, NULL);
    dialog_cancel();
}

void speedtest_reset(void)
{
    if (test.pid >= 0)
        return;
    test = (typeof(test)){.pid = -1, .fd = -1};
    paint();
}

void speedtest_stop(void)
{
    if (test.pid < 0)
        return;
    kill(test.pid, SIGKILL);
    lv_timer_delete(reader);
    reader = NULL;
    reap();
    test.phase = IDLE;
    paint();
}

bool speedtest_running(void)
{
    return test.pid >= 0;
}

static void load_backend(void)
{
    char id[32];
    screen_option("speedtest_backend", id, sizeof id);
    for (int i = 0; i < BACKENDS; i++)
        if (!strcmp(id, backends[i].id))
            backend = &backends[i];
}

static void on_backend(lv_event_t *e)
{
    backend = lv_event_get_user_data(e);
    dialog_close();
    screen_option_set("speedtest_backend", backend->id);
    speedtest_reset();
}

static void on_server(lv_event_t *e)
{
    dialog_open(L("切换后端", "Switch backend"), NULL);
    for (int i = 0; i < BACKENDS; i++) {
        lv_obj_t *button = dialog_button(L(backends[i].zh, backends[i].en), on_backend, (void *)&backends[i]);
        if (&backends[i] == backend)
            ui_primary(button);
    }
    dialog_cancel();
}

static void on_go(lv_event_t *e)
{
    start();
}

static lv_obj_t *headline(lv_obj_t *parent, const char *arrow, const char *title, lv_color_t color)
{
    lv_obj_t *cell = ui_box(parent, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_set_width(cell, 0);
    lv_obj_set_flex_grow(cell, 1);
    lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *head = ui_box(cell, LV_FLEX_FLOW_ROW, 4);
    lv_obj_set_width(head, LV_SIZE_CONTENT);
    lv_label_set_text(ui_text(head, 13, true, color), arrow);
    lv_label_set_text(ui_text(head, 11, false, theme.dim), title);
    lv_label_set_text(ui_text(head, 9, false, theme.dim), "Mbps");
    lv_obj_t *value = ui_text(cell, 24, true, theme.text);
    lv_label_set_text_static(value, "—");
    return value;
}

static lv_obj_t *stat(lv_obj_t *parent, const char *title)
{
    lv_obj_t *row = ui_box(parent, LV_FLEX_FLOW_ROW, 4);
    lv_obj_set_width(row, LV_SIZE_CONTENT);
    lv_label_set_text(ui_text(row, 11, false, theme.dim), title);
    lv_obj_t *value = ui_text(row, 12, true, theme.text);
    lv_label_set_text_static(value, "—");
    lv_label_set_text(ui_text(row, 10, false, theme.dim), "ms");
    return value;
}

static lv_obj_t *info_line(lv_obj_t *parent, int size, bool bold, lv_color_t color)
{
    lv_obj_t *label = ui_text(parent, size, bold, color);
    lv_obj_set_height(label, lv_font_get_line_height(lv_obj_get_style_text_font(label, 0)));
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    return label;
}

/* A label sized by LVGL measures the text wrapped into words, so the dots would
 * cut the first word short; size it to the unwrapped text instead. */
static void fit_info(lv_obj_t *cell)
{
    lv_obj_t *icon = lv_obj_get_child(cell, 0), *text = lv_obj_get_child(cell, 1);
    int32_t room = lv_obj_get_content_width(cell) - lv_obj_get_width(icon) - lv_obj_get_style_pad_column(cell, 0);
    for (uint32_t i = 0; i < lv_obj_get_child_count(text); i++) {
        lv_obj_t *label = lv_obj_get_child(text, (int32_t)i);
        const char *line = lv_label_get_text(label);
        lv_text_attributes_t attributes = {.letter_space = lv_obj_get_style_text_letter_space(label, 0)};
        int32_t width = lv_text_get_width(line, strlen(line), lv_obj_get_style_text_font(label, 0), &attributes);
        lv_obj_set_width(label, LV_MIN(width, room));
    }
}

static void on_info_size(lv_event_t *e)
{
    fit_info(lv_event_get_current_target_obj(e));
}

static lv_obj_t *info(lv_obj_t *parent, const char *icon, lv_obj_t **title, lv_obj_t **detail)
{
    lv_obj_t *cell = ui_box(parent, LV_FLEX_FLOW_ROW, 6);
    lv_obj_set_width(cell, 0);
    lv_obj_set_flex_grow(cell, 1);
    lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(cell, S(4), 0);
    lv_obj_set_style_radius(cell, S(11), 0);
    lv_obj_set_style_text_color(icon_create(cell, icon, S(18)), theme.dim, 0);
    lv_obj_t *text = ui_box(cell, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_set_width(text, LV_SIZE_CONTENT);
    *title = info_line(text, 11, true, theme.text);
    *detail = info_line(text, 10, false, theme.dim);
    lv_obj_add_event_cb(cell, on_info_size, LV_EVENT_SIZE_CHANGED, NULL);
    return cell;
}

void speedtest_create(lv_obj_t *parent)
{
    lv_anim_delete(&test, shown_anim);
    memset(&ui, 0, sizeof ui);
    ui.root = ui_box(parent, LV_FLEX_FLOW_COLUMN, 4);
    lv_obj_set_flex_align(ui.root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *heads = ui_box(ui.root, LV_FLEX_FLOW_ROW, 8);
    ui.down = headline(heads, "↓", L("下载", "Download"), theme.accent);
    ui.up = headline(heads, "↑", L("上传", "Upload"), lv_color_hex(0xc06bff));
    lv_obj_t *stats = ui_box(ui.root, LV_FLEX_FLOW_ROW, 16);
    lv_obj_set_width(stats, LV_SIZE_CONTENT);
    ui.ping = stat(stats, L("延迟", "Ping"));
    ui.jitter = stat(stats, L("抖动", "Jitter"));

    int32_t size = S(196);
    lv_obj_t *dial = lv_obj_create(ui.root);
    lv_obj_remove_style_all(dial);
    lv_obj_set_size(dial, size, size * 96 / 100);
    lv_obj_set_scrollable(dial, false);
    lv_obj_set_style_pad_top(dial, S(4), 0);

    ui.arc = lv_arc_create(dial);
    lv_obj_set_size(ui.arc, size, size);
    lv_obj_align(ui.arc, LV_ALIGN_TOP_MID, 0, 0);
    lv_arc_set_rotation(ui.arc, 135);
    lv_arc_set_bg_angles(ui.arc, 0, SWEEP);
    lv_arc_set_range(ui.arc, 0, 1000);
    lv_obj_set_clickable(ui.arc, false);
    lv_obj_remove_style(ui.arc, NULL, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(ui.arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_arc_width(ui.arc, S(10), 0);
    lv_obj_set_style_arc_width(ui.arc, S(10), LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(ui.arc, theme.line, 0);
    lv_obj_set_style_arc_rounded(ui.arc, true, 0);
    lv_obj_set_style_arc_rounded(ui.arc, true, LV_PART_INDICATOR);

    for (int i = 0; i <= SEGMENTS; i++) {
        float angle = (135 + SWEEP * (float)i / SEGMENTS) * (float)M_PI / 180;
        float r = size / 2.0f - S(26);
        ui.marks[i] = ui_text(dial, 10, false, theme.dim);
        lv_label_set_text_fmt(ui.marks[i], "%d", (int)marks[i]);
        lv_obj_align(ui.marks[i], LV_ALIGN_TOP_MID, (int32_t)lroundf(cosf(angle) * r),
                     (int32_t)lroundf(size / 2.0f + sinf(angle) * r - S(7)));
    }

    ui.needle = lv_obj_create(dial);
    lv_obj_remove_style_all(ui.needle);
    lv_obj_set_size(ui.needle, size, size);
    lv_obj_align(ui.needle, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_clickable(ui.needle, false);
    lv_obj_add_event_cb(ui.needle, draw_needle, LV_EVENT_DRAW_MAIN, NULL);

    ui.readout = ui_box(dial, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_set_width(ui.readout, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(ui.readout, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(ui.readout, LV_ALIGN_TOP_MID, 0, size / 2 + S(10));
    ui.value = ui_text(ui.readout, 32, true, theme.text);
    ui.unit = ui_text(ui.readout, 11, true, theme.dim);

    ui.go = lv_button_create(dial);
    lv_obj_remove_style_all(ui.go);
    lv_obj_set_size(ui.go, S(108), S(108));
    lv_obj_align(ui.go, LV_ALIGN_TOP_MID, 0, size / 2 - S(54));
    lv_obj_set_style_radius(ui.go, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(ui.go, S(3), 0);
    lv_obj_set_style_border_color(ui.go, theme.accent, 0);
    lv_obj_set_style_bg_color(ui.go, theme.accent, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(ui.go, LV_OPA_20, LV_STATE_PRESSED);
    lv_obj_set_style_transform_scale(ui.go, 240, LV_STATE_PRESSED);
    lv_obj_set_style_transform_pivot_x(ui.go, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(ui.go, LV_PCT(50), 0);
    lv_obj_t *go = ui_text(ui.go, 24, true, theme.text);
    lv_label_set_text(go, L("开始", "GO"));
    lv_obj_center(go);
    lv_obj_add_event_cb(ui.go, on_go, LV_EVENT_CLICKED, NULL);
    ui_focusable(ui.go);
    lv_obj_set_style_outline_width(ui.go, 0, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_bg_color(ui.go, theme.accent, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_bg_opa(ui.go, LV_OPA_30, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_border_color(ui.go, theme.text, LV_STATE_FOCUS_KEY);

    lv_obj_t *where = ui_box(ui.root, LV_FLEX_FLOW_ROW, 10);
    lv_obj_set_style_pad_top(where, S(6), 0);
    ui.cells[0] = info(where, "user", &ui.isp, &ui.ip);
    lv_obj_t *server = ui.cells[1] = info(where, "globe", &ui.sponsor, &ui.place);
    /* Centred exactly, the content still looks right of centre in the focus box. */
    lv_obj_set_style_pad_right(server, S(12), 0);
    lv_obj_set_style_bg_color(server, theme.button, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(server, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_clickable(server, true);
    lv_obj_add_event_cb(server, on_server, LV_EVENT_CLICKED, NULL);
    ui_focusable(server);
    load_backend();
    paint();
}
