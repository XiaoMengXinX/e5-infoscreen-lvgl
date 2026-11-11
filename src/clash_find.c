/* Plugins (OpenClash, nikki, ShellCrash, homeproxy…) start the core with the
 * configuration they generate, so the running process names the file that
 * holds the controller's address and secret. Clash reads its flags Go's way and
 * resolves relative paths against its working directory; sing-box takes -c
 * files and -C directories of JSON. */
#include "clash_find.h"

#include <ctype.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cJSON.h"
#include "util.h"

#define MAX_ARGS 32

struct process {
    int pid, argc;
    char *argv[MAX_ARGS];
    char buf[4096];
};

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return NULL;
    size_t cap = 65536, len = 0, n;
    char *text = malloc(cap);
    while (text && (n = fread(text + len, 1, cap - len - 1, f)) > 0) {
        len += n;
        if (cap - len < 4096) {
            char *more = realloc(text, cap *= 2);
            if (!more)
                free(text);
            text = more;
        }
    }
    fclose(f);
    if (text)
        text[len] = 0;
    return text;
}

static bool read_process(int pid, struct process *p)
{
    char path[48];
    snprintf(path, sizeof path, "/proc/%d/cmdline", pid);
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    size_t len = fread(p->buf, 1, sizeof p->buf - 1, f);
    fclose(f);
    p->buf[len] = 0;
    p->pid = pid;
    p->argc = 0;
    for (size_t i = 0; i < len && p->argc < MAX_ARGS; i += strlen(p->buf + i) + 1)
        p->argv[p->argc++] = p->buf + i;
    return p->argc > 0;
}

static bool flag_name(const char *arg, const char *name, const char **rest)
{
    size_t len = strlen(name);
    if (strncmp(arg, name, len) || (arg[len] && arg[len] != '='))
        return false;
    *rest = arg + len;
    return true;
}

/* The next value, from argument *from on, of -name or --name (or the long
 * name), given as the next argument or after "=". */
static const char *flag(const struct process *p, const char *name, const char *alias, int *from)
{
    for (int i = *from; i < p->argc; i++) {
        const char *arg = p->argv[i], *rest;
        if (*arg != '-')
            continue;
        arg += arg[1] == '-' ? 2 : 1;
        if (!flag_name(arg, name, &rest) && !(alias && flag_name(arg, alias, &rest)))
            continue;
        if (*rest == '=') {
            *from = i + 1;
            return rest + 1;
        }
        if (i + 1 < p->argc) {
            *from = i + 2;
            return p->argv[i + 1];
        }
    }
    return NULL;
}

static const char *last_flag(const struct process *p, const char *name)
{
    const char *value = NULL, *next;
    for (int from = 0; (next = flag(p, name, NULL, &from));)
        value = next;
    return value;
}

static bool has_arg(const struct process *p, const char *arg)
{
    for (int i = 1; i < p->argc; i++)
        if (!strcmp(p->argv[i], arg))
            return true;
    return false;
}

static void resolve(const struct process *p, const char *path, char *out, size_t size)
{
    if (*path == '/')
        snprintf(out, size, "%s", path);
    else
        snprintf(out, size, "/proc/%d/cwd/%s", p->pid, path);
}

/* "host:port", the host bare, bracketed for IPv6, or empty for every address. */
static void read_address(const char *address, struct clash_core *core)
{
    const char *colon = strrchr(address, ':');
    int port = colon ? atoi(colon + 1) : 0;
    core->port = port > 0 && port <= 65535 ? port : 0;
    if (!core->port)
        return;
    size_t len = (size_t)(colon - address);
    if (len >= 2 && address[0] == '[' && address[len - 1] == ']')
        address++, len -= 2;
    snprintf(core->host, sizeof core->host, "%.*s", (int)len, address);
    static const char *const local[] = {"", "*", "0.0.0.0", "::", "127.0.0.1", "::1", "localhost"};
    for (size_t i = 0; i < sizeof local / sizeof *local; i++)
        if (!strcmp(core->host, local[i]))
            *core->host = 0;
}

/* A top-level "key: value" line of YAML, without its quotes or comment. */
static bool yaml_value(const char *text, const char *key, char *out, size_t size)
{
    size_t len = strlen(key);
    for (const char *line = text; line; line = strchr(line, '\n'), line = line ? line + 1 : NULL) {
        if (strncmp(line, key, len) || line[len] != ':')
            continue;
        const char *v = line + len + 1;
        v += strspn(v, " \t");
        const char *end = v + strcspn(v, "\r\n");
        if (*v == '"' || *v == '\'') {
            const char *close = memchr(v + 1, *v, (size_t)(end - v - 1));
            if (!close)
                return false;
            end = close;
            v++;
        } else {
            for (const char *c = v; c < end; c++)
                if (*c == '#' && (c == v || isspace((unsigned char)c[-1]))) {
                    end = c;
                    break;
                }
            while (end > v && isspace((unsigned char)end[-1]))
                end--;
        }
        snprintf(out, size, "%.*s", (int)(end - v), v);
        return true;
    }
    return false;
}

