/* SIM Toolkit. The modem forwards the SIM's proactive commands as
 * +SPUSATPROCMDIND URCs on each card's URC ring of the AT channel; ModemManager
 * owns that channel and drops them, so a return probe on sbuf_read copies the
 * rings out through a tracefs instance for as long as the screen runs. Answers
 * go back through the AT owner, card by card.
 *
 * As Android's STK app does, the main menu is kept and shown again (here also
 * across restarts, by ICCID), and a command left on screen gets
 * NO RESPONSE FROM USER once its time runs out. */
#include "stk.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "app.h"
#include "input.h"
#include "keyboard.h"
#include "theme.h"
#include "ui.h"
#include "util.h"

#define TRACE "/sys/kernel/debug/tracing"
#define INSTANCE TRACE "/instances/e5stk"
#define PROBE "e5stk/sbuf"
#define AT_HELPER "/usr/libexec/unisoc-modem/at"
#define CACHE "/etc/e5-infoscreen/stk"
#define SESSIONS "/tmp/e5-stk"
#define ITEMS 32
/* Android's StkApp.UI_TIMEOUT, DISP_TEXT_WAIT_FOR_USER_TIMEOUT and
 * DISP_TEXT_CLEAR_AFTER_DELAY_TIMEOUT. */
#define UI_TIMEOUT 30000
#define TEXT_TIMEOUT 60000
#define TEXT_CLEAR_TIMEOUT 15000

/* What the screen answers itself: menus, item lists, text, input and call setup.
 * The modem refuses a TE profile that claims what it handles on its own. */
static const char te_profile[] = "08E09731610D005C6B00001F40000000032B0000000408000000000000000000";

enum { CMD_REFRESH = 0x01, CMD_SET_UP_EVENT_LIST = 0x05, CMD_PLAY_TONE = 0x20, CMD_DISPLAY_TEXT = 0x21,
       CMD_GET_INKEY = 0x22, CMD_GET_INPUT = 0x23, CMD_SELECT_ITEM = 0x24, CMD_SET_UP_MENU = 0x25,
       CMD_PROVIDE_LOCAL_INFO = 0x26, CMD_SET_UP_IDLE_MODE_TEXT = 0x28, CMD_LANGUAGE_NOTIFICATION = 0x35 };
enum { RESULT_OK = 0x00, RESULT_BACK = 0x11, RESULT_NO_RESPONSE = 0x12, RESULT_UNSUPPORTED = 0x30 };
/* V_EMPTY first: a card nobody has opened yet waits for nothing. */
enum view { V_EMPTY, V_LOADING, V_MENU, V_WAIT, V_SELECT, V_TEXT, V_INPUT, V_YESNO, V_CALL };

struct item {
    uint8_t id;
    char text[128];
};

struct cmd {
    uint8_t details[3];
    char title[256], text[512], fill[256];
    struct item items[ITEMS];
    int count, min, max;
    uint32_t duration;
    uint8_t default_item;
    bool immediate;
};

static struct card {
    bool menu_known, no_menu, pending, recovering, registered, auto_clear;
    struct cmd menu, cmd;
    enum view view;
    uint32_t since, deadline, polled;
    char iccid[24], profile[80];
    /* What the cache should hold, until the ICCID it is kept under is known. */
    char menu_hex[600];
    int status_le;
} cards[2];

/* Card 1's URCs come on ring 0 and card 2's on ring 3, once the modem is in
 * its dual-SIM channel mapping; see stk_map_channels. */
static struct ring_line {
    char line[1200];
    size_t len;
} rings[2];

static struct {
    lv_obj_t *root, *switcher, *tabs[2], *body;
    lv_obj_t *field;
} ui;
/* Up from the first entry moves the keys onto the SIM switch, as in the dialer. */
static bool on_switcher;

static int current;
static bool open_now;
/* The card whose command the popup shows, or -1. */
static int popup = -1;
static int ring_dst = -1, ring_ch = -1;
static int trace_fd = -1;
static char trace_buf[8192];
static size_t trace_len;
static lv_timer_t *ticker;

static struct {
    struct {
        int card;
        void (*done)(int card, const char *reply);
        char cmd[640];
    } q[16];
    int head, count;
    pid_t pid;
    int fd;
    char out[1024];
    size_t len;
} at = {.pid = -1, .fd = -1};

static void render(void);

/* ---------- text ---------- */

static const uint16_t gsm[128] = {
    '@', 0xA3, '$', 0xA5, 0xE8, 0xE9, 0xF9, 0xEC, 0xF2, 0xC7, '\n', 0xD8, 0xF8, '\r', 0xC5, 0xE5,
    0x394, '_', 0x3A6, 0x393, 0x39B, 0x3A9, 0x3A0, 0x3A8, 0x3A3, 0x398, 0x39E, 0x1B, 0xC6, 0xE6, 0xDF, 0xC9,
    ' ', '!', '"', '#', 0xA4, '%', '&', '\'', '(', ')', '*', '+', ',', '-', '.', '/',
    '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', ':', ';', '<', '=', '>', '?',
    0xA1, 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O',
    'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', 0xC4, 0xD6, 0xD1, 0xDC, 0xA7,
    0xBF, 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o',
    'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z', 0xE4, 0xF6, 0xF1, 0xFC, 0xE0,
};

static const uint16_t gsm_ext[][2] = {
    {0x0A, '\f'}, {0x14, '^'}, {0x28, '{'}, {0x29, '}'}, {0x2F, '\\'},
    {0x3C, '['}, {0x3D, '~'}, {0x3E, ']'}, {0x40, '|'}, {0x65, 0x20AC},
};

struct out {
    char *p, *end;
};

static void put(struct out *o, uint32_t c)
{
    char b[4];
    int n = c < 0x80 ? 1 : c < 0x800 ? 2 : 3;
    if (c >= 0xD800 && c < 0xE000)
        c = '?';
    if (n == 1) {
        b[0] = (char)c;
    } else if (n == 2) {
        b[0] = (char)(0xC0 | c >> 6);
        b[1] = (char)(0x80 | (c & 0x3F));
    } else {
        b[0] = (char)(0xE0 | c >> 12);
        b[1] = (char)(0x80 | (c >> 6 & 0x3F));
        b[2] = (char)(0x80 | (c & 0x3F));
    }
    if (o->end - o->p > n) {
        memcpy(o->p, b, (size_t)n);
        o->p += n;
        *o->p = 0;
    }
}

/* Septets of the default alphabet, one per byte. */
static void put_gsm(struct out *o, const uint8_t *s, int n)
{
    for (int i = 0; i < n; i++) {
        if (s[i] == 0xFF)
            break;
        if (s[i] == 0x1B && i + 1 < n) {
            uint32_t c = ' ';
            for (size_t k = 0; k < sizeof gsm_ext / sizeof *gsm_ext; k++)
                if (gsm_ext[k][0] == s[i + 1])
                    c = gsm_ext[k][1];
            put(o, c);
            i++;
            continue;
        }
        put(o, gsm[s[i] & 0x7F]);
    }
}

