/* Speed test: runs speedtest-go (speedtest.net servers) and shows its live
 * figures on a gauge. Its progress is only printed to a terminal, so it runs on
 * a pseudo-terminal and the screen lines it redraws are parsed as they come. */
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

#include "app.h"
#include "icons.h"
#include "theme.h"
#include "ui.h"

#define PROGRAM "/usr/bin/speedtest-go"

/* Gauge marks, evenly spaced around the dial as on the speedtest.net app. */
static const double marks[] = {0, 5, 10, 50, 100, 250, 500, 1000};
#define SEGMENTS ((int)(sizeof marks / sizeof *marks) - 1)
#define SWEEP 270

enum phase { IDLE, CONNECTING, PING, DOWNLOAD, UPLOAD, DONE, FAILED };

static struct {
    pid_t pid;
    int fd;
    char line[512];
    size_t len;
    enum phase phase;
    double ping, jitter, down, up, shown;
    char isp[64], ip[48], sponsor[64], place[64], loss[32];
} test = {.pid = -1, .fd = -1};

static struct {
    lv_obj_t *root, *arc, *needle, *readout, *value, *unit, *go;
    lv_obj_t *down, *up, *ping, *jitter, *sponsor, *place, *isp, *ip;
    lv_obj_t *marks[SEGMENTS + 1];
} ui;
static lv_timer_t *reader;

/* ---------- gauge ---------- */

static float gauge_position(double mbps)
{
    if (!(mbps > 0))
        return 0;
    for (int i = 0; i < SEGMENTS; i++)
        if (mbps < marks[i + 1])
            return (float)(i + (mbps - marks[i]) / (marks[i + 1] - marks[i])) / SEGMENTS;
    return 1;
}

static bool uploading(void)
{
    return test.phase == UPLOAD;
}

static lv_color_t phase_color(void)
{
    return uploading() ? lv_color_hex(0xc06bff) : theme.accent;
}

/* A tapered needle from a hub at the dial's centre. */
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
    bool running = test.phase >= CONNECTING && test.phase <= UPLOAD;
    float at = gauge_position(test.shown);
    lv_arc_set_value(ui.arc, (int32_t)lroundf(at * 1000));
    lv_obj_set_style_arc_color(ui.arc, phase_color(), LV_PART_INDICATOR);
    for (int i = 0; i <= SEGMENTS; i++)
        lv_obj_set_style_text_color(ui.marks[i], running && (float)i / SEGMENTS <= at ? theme.text : theme.dim, 0);
    /* Before a test, and after one, only the button shows; the dial is for the test. */
    lv_obj_set_hidden(ui.go, running);
    lv_obj_set_hidden(ui.arc, !running);
    for (int i = 0; i <= SEGMENTS; i++)
        lv_obj_set_hidden(ui.marks[i], !running);
    lv_obj_set_hidden(ui.readout, !running);
    if (test.phase == DOWNLOAD || test.phase == UPLOAD) {
        ui_set_fmt(ui.value, test.shown >= 100 ? "%.0f" : "%.1f", test.shown);
        ui_set_fmt(ui.unit, "%s Mbps", uploading() ? "↑" : "↓");
    } else if (test.phase == PING && test.ping > 0) {
        ui_set_fmt(ui.value, "%.0f", test.ping);
        ui_set_text(ui.unit, L("延迟 ms", "Ping ms"));
    } else {
        ui_set_text(ui.value, "···");
        ui_set_text(ui.unit, L("连接服务器", "Connecting"));
    }
    lv_obj_set_style_text_color(ui.unit, running ? phase_color() : theme.dim, 0);
    lv_obj_invalidate(ui.needle);
}

static void shown_anim(void *var, int32_t v)
{
    test.shown = v / 100.0;
    paint_gauge();
}

/* The needle eases to each new reading rather than jumping. */
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

/* ---------- results ---------- */

static void paint_result(lv_obj_t *label, double value, const char *fmt)
{
    if (value > 0)
        ui_set_fmt(label, fmt, value);
    else
        ui_set_text(label, "—");
}

static void paint(void)
{
    if (!ui.root)
        return;
    paint_result(ui.down, test.down, "%.1f");
    paint_result(ui.up, test.up, "%.1f");
    paint_result(ui.ping, test.ping, "%.0f");
    paint_result(ui.jitter, test.jitter, "%.0f");
    ui_set_text(ui.sponsor, *test.sponsor ? test.sponsor : L("自动选择", "Automatic"));
    ui_set_text(ui.place, *test.place ? test.place : "speedtest.net");
    ui_set_text(ui.isp, *test.isp ? test.isp : L("本机网络", "This network"));
    ui_set_text(ui.ip, *test.ip ? test.ip : "—");
    paint_gauge();
}