/* False when the process names no configuration or controller, as a core does. */
static bool from_clash(const struct process *p, struct clash_core *core)
{
    char path[512], address[128] = "";
    const char *file = last_flag(p, "f"), *dir = last_flag(p, "d");
    if (file) {
        resolve(p, file, path, sizeof path);
    } else if (dir) {
        char base[480];
        resolve(p, dir, base, sizeof base);
        snprintf(path, sizeof path, "%s/config.yaml", base);
    }
    char *text = file || dir ? slurp(path) : NULL;
    if (text) {
        yaml_value(text, "external-controller", address, sizeof address);
        yaml_value(text, "secret", core->secret, sizeof core->secret);
        free(text);
    }
    /* The command line's own controller and secret win over the file's. */
    const char *control = last_flag(p, "ext-ctl"), *secret = last_flag(p, "secret");
    if (control)
        snprintf(address, sizeof address, "%s", control);
    if (secret)
        snprintf(core->secret, sizeof core->secret, "%s", secret);
    read_address(address, core);
    return file || dir || control;
}

/* True once a file has the Clash API. */
static bool singbox_file(const char *path, struct clash_core *core)
{
    char *text = slurp(path);
    cJSON *json = text ? cJSON_Parse(text) : NULL;
    free(text);
    const cJSON *api = jget(json, "experimental.clash_api");
    bool found = cJSON_IsObject(api);
    if (found) {
        read_address(jstr_or(api, "external_controller", ""), core);
        snprintf(core->secret, sizeof core->secret, "%s", jstr_or(api, "secret", ""));
    }
    cJSON_Delete(json);
    return found;
}

static void from_singbox(const struct process *p, struct clash_core *core)
{
    char path[512];
    const char *value;
    bool any = false;
    for (int from = 0; (value = flag(p, "c", "config", &from));) {
        any = true;
        resolve(p, value, path, sizeof path);
        if (singbox_file(path, core))
            return;
    }
    for (int from = 0; (value = flag(p, "C", "config-directory", &from));) {
        any = true;
        char dir_path[400];
        resolve(p, value, dir_path, sizeof dir_path);
        DIR *dir = opendir(dir_path);
        struct dirent *entry;
        while (dir && (entry = readdir(dir))) {
            size_t len = strlen(entry->d_name);
            if (len < 5 || strcmp(entry->d_name + len - 5, ".json"))
                continue;
            snprintf(path, sizeof path, "%s/%s", dir_path, entry->d_name);
            if (singbox_file(path, core)) {
                closedir(dir);
                return;
            }
        }
        if (dir)
            closedir(dir);
    }
    if (!any) {
        resolve(p, "config.json", path, sizeof path);
        singbox_file(path, core);
    }
}

bool clash_process(int pid, double *memory, double *uptime)
{
    char path[48], line[128];
    long rss = -1;
    snprintf(path, sizeof path, "/proc/%d/status", pid);
    FILE *f = fopen(path, "r");
    while (f && fgets(line, sizeof line, f))
        if (sscanf(line, "VmRSS: %ld kB", &rss) == 1)
            break;
    if (f)
        fclose(f);
    /* The start time is field 22 of stat, after the parenthesized name. */
    snprintf(path, sizeof path, "/proc/%d/stat", pid);
    char *stat = slurp(path), *rest = stat ? strrchr(stat, ')') : NULL;
    unsigned long long start = 0;
    bool started = rest && sscanf(rest + 2,
                                  "%*c %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s "
                                  "%llu",
                                  &start) == 1;
    free(stat);
    char *boot = slurp("/proc/uptime");
    double since_boot = boot ? atof(boot) : 0;
    free(boot);
    if (rss < 0 || !started || !since_boot)
        return false;
    *memory = (double)rss * 1024;
    *uptime = since_boot - (double)start / (double)sysconf(_SC_CLK_TCK);
    return true;
}

int clash_find(struct clash_core *cores, int max)
{
    DIR *proc = opendir("/proc");
    if (!proc)
        return 0;
    int n = 0;
    struct dirent *entry;
    struct process p;
    while (n < max && (entry = readdir(proc))) {
        int pid = atoi(entry->d_name);
        if (pid <= 0 || !read_process(pid, &p))
            continue;
        const char *name = strrchr(p.argv[0], '/');
        name = name ? name + 1 : p.argv[0];
        /* ShellCrash runs either core as CrashCore; sing-box takes the run command. */
        bool singbox = (strstr(name, "sing-box") || !strcmp(name, "CrashCore")) && has_arg(&p, "run");
        bool clash = !singbox && (strstr(name, "mihomo") || strstr(name, "clash") || !strcmp(name, "CrashCore"));
        struct clash_core *core = &cores[n];
        memset(core, 0, sizeof *core);
        core->pid = pid;
        snprintf(core->name, sizeof core->name, "%s", singbox ? "sing-box" : name);
        if (singbox)
            from_singbox(&p, core);
        else if (!clash || !from_clash(&p, core))
            continue;
        n++;
    }
    closedir(proc);
    return n;
}