static void put_ucs2(struct out *o, const uint8_t *s, int n)
{
    for (int i = 0; i + 1 < n; i += 2)
        if (s[i] != 0xFF || s[i + 1] != 0xFF)
            put(o, (uint32_t)s[i] << 8 | s[i + 1]);
}

/* An alpha identifier: the default alphabet, or UCS2 in one of 102 221's three forms. */
static void alpha(const uint8_t *s, int n, char *text, size_t size)
{
    struct out o = {text, text + size};
    *text = 0;
    if (n <= 0)
        return;
    if (s[0] == 0x80) {
        put_ucs2(&o, s + 1, n - 1);
    } else if ((s[0] == 0x81 && n >= 3) || (s[0] == 0x82 && n >= 4)) {
        int count = s[1], at = s[0] == 0x81 ? 3 : 4;
        uint32_t base = s[0] == 0x81 ? (uint32_t)s[2] << 7 : (uint32_t)s[2] << 8 | s[3];
        for (int i = 0; i < count && at + i < n; i++) {
            uint8_t b = s[at + i];
            if (b & 0x80)
                put(&o, base + (b & 0x7F));
            else
                put_gsm(&o, &b, 1);
        }
    } else {
        put_gsm(&o, s, n);
    }
}

/* A text string: a data coding scheme byte, then packed septets, bytes or UCS2. */
static void text_string(const uint8_t *s, int n, char *text, size_t size)
{
    struct out o = {text, text + size};
    *text = 0;
    if (n <= 1)
        return;
    uint8_t dcs = s[0] & 0x0C;
    s++;
    n--;
    if (dcs == 0x08) {
        put_ucs2(&o, s, n);
    } else if (dcs == 0x04) {
        put_gsm(&o, s, n);
    } else {
        uint8_t septets[300];
        int count = LV_MIN(n * 8 / 7, (int)sizeof septets);
        for (int i = 0; i < count; i++) {
            int bit = i * 7, at = bit / 8, shift = bit % 8;
            int v = s[at] >> shift;
            if (shift > 1 && at + 1 < n)
                v |= s[at + 1] << (8 - shift);
            septets[i] = v & 0x7F;
        }
        if (count && n * 8 % 7 == 0 && septets[count - 1] == '\r')
            count--;
        put_gsm(&o, septets, count);
    }
}

static uint32_t next_char(const char **s)
{
    const uint8_t *p = (const uint8_t *)*s;
    uint32_t c = *p++;
    int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    c &= more == 3 ? 0x07 : more == 2 ? 0x0F : more == 1 ? 0x1F : 0x7F;
    while (more-- && (*p & 0xC0) == 0x80)
        c = c << 6 | (*p++ & 0x3F);
    *s = (const char *)p;
    return c;
}

/* An answer as a text string TLV, in the coding the command asked for. */
static int answer_text(const char *text, uint8_t qualifier, uint8_t *b, int room)
{
    uint8_t data[512];
    int n = 0;
    bool ucs2 = qualifier & 0x02, packed = !ucs2 && (qualifier & 0x08);
    for (const char *s = text; *s && n < (int)sizeof data - 2;) {
        uint32_t c = next_char(&s);
        if (ucs2) {
            if (c > 0xFFFF)
                c = '?';
            data[n++] = (uint8_t)(c >> 8);
            data[n++] = (uint8_t)c;
            continue;
        }
        int code = '?';
        for (int k = 0; k < 128; k++)
            if (gsm[k] == c && k != 0x1B)
                code = k;
        for (size_t k = 0; code == '?' && c != '?' && k < sizeof gsm_ext / sizeof *gsm_ext; k++)
            if (gsm_ext[k][1] == c) {
                data[n++] = 0x1B;
                code = gsm_ext[k][0];
            }
        data[n++] = (uint8_t)code;
    }
    if (packed) {
        int bytes = (n * 7 + 7) / 8;
        uint8_t p[512] = {0};
        for (int i = 0; i < n; i++) {
            int bit = i * 7, at = bit / 8, shift = bit % 8;
            p[at] |= (uint8_t)(data[i] << shift);
            if (shift > 1)
                p[at + 1] |= (uint8_t)(data[i] >> (8 - shift));
        }
        memcpy(data, p, (size_t)bytes);
        n = bytes;
    }
    int len = n + 1, at = 0;
    if (len + 3 > room)
        return 0;
    b[at++] = 0x8D;
    if (len > 127)
        b[at++] = 0x81;
    b[at++] = (uint8_t)len;
    b[at++] = ucs2 ? 0x08 : packed ? 0x00 : 0x04;
    memcpy(b + at, data, (size_t)n);
    return at + n;
}

/* ---------- proactive commands ---------- */

static bool decode(const uint8_t *b, int n, struct cmd *m)
{
    if (n < 2 || b[0] != 0xD0)
        return false;
    int i = 1, len = b[i++];
    if (len == 0x81 && i < n)
        len = b[i++];
    int end = LV_MIN(n, i + len);
    memset(m, 0, sizeof *m);
    m->max = 160;
    while (i + 2 <= end) {
        uint8_t tag = b[i++] & 0x7F;
        int l = b[i++];
        if (l == 0x81 && i < end)
            l = b[i++];
        if (i + l > end)
            break;
        const uint8_t *v = b + i;
        if (tag == 0x01 && l >= 3) {
            memcpy(m->details, v, 3);
        } else if (tag == 0x05) {
            alpha(v, l, m->title, sizeof m->title);
        } else if (tag == 0x0D) {
            text_string(v, l, m->text, sizeof m->text);
        } else if (tag == 0x0F && l >= 1 && m->count < ITEMS) {
            m->items[m->count].id = v[0];
            alpha(v + 1, l - 1, m->items[m->count].text, sizeof m->items[0].text);
            m->count++;
        } else if (tag == 0x11 && l >= 2) {
            m->min = v[0];
            m->max = v[1];
        } else if (tag == 0x17) {
            text_string(v, l, m->fill, sizeof m->fill);
        } else if (tag == 0x10 && l >= 1) {
            m->default_item = v[0];
        } else if (tag == 0x2B) {
            m->immediate = true;
        } else if (tag == 0x04 && l >= 2) {
            static const uint32_t unit[] = {60000, 1000, 100};
            m->duration = v[0] < 3 ? unit[v[0]] * v[1] : 0;
        }
        i += l;
    }
    return m->details[1] != 0;
}

static int unhex(const char *s, uint8_t *b, int room)
{
    int n = 0;
    while (s[0] && s[1] && n < room) {
        unsigned v;
        if (sscanf(s, "%2x", &v) != 1)
            break;
        b[n++] = (uint8_t)v;
        s += 2;
    }
    return n;
}

static void tohex(const uint8_t *b, int n, char *s)
{
    for (int i = 0; i < n; i++)
        sprintf(s + 2 * i, "%02X", b[i]);
    s[2 * n] = 0;
}

