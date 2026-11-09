#define _GNU_SOURCE

#include "lpac.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "lvgl.h"

#include "app.h"
#include "util.h"

#define LPAC "/usr/lib/lpac"
/* The CP's free command ring; t551m-atd holds nr2/nr6/nr7 and ModemManager
 * /dev/wwan0at0. Reading it here leaves the data card's traffic undisturbed. */
#define DEVICE "/dev/stty_nr4"
#define GNUTLS_CURL "/usr/lib/libcurl-gnutls.so.4"
#define TLS_SHIM "/tmp/e5-lpac-tls"
#define QUEUE 8
#define PERIOD 40
#define PROBES 5

struct job {
    int card;
    char **argv;
    lpac_progress_fn progress;
    lpac_done_fn done;
};

static struct {
    struct job q[QUEUE];
    int head, count;
    pid_t pid;
    int out, in, master, dev;
    int card;
    uint32_t started, watchdog;
    char *outbuf;
    size_t outlen, outcap;
    bool preview, cancelling;
    uint32_t cancel_at;
    char ptybuf[4096];
    size_t ptylen;
    bool ok;
    char error[64], detail[512];
    cJSON *result;
    const cJSON *data;
    bool refreshing, refresh_ok;
    lpac_done_fn refresh_done;
} run = {.pid = -1, .out = -1, .in = -1, .master = -1, .dev = -1};

/* The one modem command in flight. */
static struct {
    bool active;
    uint32_t started, timeout;
    char reply[2048];
    size_t len;
    void (*then)(const char *reply);
} at;

/* The lpac AT request being answered: from the line lpac wrote to the answer it
 * waits for. The channel number lpac sends back is only a label; this is the
 * real one, and it changes when the card is recovered. */
static struct {
    bool busy;
    char aid[64];
    char apdu[768];
    char channel[16];
    bool open;
    char ccho_error[8], cgla_error[8];
    int attempt, sweep;
    bool recovered, retried;
    uint32_t wait_from, wait_ms;
    void (*wait_then)(void);
    void (*opened)(const char *channel);
} bridge;

static struct {
    int probes;
    void (*then)(void);
} recover;

static lv_timer_t *poll;
/* A finished job, while its callback queues the next one. */
static bool ending;

static void job_next(void);
static void ticking(void);
static void ccho_send(void);
static void cgla_send(void);
static void on_refresh(const char *reply);

static void copy_text(char *out, size_t size, const char *in)
{
    size_t n = strlen(in);
    if (n >= size)
        n = size - 1;
    memcpy(out, in, n);
    out[n] = 0;
}

/* The mbedTLS libcurl turns away certificate chains carrying critical
 * extensions it does not parse (the GSMA RSP2 roots' Certificate Policies,
 * which some SM-DP+ and notification servers send), where the GnuTLS build
 * continues; lpac loads it through a shim when the package is installed. */
static void tls_shim(void)
{
    if (access(GNUTLS_CURL, R_OK))
        return;
    char link[sizeof TLS_SHIM + 16];
    mkdir(TLS_SHIM, 0700);
    snprintf(link, sizeof link, "%s/libcurl.so.4", TLS_SHIM);
    unlink(link);
    if (!symlink(GNUTLS_CURL, link))
        setenv("LD_LIBRARY_PATH", TLS_SHIM, 1);
}

static void say(const char *line)
{
    if (run.master < 0)
        return;
    char text[sizeof at.reply + 2];
    size_t n = strlen(line);
    if (n > sizeof text - 2)
        n = sizeof text - 2;
    memcpy(text, line, n);
    text[n] = '\r';
    text[n + 1] = '\n';
    ssize_t written = write(run.master, text, n + 2);
    (void)written;
}

static void respond_error(const char *code)
{
    char line[32];
    snprintf(line, sizeof line, "+CME ERROR: %s", code);
    say(line);
    say("ERROR");
}

static bool has_line(const char *reply, const char *text)
{
    size_t n = strlen(text);
    for (const char *p = reply; *p;) {
        size_t len = strcspn(p, "\r\n");
        if (len == n && !strncmp(p, text, n))
            return true;
        p += len + strspn(p + len, "\r\n");
    }
    return false;
}