/* ---------- speedtest-go output ---------- */

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

/* "Test Server: [12687] 6.56km Singapore by SIMBA Telecom" -> "SIMBA Telecom", "Singapore" */
static void parse_server(const char *s)
{
    const char *place = strstr(s, "km ");
    const char *by = strstr(s, " by ");
    if (!place || !by || by < place) {
        snprintf(test.sponsor, sizeof test.sponsor, "%s", s);
        return;
    }
    place += 3;
    snprintf(test.sponsor, sizeof test.sponsor, "%s", by + 4);
    snprintf(test.place, sizeof test.place, "%.*s", (int)(by - place), place);
}

/* Moves on to a stage; false when the test is already past it. speedtest-go
 * redraws its whole screen on every tick, so finished stages keep reappearing. */
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
    /* A test in progress keeps the screen awake. */
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
    } else if ((s = strstr(line, "Packet Loss: "))) {
        snprintf(test.loss, sizeof test.loss, "%s", s + 13);
    } else {
        return;
    }
    paint();
}

/* Keeps the printable text of a redrawn screen line: no colour or cursor codes,
 * no spinner glyph. */
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

static void finish(void)
{
    close(test.fd);
    test.fd = -1;
    int status = 0;
    waitpid(test.pid, &status, 0);
    test.pid = -1;
    lv_anim_delete(&test, shown_anim);
    bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0 && test.down > 0;
    test.phase = ok ? DONE : FAILED;
    test.shown = 0;
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

static void start(void)
{
    if (test.pid >= 0)
        return;
    if (access(PROGRAM, X_OK)) {
        toast("%s", L("未安装 speedtest-go", "speedtest-go is not installed"));
        return;
    }
    int fd;
    pid_t pid = forkpty(&fd, NULL, NULL, NULL);
    if (pid < 0) {
        toast("%s", L("无法启动测速", "Cannot start the speed test"));
        return;
    }
    if (pid == 0) {
        execl(PROGRAM, PROGRAM, (char *)NULL);
        _exit(127);
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    test = (typeof(test)){.pid = pid, .fd = fd, .phase = CONNECTING};
    reader = lv_timer_create(poll_output, 50, NULL);
    paint();
}

/* Opening the view starts clean, without the last round's figures. */
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
    close(test.fd);
    test.fd = -1;
    waitpid(test.pid, NULL, 0);
    test.pid = -1;
    lv_anim_delete(&test, shown_anim);
    test.phase = IDLE;
    test.shown = 0;
    paint();
}

bool speedtest_running(void)
{
    return test.pid >= 0;
}

/* ---------- view ---------- */

static void on_go(lv_event_t *e)
{
    start();
}

/* A headline figure: coloured arrow, caption and unit, then the value. */
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

/* A small figure on the line under the headlines. */
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

static void info(lv_obj_t *parent, const char *icon, lv_obj_t **title, lv_obj_t **detail)
{
    lv_obj_t *cell = ui_box(parent, LV_FLEX_FLOW_ROW, 6);
    lv_obj_set_width(cell, 0);
    lv_obj_set_flex_grow(cell, 1);
    lv_obj_set_style_text_color(icon_create(cell, icon, S(18)), theme.dim, 0);
    lv_obj_t *text = ui_box(cell, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_set_width(text, 0);
    lv_obj_set_flex_grow(text, 1);
    *title = ui_text(text, 11, true, theme.text);
    lv_obj_set_width(*title, LV_PCT(100));
    lv_label_set_long_mode(*title, LV_LABEL_LONG_MODE_DOTS);
    if (detail) {
        *detail = ui_text(text, 10, false, theme.dim);
        lv_obj_set_width(*detail, LV_PCT(100));
        lv_label_set_long_mode(*detail, LV_LABEL_LONG_MODE_DOTS);
    }
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

    /* The marks, inside the ring; the ones the needle has passed light up. */
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

    /* The reading sits under the hub, in the dial's open bottom. */
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
    /* A hollow ring, as the speedtest.net app's GO. */
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
    /* The ring is already the accent; the key focus fills it instead. */
    lv_obj_set_style_outline_width(ui.go, 0, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_bg_color(ui.go, theme.accent, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_bg_opa(ui.go, LV_OPA_30, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_border_color(ui.go, theme.text, LV_STATE_FOCUS_KEY);


    lv_obj_t *where = ui_box(ui.root, LV_FLEX_FLOW_ROW, 10);
    lv_obj_set_style_pad_top(where, S(6), 0);
    info(where, "user", &ui.isp, &ui.ip);
    info(where, "globe", &ui.sponsor, &ui.place);
    paint();
}