/* ---------- AT, one command at a time ---------- */

static void tick(lv_timer_t *t);

static void ticking(void)
{
    if (!ticker)
        ticker = lv_timer_create(tick, 50, NULL);
}

static void at_next(void)
{
    if (at.pid >= 0 || !at.count)
        return;
    int fds[2];
    if (pipe(fds))
        return;
    pid_t pid = fork();
    if (pid == 0) {
        dup2(fds[1], 1);
        close(fds[0]);
        close(fds[1]);
        setenv("MU300_SIM_SLOT", at.q[at.head].card ? "1" : "0", 1);
        execl(AT_HELPER, AT_HELPER, "-t", "10", at.q[at.head].cmd, (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    if (pid < 0) {
        close(fds[0]);
        return;
    }
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    at.pid = pid;
    at.fd = fds[0];
    at.len = 0;
    ticking();
}

/* done runs with the reply when the command ends in OK. */
static void at_call(int card, void (*done)(int card, const char *reply), const char *fmt, ...)
{
    if (at.count == (int)(sizeof at.q / sizeof *at.q))
        return;
    int slot = (at.head + at.count++) % (int)(sizeof at.q / sizeof *at.q);
    at.q[slot].card = card;
    at.q[slot].done = done;
    va_list args;
    va_start(args, fmt);
    vsnprintf(at.q[slot].cmd, sizeof at.q[slot].cmd, fmt, args);
    va_end(args);
    at_next();
}

#define at_send(card, ...) at_call(card, NULL, __VA_ARGS__)

static void at_poll(void)
{
    if (at.fd < 0)
        return;
    for (;;) {
        ssize_t n = read(at.fd, at.out + at.len, sizeof at.out - 1 - at.len);
        if (n > 0 && at.len + (size_t)n < sizeof at.out - 1) {
            at.len += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EAGAIN)
            return;
        if (n > 0)
            at.len += (size_t)n;
        break;
    }
    at.out[at.len] = 0;
    close(at.fd);
    at.fd = -1;
    int status = 0;
    waitpid(at.pid, &status, 0);
    at.pid = -1;
    int card = at.q[at.head].card;
    void (*done)(int, const char *) = at.q[at.head].done;
    const char *cmd = at.q[at.head].cmd;
    bool ok = WIFEXITED(status) && !WEXITSTATUS(status) && strstr(at.out, "OK");
    bool answer = strstr(cmd, "+SPUSATTERMINAL") || strstr(cmd, "+SPUSATENVECMD") || strstr(cmd, "+SPUSATCALLSETUP");
    if (answer && !ok)
        toast("%s", L("SIM 卡没有响应", "The SIM did not respond"));
    at.head = (at.head + 1) % (int)(sizeof at.q / sizeof *at.q);
    at.count--;
    if (ok && done)
        done(card, at.out);
    at_next();
}

/* ---------- answers ---------- */

static int terminal_response(const uint8_t details[3], uint8_t result, const uint8_t *extra, int extra_len,
                             uint8_t b[320])
{
    static const uint8_t head[] = {0x81, 3, 0, 0, 0, 0x82, 2, 0x82, 0x81, 0x83, 1, 0};
    int n = sizeof head;
    memcpy(b, head, sizeof head);
    memcpy(b + 2, details, 3);
    b[11] = result;
    if (extra_len > 0 && n + extra_len <= 320) {
        memcpy(b + n, extra, (size_t)extra_len);
        n += extra_len;
    }
    return n;
}

static void respond(int c, const uint8_t details[3], uint8_t result, const uint8_t *extra, int extra_len)
{
    uint8_t b[320];
    char hex[641];
    tohex(b, terminal_response(details, result, extra, extra_len, b), hex);
    at_send(c, "AT+SPUSATTERMINAL=\"%s\"", hex);
}

/* ---------- the command waiting for the user, kept across restarts ---------- */

static uint32_t boot_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_BOOTTIME, &t);
    return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

static void session_path(int c, char *path, size_t size)
{
    snprintf(path, size, SESSIONS "/%d", c);
}

/* "<kind> <since, boot ms> <hex>": P for a proactive command, C for call setup. */
static bool session_read(int c, char *kind, uint32_t *since, char *hex, size_t size)
{
    char path[32], format[32];
    session_path(c, path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    snprintf(format, sizeof format, "%%c %%u %%%zus", size - 1);
    bool ok = fscanf(f, format, kind, since, hex) == 3;
    fclose(f);
    return ok;
}

static void popup_close(int c)
{
    if (popup != c)
        return;
    popup = -1;
    if (dialog_is_open())
        dialog_close();
}

static void settle(int c)
{
    char path[32];
    popup_close(c);
    cards[c].pending = false;
    session_path(c, path, sizeof path);
    unlink(path);
}

/* The same command sent again, as the modem replays it, keeps its time. */
static void wait_for_user(int c, char kind, const uint8_t *b, int n)
{
    struct card *k = &cards[c];
    char hex[600], old[600], old_kind, path[32];
    uint32_t now = boot_ms(), since = now;
    tohex(b, LV_MIN(n, 299), hex);
    if (session_read(c, &old_kind, &since, old, sizeof old) && (old_kind != kind || strcmp(old, hex)))
        since = now;
    k->pending = true;
    k->since = lv_tick_get() - (now - since);
    mkdir(SESSIONS, 0700);
    session_path(c, path, sizeof path);
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "%c %u %s\n", kind, since, hex);
        fclose(f);
    }
}

static void answer(int c, uint8_t result, const uint8_t *extra, int extra_len)
{
    struct card *k = &cards[c];
    if (!k->pending)
        return;
    settle(c);
    respond(c, k->cmd.details, result, extra, extra_len);
    k->view = V_WAIT;
    k->since = lv_tick_get();
}

static void activate(int c);

/* A session restored before the menu was read reads it on its way back. */
static void back_to_menu(int c)
{
    struct card *k = &cards[c];
    settle(c);
    if (k->menu_known)
        k->view = V_MENU;
    else if (open_now)
        activate(c);
    else
        k->view = V_EMPTY;
}

/* ---------- what the user does, in the view or a popup ---------- */

static void choose(int c, uint8_t id)
{
    struct card *k = &cards[c];
    if (k->view == V_MENU) {
        at_send(c, "AT+SPUSATENVECMD=\"D307820201819001%02X\"", id);
        k->view = V_WAIT;
        k->since = lv_tick_get();
    } else {
        uint8_t extra[] = {0x90, 1, id};
        answer(c, RESULT_OK, extra, sizeof extra);
    }
}

static void acknowledge(int c)
{
    if (cards[c].pending)
        answer(c, RESULT_OK, NULL, 0);
    else
        back_to_menu(c);
}

static void yes_no(int c, bool yes)
{
    uint8_t extra[] = {0x8D, 2, 0x04, yes};
    answer(c, RESULT_OK, extra, sizeof extra);
}

static void call_setup(int c, bool accept)
{
    struct card *k = &cards[c];
    at_send(c, "AT+SPUSATCALLSETUP=%d", accept);
    settle(c);
    k->view = V_WAIT;
    k->since = lv_tick_get();
}

/* False at the menu, where Back leaves the toolkit. */
static bool go_back(int c)
{
    struct card *k = &cards[c];
    switch (k->view) {
    case V_SELECT:
    case V_INPUT:
    case V_YESNO:
    case V_TEXT:
        if (k->pending)
            answer(c, RESULT_BACK, NULL, 0);
        else
            back_to_menu(c);
        return true;
    case V_CALL:
        call_setup(c, false);
        return true;
    case V_WAIT:
        back_to_menu(c);
        return true;
    default:
        return false;
    }
}

/* -1 is Cancel; otherwise the item id, or 1 and 0 for yes and no. */
static void on_popup(lv_event_t *e)
{
    int c = popup, choice = (int)(intptr_t)lv_event_get_user_data(e);
    if (c < 0)
        return;
    popup = -1;
    dialog_close();
    if (choice < 0)
        go_back(c);
    else if (cards[c].view == V_SELECT)
        choose(c, (uint8_t)choice);
    else if (cards[c].view == V_YESNO)
        yes_no(c, choice);
    else if (cards[c].view == V_CALL)
        call_setup(c, true);
    else
        acknowledge(c);
}

static void show_popup(int c)
{
    const struct card *k = &cards[c];
    char title[64];
    if (*k->cmd.title)
        snprintf(title, sizeof title, "%s", k->cmd.title);
    else
        snprintf(title, sizeof title, "SIM %d", c + 1);
    const char *note = *k->cmd.text ? k->cmd.text
                       : k->view == V_CALL ? L("SIM 卡请求拨打电话", "The SIM asks to make a call") : NULL;
    dialog_open(title, note);
    popup = c;
    if (k->view == V_SELECT) {
        for (int i = 0; i < k->cmd.count; i++)
            dialog_button(k->cmd.items[i].text, on_popup, (void *)(intptr_t)k->cmd.items[i].id);
        dialog_button(L("取消", "Cancel"), on_popup, (void *)-1);
    } else if (k->view == V_YESNO) {
        dialog_button(L("是", "Yes"), on_popup, (void *)1);
        dialog_button(L("否", "No"), on_popup, (void *)0);
    } else if (k->view == V_CALL) {
        dialog_button(L("拨打", "Call"), on_popup, (void *)1);
        dialog_button(L("取消", "Cancel"), on_popup, (void *)-1);
    } else {
        dialog_button(L("确定", "OK"), on_popup, (void *)0);
        if (k->pending)
            dialog_button(L("返回", "Back"), on_popup, (void *)-1);
    }
}

/* ---------- the main menu, kept by ICCID ---------- */

static void cache_path(const struct card *k, char *path, size_t size)
{
    snprintf(path, size, CACHE "/%s", k->iccid);
}

static void cache_load(struct card *k)
{
    char path[96], hex[600] = "";
    cache_path(k, path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    size_t n = fread(hex, 1, sizeof hex - 1, f);
    fclose(f);
    hex[n] = 0;
    uint8_t b[300];
    struct cmd m;
    k->no_menu = !strcmp(hex, "none");
    if (decode(b, unhex(hex, b, sizeof b), &m) && m.details[1] == CMD_SET_UP_MENU && m.count) {
        k->menu = m;
        k->menu_known = true;
    }
}

/* The menu's bytes in hex, or "none" for a card found to offer none. */
static void cache_write(const struct card *k, const char *text)
{
    if (!*k->iccid)
        return;
    char path[96], old[600] = "";
    cache_path(k, path, sizeof path);
    FILE *f = fopen(path, "r");
    if (f) {
        old[fread(old, 1, sizeof old - 1, f)] = 0;
        fclose(f);
    }
    if (!strcmp(old, text))
        return;
    mkdir(CACHE, 0755);
    if ((f = fopen(path, "w"))) {
        fputs(text, f);
        fclose(f);
    }
}

static void cache_save(struct card *k, const uint8_t *b, int n)
{
    tohex(b, LV_MIN(n, 299), k->menu_hex);
    cache_write(k, k->menu_hex);
}

static void no_menu(struct card *k)
{
    k->menu_known = false;
    k->no_menu = true;
    snprintf(k->menu_hex, sizeof k->menu_hex, "none");
    cache_write(k, k->menu_hex);
}

/* ---------- URCs ---------- */

/* A command the SIM sends while the toolkit is closed wakes the screen and
 * pops up; one that needs typing opens the toolkit instead. Dismissed, it waits
 * in the toolkit until its time runs out. */
static void call_attention(int c)
{
    if (open_now) {
        current = c;
        return;
    }
    if (phone_call_active())
        return;
    if (app.blank)
        screen_set_blank(false);
    if (cards[c].view != V_INPUT) {
        show_popup(c);
    } else if (!lock_active()) {
        current = c;
        apps_open_toolkit();
    }
}

static void show_command(struct card *k)
{
    uint8_t type = k->cmd.details[1];
    k->auto_clear = false;
    k->view = type == CMD_SELECT_ITEM ? V_SELECT
              : type == CMD_DISPLAY_TEXT ? V_TEXT
              : type == CMD_GET_INKEY && (k->cmd.details[2] & 0x04) ? V_YESNO : V_INPUT;
    if (type == CMD_GET_INKEY && k->view == V_INPUT)
        k->cmd.min = k->cmd.max = 1;
}

static uint32_t answer_time(const struct card *k)
{
    if (k->cmd.duration)
        return k->cmd.duration;
    return k->cmd.details[1] == CMD_DISPLAY_TEXT ? TEXT_TIMEOUT : UI_TIMEOUT;
}

static void command(int c, const uint8_t *b, int n)
{
    struct card *k = &cards[c];
    struct cmd m;
    if (!decode(b, n, &m))
        return;
    uint8_t type = m.details[1], qualifier = m.details[2];
    if (type == CMD_SET_UP_MENU) {
        k->menu = m;
        k->menu_known = m.count > 0;
        k->no_menu = !k->menu_known;
        k->recovering = false;
        if (k->menu_known)
            cache_save(k, b, n);
        else
            no_menu(k);
        respond(c, m.details, RESULT_OK, NULL, 0);
        if (k->view == V_LOADING || k->view == V_EMPTY || k->view == V_MENU)
            k->view = k->menu_known ? V_MENU : V_EMPTY;
    } else if (type == CMD_SELECT_ITEM || type == CMD_GET_INPUT || type == CMD_GET_INKEY ||
               type == CMD_DISPLAY_TEXT) {
        k->cmd = m;
        show_command(k);
        /* Text that clears by itself, or asks for an immediate response, is
         * acknowledged at once; the first kind also goes away by itself. */
        if (type == CMD_DISPLAY_TEXT && (!(qualifier & 0x80) || m.immediate)) {
            settle(c);
            respond(c, m.details, RESULT_OK, NULL, 0);
            k->auto_clear = !(qualifier & 0x80);
            k->since = lv_tick_get();
        } else {
            wait_for_user(c, 'P', b, n);
        }
        call_attention(c);
    } else if (type == CMD_PLAY_TONE || type == CMD_SET_UP_IDLE_MODE_TEXT || type == CMD_LANGUAGE_NOTIFICATION ||
               type == CMD_SET_UP_EVENT_LIST || type == CMD_REFRESH) {
        respond(c, m.details, RESULT_OK, NULL, 0);
        if (type == CMD_PLAY_TONE && *m.title)
            toast("%s", m.title);
    } else if (type == CMD_PROVIDE_LOCAL_INFO && qualifier == 0x04) {
        uint8_t language[] = {0x2D, 2, app.en ? 'e' : 'z', app.en ? 'n' : 'h'};
        respond(c, m.details, RESULT_OK, language, sizeof language);
    } else {
        respond(c, m.details, RESULT_UNSUPPORTED, NULL, 0);
    }
    if (c == current)
        render();
}

static void urc(int c, const char *line)
{
    struct card *k = &cards[c];
    uint8_t b[300];
    const char *data;
    if ((data = strstr(line, "+SPUSATPROCMDIND:"))) {
        command(c, b, unhex(data + 17, b, sizeof b));
        return;
    }
    if ((data = strstr(line, "+SPUSATSETUPCALL:"))) {
        int n = unhex(data + 17, b, sizeof b);
        if (decode(b, n, &k->cmd)) {
            wait_for_user(c, 'C', b, n);
            k->view = V_CALL;
            call_attention(c);
        }
    } else if ((data = strstr(line, "+SPUSATDISPLAY:"))) {
        struct cmd m;
        if (decode(b, unhex(data + 15, b, sizeof b), &m) && *m.title)
            toast("%s", m.title);
    } else if (strstr(line, "+SPUSATENDSESSIONIND")) {
        if (k->view != V_TEXT || k->pending)
            back_to_menu(c);
    } else {
        return;
    }
    if (c == current)
        render();
}

/* What came, for which card, and when: the boot's commands can only be read here. */
static void log_urc(int c, const char *line)
{
    struct stat st;
    if (!stat(SESSIONS ".log", &st) && st.st_size > 65536)
        rename(SESSIONS ".log", SESSIONS ".log.old");
    FILE *f = fopen(SESSIONS ".log", "a");
    if (f) {
        fprintf(f, "%u card%d %s\n", boot_ms() / 1000, c + 1, line);
        fclose(f);
    }
}

static void ring_data(int ring, const uint8_t *b, int n)
{
    struct ring_line *r = &rings[ring == 3];
    for (int i = 0; i < n; i++) {
        if (b[i] == '\n') {
            r->line[r->len] = 0;
            int c = ring == 3;
            if (strstr(r->line, "+SPUSAT"))
                log_urc(c, r->line);
            urc(c, r->line);
            r->len = 0;
        } else if (b[i] != '\r' && r->len < sizeof r->line - 1) {
            r->line[r->len++] = (char)b[i];
        }
    }
}

/* "... ring=0 n=145 d={0x...,...}": up to 32 little-endian words of the read. */
static void trace_line(char *line)
{
    char *r = strstr(line, " ring="), *n = strstr(line, " n="), *d = strstr(line, " d={");
    if (!r || !n || !d)
        return;
    int ring = atoi(r + 6), count = LV_MIN(atoi(n + 3), 256);
    uint8_t b[256];
    char *s = d + 4;
    for (int w = 0; w < 32 && *s && *s != '}'; w++) {
        unsigned long long v = strtoull(s, &s, 16);
        for (int i = 0; i < 8; i++)
            b[w * 8 + i] = (uint8_t)(v >> (8 * i));
        if (*s == ',')
            s++;
    }
    ring_data(ring, b, count);
}

static void trace_poll(void)
{
    if (trace_fd < 0)
        return;
    for (;;) {
        ssize_t n = read(trace_fd, trace_buf + trace_len, sizeof trace_buf - 1 - trace_len);
        if (n <= 0)
            break;
        trace_len += (size_t)n;
        trace_buf[trace_len] = 0;
        char *start = trace_buf, *nl;
        while ((nl = strchr(start, '\n'))) {
            *nl = 0;
            trace_line(start);
            start = nl + 1;
        }
        trace_len -= (size_t)(start - trace_buf);
        memmove(trace_buf, start, trace_len);
        if (trace_len == sizeof trace_buf - 1)
            trace_len = 0;
    }
}

static bool write_file(const char *path, const char *text, int flags)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC | flags);
    if (fd < 0)
        return false;
    bool ok = write(fd, text, strlen(text)) == (ssize_t)strlen(text);
    close(fd);
    return ok;
}