static bool cme_error(const char *reply, char *code, size_t size)
{
    const char *error = strstr(reply, "+CME ERROR:");
    if (!error)
        return false;
    error += strlen("+CME ERROR:");
    error += strspn(error, " ");
    size_t n = strspn(error, "0123456789");
    if (!n)
        return false;
    if (n >= size)
        n = size - 1;
    memcpy(code, error, n);
    code[n] = 0;
    return true;
}

/* Gone-card errors: the CP powers the standby SIM down after APDU traffic. */
static bool gone(const char *code)
{
    return !strcmp(code, "10") || !strcmp(code, "21") || !strcmp(code, "100") || !strcmp(code, "3");
}

static bool ccho_channel(const char *reply, char *out, size_t size)
{
    const char *line = strstr(reply, "+CCHO:");
    if (!line)
        return false;
    line += strlen("+CCHO:");
    line += strspn(line, " ");
    size_t n = strspn(line, "0123456789");
    if (!n)
        return false;
    if (n >= size)
        n = size - 1;
    memcpy(out, line, n);
    out[n] = 0;
    return true;
}

static bool cgla_line(const char *reply, char *out, size_t size)
{
    const char *line = strstr(reply, "+CGLA:");
    if (!line)
        return false;
    size_t n = strcspn(line, "\r\n");
    if (n >= size)
        return false;
    memcpy(out, line, n);
    out[n] = 0;
    return true;
}

static bool parse_cgla(const char *body, char *out, size_t size)
{
    const char *comma = strchr(body, ',');
    if (!comma)
        return false;
    comma = strchr(comma + 1, ',');
    if (!comma)
        return false;
    const char *open = strchr(comma + 1, '"');
    if (!open)
        return false;
    const char *close = strchr(open + 1, '"');
    if (!close)
        return false;
    size_t n = (size_t)(close - open - 1);
    if (!n || n >= size)
        return false;
    for (size_t i = 0; i < n; i++) {
        if (!isxdigit((unsigned char)open[1 + i]))
            return false;
        out[i] = (char)toupper((unsigned char)open[1 + i]);
    }
    out[n] = 0;
    return true;
}

static int modem_open(void)
{
    int fd = open(DEVICE, O_RDWR | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    struct termios tio;
    if (!tcgetattr(fd, &tio)) {
        cfmakeraw(&tio);
        tcsetattr(fd, TCSANOW, &tio);
    }
    return fd;
}

static void drain(void)
{
    char buf[256];
    while (run.dev >= 0 && read(run.dev, buf, sizeof buf) > 0) {
    }
}

static void at_send(const char *body, uint32_t timeout, void (*then)(const char *reply))
{
    at.active = true;
    at.then = then;
    at.started = lv_tick_get();
    at.timeout = timeout;
    at.len = 0;
    at.reply[0] = 0;
    if (run.dev < 0)
        return;
    drain();
    char command[sizeof at.reply];
    int n = snprintf(command, sizeof command, "AT+SPACTCARD=%d;%s\r\n", run.card, body);
    if (n > 0) {
        ssize_t written = write(run.dev, command, (size_t)n);
        (void)written;
    }
}

static bool at_complete(void)
{
    return strstr(at.reply, "+CME ERROR") || has_line(at.reply, "OK") || has_line(at.reply, "ERROR");
}

static void at_poll(void)
{
    if (!at.active)
        return;
    if (run.dev >= 0) {
        for (;;) {
            size_t room = sizeof at.reply - 1 - at.len;
            if (!room || at_complete())
                break;
            ssize_t n = read(run.dev, at.reply + at.len, room);
            if (n > 0) {
                at.len += (size_t)n;
                at.reply[at.len] = 0;
                continue;
            }
            if (n < 0 && errno == EINTR)
                continue;
            break;
        }
    }
    if (run.dev < 0 || at.len == sizeof at.reply - 1 || at_complete() ||
        lv_tick_elaps(at.started) >= at.timeout) {
        at.active = false;
        void (*then)(const char *) = at.then;
        at.then = NULL;
        then(at.reply);
    }
}

static void later(uint32_t ms, void (*then)(void))
{
    bridge.wait_from = lv_tick_get();
    bridge.wait_ms = ms;
    bridge.wait_then = then;
}

static void wait_poll(void)
{
    if (!bridge.wait_then || lv_tick_elaps(bridge.wait_from) < bridge.wait_ms)
        return;
    void (*then)(void) = bridge.wait_then;
    bridge.wait_then = NULL;
    then();
}

static void probe_card(void);

static void on_sfun(const char *reply)
{
    later(1000, probe_card);
}

static void on_ccid(const char *reply)
{
    if (strstr(reply, "+CCID:")) {
        void (*then)(void) = recover.then;
        recover.then = NULL;
        then();
        return;
    }
    later(1000, probe_card);
}

static void probe_card(void)
{
    if (recover.probes == PROBES) {
        void (*then)(void) = recover.then;
        recover.then = NULL;
        then();
        return;
    }
    recover.probes++;
    at_send("+CCID", 10000, on_ccid);
}

/* +SFUN=2 alone revives the card; anything appended to it powers it down again. */
static void recover_card(void (*then)(void))
{
    recover.probes = 0;
    recover.then = then;
    at_send("+SFUN=2", 20000, on_sfun);
}

/* A profile switch reaches the modem only after its SIM interface restarts:
 * radio off, radio on, then SIM power-up. */
static void send_power(void)
{
    at_send("+SFUN=2", 20000, on_refresh);
}

static void on_radio(const char *reply)
{
    run.refresh_ok &= has_line(reply, "OK");
    later(1000, send_power);
}

static void send_radio(void)
{
    at_send("+SFUN=4", 20000, on_radio);
}

static void on_off(const char *reply)
{
    run.refresh_ok &= has_line(reply, "OK");
    later(1000, send_radio);
}

static void on_sweep(const char *reply)
{
    if (bridge.sweep > 3) {
        if (gone(bridge.ccho_error) && !bridge.recovered) {
            bridge.recovered = true;
            recover_card(ccho_send);
            return;
        }
        ccho_send();
        return;
    }
    char command[64];
    snprintf(command, sizeof command, "+CSIM=10,\"0070800%d00\"", bridge.sweep++);
    at_send(command, 10000, on_sweep);
}

/* Leaked channels make CCHO fail with CME 22; closing 1 to 3 clears them. */
static void sweep_channels(void)
{
    bridge.sweep = 1;
    on_sweep(NULL);
}

static void on_opened(const char *channel)
{
    bridge.busy = false;
    if (!channel) {
        respond_error("10");
        return;
    }
    char line[32];
    snprintf(line, sizeof line, "+CCHO: %s", channel);
    say(line);
    say("");
    say("OK");
}

static void on_ccho(const char *reply)
{
    char code[8], channel[sizeof bridge.channel];
    bool failed = cme_error(reply, code, sizeof code);
    if (!failed && ccho_channel(reply, channel, sizeof channel) && strcmp(channel, "0")) {
        copy_text(bridge.channel, sizeof bridge.channel, channel);
        bridge.open = true;
        bridge.opened(bridge.channel);
        return;
    }
    if (!failed)
        copy_text(code, sizeof code, "10");
    if (bridge.attempt) {
        bridge.opened(NULL);
        return;
    }
    bridge.attempt = 1;
    copy_text(bridge.ccho_error, sizeof bridge.ccho_error, code);
    sweep_channels();
}

static void open_channel(void (*opened)(const char *channel))
{
    bridge.opened = opened;
    bridge.attempt = 0;
    bridge.recovered = false;
    bridge.open = false;
    ccho_send();
}

static void ccho_send(void)
{
    char command[96];
    snprintf(command, sizeof command, "+CCHO=\"%s\"", bridge.aid);
    at_send(command, 20000, on_ccho);
}

static void on_reopened(const char *channel)
{
    if (!channel) {
        bridge.busy = false;
        respond_error(bridge.cgla_error);
        return;
    }
    cgla_send();
}

static void cgla_reopen(void)
{
    open_channel(on_reopened);
}

static void on_cgla(const char *reply)
{
    char code[8], line[sizeof at.reply];
    if (!cme_error(reply, code, sizeof code)) {
        bridge.busy = false;
        if (!cgla_line(reply, line, sizeof line)) {
            say("ERROR");
            return;
        }
        say(line);
        say("");
        say("OK");
        return;
    }
    if (gone(code) && *bridge.aid && !bridge.retried) {
        bridge.retried = true;
        copy_text(bridge.cgla_error, sizeof bridge.cgla_error, code);
        recover_card(cgla_reopen);
        return;
    }
    bridge.busy = false;
    respond_error(code);
}

static void cgla_send(void)
{
    char command[sizeof at.reply];
    snprintf(command, sizeof command, "+CGLA=%s,%d,\"%s\"", bridge.channel, (int)strlen(bridge.apdu), bridge.apdu);
    at_send(command, 25000, on_cgla);
}

static void cgla_request(const char *body)
{
    if (!parse_cgla(body, bridge.apdu, sizeof bridge.apdu)) {
        bridge.busy = false;
        say("ERROR");
        return;
    }
    if (!bridge.open) {
        bridge.busy = false;
        respond_error("21");
        return;
    }
    bridge.retried = false;
    cgla_send();
}

static void on_cchc(const char *reply)
{
    bridge.open = false;
    bridge.channel[0] = 0;
    bridge.busy = false;
    say("OK");
}

static void cchc_request(void)
{
    if (bridge.open) {
        char command[64];
        snprintf(command, sizeof command, "+CCHC=%s", bridge.channel);
        at_send(command, 20000, on_cchc);
        return;
    }
    bridge.busy = false;
    say("OK");
}

static void on_plain(const char *reply)
{
    bridge.busy = false;
    say(has_line(reply, "OK") ? "OK" : "ERROR");
}

static void bridge_line(const char *line)
{
    if (!*line || strncasecmp(line, "AT", 2))
        return;
    const char *body = line + 2;
    size_t len = strlen(body);
    bridge.busy = true;
    if (!len) {
        bridge.busy = false;
        say("OK");
    } else if (len >= 2 && !strcmp(body + len - 2, "=?")) {
        bridge.busy = false;
        say("OK");
    } else if (!strncasecmp(body, "+CCHO=\"", 7)) {
        body += 7;
        size_t n = strcspn(body, "\"");
        if (n >= sizeof bridge.aid)
            n = sizeof bridge.aid - 1;
        memcpy(bridge.aid, body, n);
        bridge.aid[n] = 0;
        open_channel(on_opened);
    } else if (!strncasecmp(body, "+CGLA=", 6)) {
        cgla_request(body + 6);
    } else if (!strncasecmp(body, "+CCHC=", 6)) {
        cchc_request();
    } else {
        at_send(body, 20000, on_plain);
    }
}

static void bridge_clear(void)
{
    bridge = (typeof(bridge)){0};
    recover = (typeof(recover)){0};
}

static void take_json(const char *line)
{
    if (!*line)
        return;
    cJSON *doc = cJSON_Parse(line);
    if (!doc)
        return;
    const char *type = jstr(doc, "type");
    const cJSON *payload = jget(doc, "payload");
    if (type && !strcmp(type, "progress")) {
        const struct job *job = &run.q[run.head];
        if (jstr_is(payload, "message", "preview")) {
            run.preview = true;
            if (run.cancelling)
                lpac_download_confirm(false);
        }
        if (job->progress)
            job->progress(jstr_or(payload, "message", ""), jget(payload, "data"));
        cJSON_Delete(doc);
        return;
    }
    if (!type || strcmp(type, "lpa")) {
        cJSON_Delete(doc);
        return;
    }
    cJSON_Delete(run.result);
    run.result = doc;
    run.data = jget(payload, "data");
    run.ok = jnum_or(payload, "code", -1) == 0;
    copy_text(run.error, sizeof run.error, jstr_or(payload, "message", ""));
    run.detail[0] = 0;
    if (cJSON_IsString(run.data))
        copy_text(run.detail, sizeof run.detail, run.data->valuestring);
}

static void take_lines(void)
{
    size_t start = 0;
    for (;;) {
        char *nl = memchr(run.outbuf + start, '\n', run.outlen - start);
        if (!nl)
            break;
        size_t n = (size_t)(nl - (run.outbuf + start));
        run.outbuf[start + n] = 0;
        if (n && run.outbuf[start + n - 1] == '\r')
            run.outbuf[start + n - 1] = 0;
        take_json(run.outbuf + start);
        start += n + 1;
        if (run.pid < 0)
            return;
    }
    if (start) {
        memmove(run.outbuf, run.outbuf + start, run.outlen - start);
        run.outlen -= start;
    }
}

static void end_job(bool kill_child);

static void output_poll(void)
{
    for (;;) {
        if (run.outcap - run.outlen < 4097) {
            size_t capacity = run.outcap ? run.outcap * 2 : 8192;
            char *buffer = realloc(run.outbuf, capacity);
            if (!buffer) {
                end_job(true);
                return;
            }
            run.outbuf = buffer;
            run.outcap = capacity;
        }
        ssize_t n = read(run.out, run.outbuf + run.outlen, run.outcap - 1 - run.outlen);
        if (n > 0) {
            run.outlen += (size_t)n;
            run.outbuf[run.outlen] = 0;
            take_lines();
            if (run.pid < 0)
                return;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && errno == EAGAIN)
            return;
        if (run.outlen)
            take_json(run.outbuf);
        end_job(false);
        return;
    }
}

static void pty_poll(void)
{
    for (;;) {
        size_t room = sizeof run.ptybuf - 1 - run.ptylen;
        if (!room)
            break;
        ssize_t n = read(run.master, run.ptybuf + run.ptylen, room);
        if (n > 0) {
            run.ptylen += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        break;
    }
    run.ptybuf[run.ptylen] = 0;
    while (!bridge.busy) {
        char *end = strpbrk(run.ptybuf, "\r\n");
        if (!end)
            break;
        size_t n = (size_t)(end - run.ptybuf);
        char line[sizeof run.ptybuf];
        memcpy(line, run.ptybuf, n);
        line[n] = 0;
        size_t used = n + 1;
        memmove(run.ptybuf, run.ptybuf + used, run.ptylen - used);
        run.ptylen -= used;
        run.ptybuf[run.ptylen] = 0;
        bridge_line(line);
    }
}

static void free_job(struct job *job)
{
    for (int i = 1; job->argv[i]; i++)
        free(job->argv[i]);
    free(job->argv);
    job->argv = NULL;
}

static void end_job(bool kill_child)
{
    ending = true;
    if (kill_child)
        kill(run.pid, SIGKILL);
    if (run.out >= 0) {
        close(run.out);
        run.out = -1;
    }
    if (run.master >= 0) {
        close(run.master);
        run.master = -1;
    }
    close(run.in);
    run.in = -1;
    at.active = false;
    at.then = NULL;
    bridge_clear();
    if (run.pid >= 0) {
        waitpid(run.pid, NULL, 0);
        run.pid = -1;
    }
    if (run.dev >= 0) {
        close(run.dev);
        run.dev = -1;
    }
    struct job *job = &run.q[run.head];
    lpac_done_fn done = job->done;
    free_job(job);
    bool seen = run.result != NULL, ok = run.ok;
    run.preview = run.cancelling = false;
    free(run.outbuf);
    run.outbuf = NULL;
    run.outcap = 0;
    const cJSON *data = run.data;
    char error[sizeof run.error], detail[sizeof run.detail];
    copy_text(error, sizeof error, run.error);
    copy_text(detail, sizeof detail, run.detail);
    run.head = (run.head + 1) % QUEUE;
    run.count--;
    run.ok = false;
    run.error[0] = run.detail[0] = 0;
    run.data = NULL;
    run.outlen = run.ptylen = 0;
    if (done) {
        if (seen)
            done(ok, data, ok ? NULL : error, detail);
        else
            done(false, NULL, "lpac", "");
    }
    cJSON_Delete(run.result);
    run.result = NULL;
    ending = false;
    job_next();
}

static bool job_start(void)
{
    struct job *job = &run.q[run.head];
    char slave[128] = "";
    int master = posix_openpt(O_RDWR | O_NOCTTY), slave_fd = -1, fds[2], input[2];
    if (master >= 0 && !grantpt(master) && !unlockpt(master)) {
        const char *path = ptsname(master);
        if (path && strlen(path) < sizeof slave) {
            copy_text(slave, sizeof slave, path);
            slave_fd = open(slave, O_RDWR | O_NOCTTY | O_CLOEXEC);
        }
    }
    if (slave_fd >= 0) {
        struct termios tio;
        if (!tcgetattr(slave_fd, &tio)) {
            cfmakeraw(&tio);
            tcsetattr(slave_fd, TCSANOW, &tio);
        }
    }
    if (slave_fd < 0 || pipe(fds)) {
        if (slave_fd >= 0)
            close(slave_fd);
        if (master >= 0)
            close(master);
        return false;
    }
    if (pipe(input)) {
        close(slave_fd);
        close(master);
        close(fds[0]);
        close(fds[1]);
        return false;
    }
    pid_t pid = fork();
    if (pid == 0) {
        close(input[1]);
        dup2(input[0], STDIN_FILENO);
        close(input[0]);
        close(fds[0]);
        close(master);
        int null = open("/dev/null", O_WRONLY | O_CLOEXEC);
        dup2(fds[1], STDOUT_FILENO);
        if (fds[1] != STDOUT_FILENO)
            close(fds[1]);
        if (null >= 0) {
            dup2(null, STDERR_FILENO);
        }
        setenv("LPAC_APDU", "at", 1);
        setenv("LPAC_APDU_AT_DEVICE", slave, 1);
        setenv("LPAC_HTTP", "curl", 1);
        tls_shim();
        execv(LPAC, job->argv);
        _exit(127);
    }
    close(fds[1]);
    close(input[0]);
    close(slave_fd);
    if (pid < 0) {
        close(input[1]);
        close(fds[0]);
        close(master);
        return false;
    }
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);
    fcntl(master, F_SETFD, FD_CLOEXEC);
    if (run.dev >= 0) {
        close(run.dev);
        run.dev = -1;
    }
    run.pid = pid;
    run.out = fds[0];
    run.in = input[1];
    fcntl(run.in, F_SETFD, FD_CLOEXEC);
    run.master = master;
    run.dev = modem_open();
    run.card = job->card;
    run.started = lv_tick_get();
    run.watchdog = job->argv[1] && job->argv[2] && !strcmp(job->argv[1], "profile") && !strcmp(job->argv[2], "download")
                       ? 300000
                       : 120000;
    run.outlen = run.ptylen = 0;
    run.ok = false;
    run.error[0] = run.detail[0] = 0;
    run.data = NULL;
    bridge_clear();
    ticking();
    return true;
}

static void job_next(void)
{
    if (ending || run.pid >= 0 || run.refreshing || !run.count)
        return;
    if (job_start())
        return;
    struct job *job = &run.q[run.head];
    lpac_done_fn done = job->done;
    free_job(job);
    run.head = (run.head + 1) % QUEUE;
    run.count--;
    if (done)
        done(false, NULL, "lpac", "");
    job_next();
}

static void tick(lv_timer_t *t)
{
    idle_reset();
    if (run.pid >= 0) {
        output_poll();
        if (run.pid >= 0) {
            pty_poll();
            if (run.pid >= 0) {
                wait_poll();
                at_poll();
            }
        }
        if (run.pid >= 0 && ((run.cancelling && lv_tick_elaps(run.cancel_at) >= 60000) ||
                            (!run.preview && lv_tick_elaps(run.started) >= run.watchdog)))
            end_job(true);
    } else if (run.refreshing) {
        wait_poll();
        at_poll();
    }
    if (run.pid < 0 && !run.count && !run.refreshing) {
        if (run.dev >= 0) {
            close(run.dev);
            run.dev = -1;
        }
        if (poll) {
            lv_timer_delete(poll);
            poll = NULL;
        }
    }
}

static void ticking(void)
{
    if (!poll)
        poll = lv_timer_create(tick, PERIOD, NULL);
}

static void on_refresh(const char *reply)
{
    char code[8];
    bool ok = run.refresh_ok && !cme_error(reply, code, sizeof code) && has_line(reply, "OK");
    run.refreshing = false;
    lpac_done_fn done = run.refresh_done;
    run.refresh_done = NULL;
    if (done)
        done(ok, NULL, ok ? NULL : "refresh", "");
    job_next();
}

bool lpac_present(void)
{
    return !access(LPAC, X_OK) && !access(GNUTLS_CURL, R_OK);
}

void lpac_call(int card, char *const argv[], lpac_progress_fn progress, lpac_done_fn done)
{
    if (run.count == QUEUE) {
        done(false, NULL, "lpac", "The command queue is full");
        return;
    }
    int argc = 0;
    while (argv[argc])
        argc++;
    char **args = calloc((size_t)argc + 2, sizeof *args);
    if (!args) {
        done(false, NULL, "lpac", "Out of memory");
        return;
    }
    args[0] = (char *)LPAC;
    for (int i = 0; i < argc; i++) {
        args[i + 1] = strdup(argv[i]);
        if (!args[i + 1]) {
            struct job failed = {.argv = args};
            free_job(&failed);
            done(false, NULL, "lpac", "Out of memory");
            return;
        }
    }
    struct job *job = &run.q[(run.head + run.count) % QUEUE];
    *job = (struct job){.card = card, .argv = args, .progress = progress, .done = done};
    run.count++;
    job_next();
}

bool lpac_download_confirm(bool accept)
{
    if (!run.preview)
        return false;
    const char *answer = accept ? "y\n" : "n\n";
    if (write(run.in, answer, 2) != 2)
        return false;
    run.preview = false;
    run.started = lv_tick_get();
    return true;
}

void lpac_cancel(void)
{
    if (run.pid < 0 || run.cancelling)
        return;
    run.cancelling = true;
    run.cancel_at = lv_tick_get();
    kill(run.pid, SIGINT);
    if (run.preview)
        lpac_download_confirm(false);
}

void lpac_sim_refresh(int card, lpac_done_fn done)
{
    if (run.dev < 0)
        run.dev = modem_open();
    run.card = card;
    run.refreshing = true;
    run.refresh_ok = true;
    run.refresh_done = done;
    ticking();
    at_send("+SFUN=5", 20000, on_off);
}

void lpac_stop(void)
{
    for (int i = 0; i < run.count; i++)
        free_job(&run.q[(run.head + i) % QUEUE]);
    run.count = 0;
    run.head = 0;
    if (run.pid >= 0) {
        kill(run.pid, SIGKILL);
        if (run.out >= 0)
            close(run.out);
        if (run.master >= 0)
            close(run.master);
        waitpid(run.pid, NULL, 0);
        run.pid = -1;
    }
    close(run.in);
    run.in = -1;
    run.out = -1;
    run.master = -1;
    free(run.outbuf);
    run.outbuf = NULL;
    run.outcap = 0;
    run.preview = run.cancelling = false;
    if (run.dev >= 0) {
        close(run.dev);
        run.dev = -1;
    }
    at.active = false;
    at.then = NULL;
    run.refreshing = false;
    run.refresh_done = NULL;
    bridge_clear();
    cJSON_Delete(run.result);
    run.result = NULL;
    run.data = NULL;
    run.ok = false;
    run.error[0] = run.detail[0] = 0;
    run.outlen = run.ptylen = 0;
    if (poll) {
        lv_timer_delete(poll);
        poll = NULL;
    }
}