/* The AT channel's SIPC endpoint: the spipe node labelled stty_nr, as
 * /sipc-virt/core@<dst>/channel@<channel>. */
static bool find_ring_owner(void)
{
    static const char base[] = "/proc/device-tree/sipc-virt";
    DIR *cores = opendir(base);
    if (!cores)
        return false;
    struct dirent *core;
    while (ring_dst < 0 && (core = readdir(cores))) {
        if (strncmp(core->d_name, "core@", 5))
            continue;
        char path[300];
        snprintf(path, sizeof path, "%s/%s", base, core->d_name);
        DIR *channels = opendir(path);
        struct dirent *ch;
        while (channels && ring_dst < 0 && (ch = readdir(channels))) {
            char label[32] = "", file[600];
            snprintf(file, sizeof file, "%s/%s/label", path, ch->d_name);
            FILE *f = strncmp(ch->d_name, "channel@", 8) ? NULL : fopen(file, "r");
            if (!f)
                continue;
            size_t n = fread(label, 1, sizeof label - 1, f);
            label[n] = 0;
            fclose(f);
            if (!strcmp(label, "stty_nr")) {
                ring_dst = (int)strtol(core->d_name + 5, NULL, 16);
                ring_ch = (int)strtol(ch->d_name + 8, NULL, 16);
            }
        }
        if (channels)
            closedir(channels);
    }
    closedir(cores);
    return ring_dst >= 0;
}

static void trace_close(void)
{
    if (trace_fd >= 0)
        close(trace_fd);
    trace_fd = -1;
    write_file(INSTANCE "/events/" PROBE "/enable", "0\n", O_TRUNC);
    rmdir(INSTANCE);
    write_file(TRACE "/kprobe_events", "-:" PROBE "\n", O_APPEND);
}

static bool trace_open(void)
{
    if (trace_fd >= 0)
        return true;
    if (ring_dst < 0 && !find_ring_owner())
        return false;
    if (access(TRACE "/events/" PROBE, F_OK))
        write_file(TRACE "/kprobe_events",
                   "r:" PROBE " sipc_core:sbuf_read dst=$arg1:u8 ch=$arg2:u8 ring=$arg3:u32 n=$retval:s32 "
                   "d=+0($arg4):x64[32]\n",
                   O_APPEND);
    char filter[96];
    snprintf(filter, sizeof filter, "n>0 && dst==%d && ch==%d && (ring==0 || ring==3)\n", ring_dst, ring_ch);
    if ((mkdir(INSTANCE, 0700) && errno != EEXIST) ||
        !write_file(INSTANCE "/events/" PROBE "/filter", filter, O_TRUNC) ||
        !write_file(INSTANCE "/events/" PROBE "/enable", "1\n", O_TRUNC) ||
        (trace_fd = open(INSTANCE "/trace_pipe", O_RDONLY | O_NONBLOCK | O_CLOEXEC)) < 0) {
        trace_close();
        return false;
    }
    trace_len = 0;
    for (int i = 0; i < 2; i++)
        rings[i].len = 0;
    return true;
}

/* ---------- getting the main menu ---------- */

/* The bytes of a +CSIM reply, status word included. */
static int csim_reply(const char *reply, uint8_t *b, int room)
{
    const char *s = strstr(reply, "+CSIM:");
    s = s ? strchr(s, ',') : NULL;
    if (!s)
        return 0;
    s += strspn(s, ", \"");
    return unhex(s, b, room);
}

static void on_profile(int c, const char *reply)
{
    const char *s = strstr(reply, "+SPUSATPROFILE:");
    if (s)
        sscanf(s + 15, "%79[0-9A-Fa-f]", cards[c].profile);
}

/* After a TERMINAL PROFILE the SIM opens with SET UP EVENT LIST, which comes
 * back in the reply itself and is answered the same way. */
static void on_terminal_profile(int c, const char *reply)
{
    uint8_t b[300], tr[320], apdu[330];
    int n = csim_reply(reply, b, sizeof b) - 2;
    struct cmd m;
    if (n <= 0 || !decode(b, n, &m))
        return;
    int len = terminal_response(m.details, m.details[1] == 0x05 ? RESULT_OK : RESULT_UNSUPPORTED, NULL, 0, tr);
    memcpy(apdu, (uint8_t[]){0x80, 0x14, 0x00, 0x00, (uint8_t)len}, 5);
    memcpy(apdu + 5, tr, (size_t)len);
    char hex[sizeof apdu * 2 + 1];
    tohex(apdu, len + 5, hex);
    at_send(c, "AT+CSIM=%d,\"%s\"", (int)strlen(hex), hex);
}

/* STATUS with the length the SIM asks for: its status word is where the modem
 * sees 91xx, a command waiting, and fetches it. */
static void on_status(int c, const char *reply)
{
    uint8_t b[300];
    int n = csim_reply(reply, b, sizeof b);
    if (n >= 2 && b[n - 2] == 0x6C)
        cards[c].status_le = b[n - 1];
}

static void terminal_profile(int c)
{
    struct card *k = &cards[c];
    size_t len = strlen(k->profile);
    if (!len)
        return;
    at_call(c, on_terminal_profile, "AT+CSIM=%d,\"80100000%02X%s\"", (int)len + 10, (int)len / 2, k->profile);
}

static void on_profile_then_terminal_profile(int c, const char *reply)
{
    on_profile(c, reply);
    terminal_profile(c);
}

/* The modem only replays the last command it forwarded; a fresh TERMINAL
 * PROFILE has the SIM send its menu again. */
static void recover(int c)
{
    struct card *k = &cards[c];
    k->recovering = true;
    k->view = V_LOADING;
    k->since = k->polled = lv_tick_get();
    if (*k->profile)
        terminal_profile(c);
    else
        at_call(c, on_profile_then_terminal_profile, "AT+SPUSATPROFILE?");
}

static void on_iccid(int c, const char *reply)
{
    struct card *k = &cards[c];
    const char *s = strstr(reply, "+CCID:");
    if (!s)
        return;
    s += 6;
    sscanf(s + strspn(s, " \""), "%23[0-9A-Fa-f]", k->iccid);
    if (*k->menu_hex)
        cache_write(k, k->menu_hex);
    if (k->menu_known || k->no_menu || !*k->iccid)
        return;
    cache_load(k);
    if (k->view != V_LOADING)
        return;
    if (k->menu_known || k->no_menu) {
        k->view = k->menu_known ? V_MENU : V_EMPTY;
        if (c == current)
            render();
    } else {
        k->since = lv_tick_get();
        at_call(c, on_profile, "AT+SPUSATPROFILE?");
    }
}

/* As Android shows the menu it was given at start, a card with a cached menu
 * (or none) asks for nothing; one without asks the modem to replay its last
 * command, which is often the menu, once its ICCID shows nothing is cached. */
static void activate(int c)
{
    struct card *k = &cards[c];
    k->view = k->menu_known ? V_MENU : k->no_menu ? V_EMPTY : V_LOADING;
    k->since = lv_tick_get();
    k->recovering = false;
    at_send(c, "AT+CUSATW=0,\"%s\"", te_profile);
    at_call(c, on_iccid, "AT+CCID");
}

/* ---------- time ---------- */

static bool in_session(const struct card *k)
{
    return k->pending || k->view == V_WAIT || k->view == V_TEXT;
}

static void expire(int c)
{
    struct card *k = &cards[c];
    uint32_t idle = lv_tick_elaps(k->since);
    if (k->pending && idle > answer_time(k)) {
        if (k->view == V_CALL) {
            at_send(c, "AT+SPUSATCALLSETUP=0");
            back_to_menu(c);
        } else {
            answer(c, RESULT_NO_RESPONSE, NULL, 0);
        }
    } else if (k->view == V_WAIT && idle > UI_TIMEOUT) {
        back_to_menu(c);
    } else if (k->view == V_TEXT && k->auto_clear && idle > (k->cmd.duration ? k->cmd.duration : TEXT_CLEAR_TIMEOUT)) {
        back_to_menu(c);
    } else if (k->view == V_LOADING && !k->recovering && idle > 3000) {
        recover(c);
    } else if (k->view == V_LOADING && k->recovering && idle > 20000) {
        k->recovering = false;
        no_menu(k);
        k->view = V_EMPTY;
    } else {
        if (k->recovering && !at.count && lv_tick_elaps(k->polled) > 2000) {
            k->polled = lv_tick_get();
            at_call(c, on_status, "AT+CSIM=10,\"80F20000%02X\"", k->status_le);
        }
        return;
    }
    if (c == current)
        render();
}

static bool present(int c);

/* The modem forwards nothing until told which commands the screen takes, and
 * holds what came before (a welcome text at SIM start) until told the toolkit
 * runs, as Android's RIL tells it at boot: each card present, once per run. */
static void tick(lv_timer_t *t)
{
    at_poll();
    trace_poll();
    for (int c = 0; c < 2; c++) {
        expire(c);
        if (!cards[c].registered && present(c)) {
            cards[c].registered = true;
            at_call(c, on_iccid, "AT+CCID");
            at_send(c, "AT+CUSATW=0,\"%s\"", te_profile);
            at_call(c, on_profile, "AT+SPUSATPROFILE?");
        }
    }
    lv_timer_set_period(t, at.pid >= 0 || open_now ? 50 : 250);
}

/* ---------- view ---------- */

static bool present(int c)
{
    const cJSON *sim;
    cJSON_ArrayForEach(sim, jget(app.status, "sims"))
        if ((int)jnum_or(sim, "slot", -1) == c)
            return jtrue(sim, "present");
    return false;
}

static void on_tab(lv_event_t *e)
{
    current = (int)(intptr_t)lv_event_get_user_data(e);
    render();
}

static bool inside(lv_obj_t *obj, lv_obj_t *parent)
{
    while (obj && obj != parent)
        obj = lv_obj_get_parent(obj);
    return obj != NULL;
}

static lv_obj_t *first_entry(lv_obj_t *parent)
{
    for (uint32_t i = 0; i < lv_obj_get_child_count(parent); i++) {
        lv_obj_t *child = lv_obj_get_child(parent, (int32_t)i);
        if (lv_obj_is_hidden(child))
            continue;
        if (lv_obj_get_group(child) == input_group())
            return child;
        if ((child = first_entry(child)))
            return child;
    }
    return NULL;
}

/* Left and Right only switch the card, on the switch; elsewhere they would
 * change the page in the middle of a SIM session. */
bool stk_key(const struct key_event *key)
{
    bool dual = present(0) && present(1);
    switch (key->kind) {
    case KEY_KIND_LEFT:
    case KEY_KIND_RIGHT:
        if (on_switcher && current != (key->kind == KEY_KIND_RIGHT)) {
            current = key->kind == KEY_KIND_RIGHT;
            render();
        }
        return true;
    case KEY_KIND_UP: {
        lv_obj_t *focused = lv_group_get_focused(input_group());
        if (on_switcher)
            return true;
        bool below_first = focused && lv_obj_has_state(focused, LV_STATE_FOCUS_KEY) && inside(focused, ui.body) &&
                           focused != first_entry(ui.body);
        if (!dual || below_first)
            return false;
        input_defocus();
        on_switcher = true;
        render();
        return true;
    }
    case KEY_KIND_DOWN: {
        if (!on_switcher)
            return false;
        on_switcher = false;
        render();
        lv_obj_t *first = first_entry(ui.body);
        if (first) {
            lv_group_focus_obj(first);
            lv_obj_add_state(first, LV_STATE_FOCUS_KEY);
        }
        return true;
    }
    case KEY_KIND_OK:
        return on_switcher;
    default:
        return false;
    }
}

static void on_item(lv_event_t *e)
{
    choose(current, (uint8_t)(uintptr_t)lv_event_get_user_data(e));
    render();
}

static void on_typed(lv_event_t *e)
{
    cards[current].since = lv_tick_get();
}

static void on_ok(lv_event_t *e)
{
    struct card *k = &cards[current];
    if (k->view == V_INPUT) {
        const char *text = lv_textarea_get_text(ui.field);
        int length = 0;
        for (const char *t = text; *t; length++)
            next_char(&t);
        if (length < k->cmd.min) {
            toast(L("至少输入 %d 个字符", "Enter at least %d characters"), k->cmd.min);
            return;
        }
        uint8_t extra[300];
        int n = answer_text(text, k->cmd.details[2], extra, sizeof extra);
        answer(current, RESULT_OK, extra, n);
    } else {
        acknowledge(current);
    }
    render();
}

static void on_yesno(lv_event_t *e)
{
    yes_no(current, lv_event_get_user_data(e) != NULL);
    render();
}

static void on_call(lv_event_t *e)
{
    call_setup(current, lv_event_get_user_data(e) != NULL);
    render();
}

static void on_retry(lv_event_t *e)
{
    recover(current);
    render();
}

static lv_obj_t *message(const char *text)
{
    lv_obj_t *card = ui_card(ui.body);
    lv_obj_t *label = ui_text(card, 14, false, theme.text);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(label, text);
    return card;
}

static void buttons(const char *ok, lv_event_cb_t cb, intptr_t ok_ctx, const char *no, intptr_t no_ctx)
{
    lv_obj_t *row = ui_box(ui.body, LV_FLEX_FLOW_ROW, 8);
    if (no)
        lv_obj_set_flex_grow(ui_button(row, no, cb, (void *)no_ctx), 1);
    lv_obj_t *b = ui_button(row, ok, cb, (void *)ok_ctx);
    ui_primary(b);
    lv_obj_set_flex_grow(b, 1);
}

static void render(void)
{
    if (!ui.root || !open_now)
        return;
    struct card *k = &cards[current];
    bool dual = present(0) && present(1);
    on_switcher &= dual;
    lv_obj_set_hidden(ui.switcher, !dual);
    for (int c = 0; c < 2; c++) {
        lv_obj_set_style_bg_opa(ui.tabs[c], c == current ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_state(ui.tabs[c], LV_STATE_FOCUS_KEY, on_switcher && c == current);
        lv_obj_set_style_text_color(lv_obj_get_child(ui.tabs[c], 0), c == current ? theme.accent : theme.dim, 0);
    }
    keyboard_close();
    lv_obj_clean(ui.body);
    ui.field = NULL;
    const struct cmd *m = k->view == V_MENU ? &k->menu : &k->cmd;
    if (k->view != V_LOADING && k->view != V_EMPTY && k->view != V_WAIT && *m->title)
        ui_heading(ui.body, m->title);
    switch (k->view) {
    case V_LOADING:
    case V_WAIT:
        message(k->view == V_LOADING ? L("正在读取 SIM 卡菜单…", "Reading the SIM menu…")
                                     : L("正在等待 SIM 卡…", "Waiting for the SIM…"));
        break;
    case V_EMPTY:
        message(present(current) ? L("SIM 卡没有提供应用菜单", "The SIM offers no menu")
                                 : L("未插入 SIM 卡", "No SIM card"));
        if (present(current))
            buttons(L("重新读取", "Retry"), on_retry, 0, NULL, 0);
        break;
    case V_MENU:
    case V_SELECT:
        for (int i = 0; i < m->count; i++) {
            lv_obj_t *row = ui_row(ui.body, NULL, m->items[i].text, NULL, ROW_CHEVRON, on_item,
                                   (void *)(uintptr_t)m->items[i].id);
            if (k->view == V_SELECT && m->default_item && m->items[i].id == m->default_item) {
                lv_group_focus_obj(row);
                lv_obj_add_state(row, LV_STATE_FOCUS_KEY);
            }
        }
        break;
    case V_TEXT:
        message(m->text);
        buttons(L("确定", "OK"), on_ok, 0, NULL, 0);
        break;
    case V_YESNO:
        message(m->text);
        buttons(L("是", "Yes"), on_yesno, 1, L("否", "No"), 0);
        break;
    case V_CALL:
        message(*m->text ? m->text : L("SIM 卡请求拨打电话", "The SIM asks to make a call"));
        buttons(L("拨打", "Call"), on_call, 1, L("取消", "Cancel"), 0);
        break;
    case V_INPUT: {
        if (*m->text)
            message(m->text);
        ui.field = lv_textarea_create(ui.body);
        lv_obj_set_width(ui.field, LV_PCT(100));
        lv_textarea_set_one_line(ui.field, true);
        lv_textarea_set_max_length(ui.field, (uint32_t)LV_MAX(m->max, 1));
        lv_textarea_set_password_mode(ui.field, m->details[2] & 0x04);
        if (!(m->details[2] & 0x01))
            lv_textarea_set_accepted_chars(ui.field, "0123456789*#+");
        lv_textarea_set_text(ui.field, m->fill);
        lv_obj_set_style_text_font(ui.field, F(14), 0);
        lv_obj_set_style_text_color(ui.field, theme.text, 0);
        lv_obj_set_style_bg_color(ui.field, theme.surface, 0);
        lv_obj_set_style_bg_opa(ui.field, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(ui.field, theme.line, 0);
        lv_obj_set_style_border_color(ui.field, theme.accent, LV_STATE_FOCUSED);
        lv_obj_set_style_border_width(ui.field, 1, 0);
        lv_obj_set_style_radius(ui.field, S(10), 0);
        lv_obj_set_style_pad_all(ui.field, S(8), 0);
        lv_obj_add_event_cb(ui.field, on_typed, LV_EVENT_VALUE_CHANGED, NULL);
        keyboard_bind(ui.field);
        ui_focusable(ui.field);
        buttons(L("确定", "OK"), on_ok, 0, NULL, 0);
        break;
    }
    }
}

/* A command the last run left waiting goes on with the time it had. */
static void session_restore(int c)
{
    struct card *k = &cards[c];
    char kind, hex[600];
    uint32_t since;
    uint8_t b[300];
    if (!session_read(c, &kind, &since, hex, sizeof hex) || !decode(b, unhex(hex, b, sizeof b), &k->cmd) ||
        trace_fd < 0) {
        settle(c);
        return;
    }
    if (kind == 'C')
        k->view = V_CALL;
    else
        show_command(k);
    k->pending = true;
    k->since = lv_tick_get() - (boot_ms() - since);
}

static bool command_ok(int fd, const char *command, int timeout_ms)
{
    char reply[256];
    size_t used = 0;
    if (write(fd, command, strlen(command)) != (ssize_t)strlen(command))
        return false;
    for (int waited = 0; waited < timeout_ms && used + 1 < sizeof reply; waited += 100) {
        struct pollfd p = {fd, POLLIN, 0};
        if (poll(&p, 1, 100) <= 0)
            continue;
        ssize_t n = read(fd, reply + used, sizeof reply - 1 - used);
        if (n > 0) {
            used += (size_t)n;
            reply[used] = 0;
            if (strstr(reply, "OK\r\n"))
                return true;
            if (strstr(reply, "ERROR"))
                return false;
        }
    }
    return false;
}

/* At its default of 1 the modem sends both cards' URCs on ring 0 with nothing
 * to tell them apart; at 0, which Android's RIL sets first at its handshake,
 * card 2's come on ring 3. The SIMs' toolkits queue what they send on the
 * mapping of the moment, so this runs at boot, as soon as the CP answers and
 * before ModemManager powers the SIMs, on a channel nothing else uses. */
int stk_map_channels(void)
{
    static const char port[] = "/dev/stty_nr5";
    for (int tries = 0; tries < 600; tries++) {
        int fd = open(port, O_RDWR | O_NONBLOCK | O_CLOEXEC | O_NOCTTY);
        if (fd >= 0) {
            struct termios t;
            if (!tcgetattr(fd, &t)) {
                cfmakeraw(&t);
                tcsetattr(fd, TCSANOW, &t);
            }
            bool ready = command_ok(fd, "AT\r", 1000);
            bool mapped = ready && command_ok(fd, "AT+SMMSWAP=0\r", 3000);
            close(fd);
            if (ready) {
                fprintf(stderr, "dual-SIM channel mapping %s\n", mapped ? "set" : "refused");
                return mapped ? 0 : 1;
            }
        }
        usleep(200000);
    }
    fprintf(stderr, "%s: no answer\n", port);
    return 1;
}

void stk_create(lv_obj_t *parent)
{
    memset(&ui, 0, sizeof ui);
    ui.root = ui_box(parent, LV_FLEX_FLOW_COLUMN, ui_stack_gap());
    /* The phone dialer's SIM switch; Left and Right pick the card. */
    lv_obj_t *tabs = ui.switcher = ui_box(ui.root, LV_FLEX_FLOW_ROW, 0);
    lv_obj_set_style_bg_color(tabs, theme.surface, 0);
    lv_obj_set_style_bg_opa(tabs, theme.surface_opa, 0);
    lv_obj_set_style_radius(tabs, S(18), 0);
    lv_obj_set_style_pad_all(tabs, S(3), 0);
    for (int c = 0; c < 2; c++) {
        ui.tabs[c] = ui_button(tabs, c ? "SIM 2" : "SIM 1", on_tab, (void *)(intptr_t)c);
        lv_group_remove_obj(ui.tabs[c]);
        lv_obj_set_flex_grow(ui.tabs[c], 1);
        lv_obj_set_style_pad_ver(ui.tabs[c], S(5), 0);
        lv_obj_set_style_radius(ui.tabs[c], S(15), 0);
        lv_obj_set_style_bg_color(ui.tabs[c], theme.active, 0);
        lv_obj_set_style_text_font(lv_obj_get_child(ui.tabs[c], 0), F(12), 0);
    }
    ui.body = ui_box(ui.root, LV_FLEX_FLOW_COLUMN, ui_stack_gap());
    ticking();
    trace_open();
    for (int c = 0; c < 2; c++)
        if (!in_session(&cards[c]))
            session_restore(c);
    render();
}

/* A session left open resumes where it was, as long as its time has not run out. */
void stk_open(void)
{
    if (popup >= 0)
        popup_close(popup);
    open_now = true;
    on_switcher = false;
    ticking();
    if (!trace_open()) {
        for (int c = 0; c < 2; c++)
            cards[c].view = V_EMPTY;
        toast("%s", L("无法读取 SIM 卡应用", "Cannot reach the SIM toolkit"));
        render();
        return;
    }
    if (!present(current))
        current = present(0) || !present(1) ? 0 : 1;
    for (int c = 0; c < 2; c++) {
        if (!present(c))
            cards[c].view = V_EMPTY;
        else if (!in_session(&cards[c]))
            activate(c);
    }
    render();
}

void stk_close(void)
{
    if (!open_now)
        return;
    open_now = false;
    keyboard_close();
}

bool stk_back(void)
{
    if (!go_back(current))
        return false;
    render();
    return true;
}
