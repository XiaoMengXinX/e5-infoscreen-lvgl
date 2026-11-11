/* The screen's own update, from the GitHub release of this repository. The
 * release carries version.json (build timestamp, commit and the binary's
 * SHA-256); the binary carries the same timestamp and commit, so a newer
 * timestamp alone decides whether there is an update. */
#include "update.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "app.h"
#include "build_version.h"
#include "input.h"
#include "install.h"
#include "theme.h"
#include "ui.h"
#include "util.h"

#define RUN_DIR "/tmp/run/e5-infoscreen"
#define STATE_FILE RUN_DIR "/update-state"
#define BACKUP_FILE "/etc/e5-infoscreen/ui.backup"
#define AUTO_OPTION "ui_update_auto"
#define REPO "XiaoMengXinX/e5-infoscreen-lvgl"
#define ASSET "e5-infoscreen-ui"
#define CHECK_TIMEOUT 300000
#define AUTO_INTERVAL 43200000
#define AUTO_RETRY 21600000

#define FETCH_FN \
    "fetch() {\n" \
    "  if command -v curl >/dev/null; then\n" \
    "    [ -n \"$3\" ] && quiet=-# || quiet=-sS\n" \
    "    curl -fL $quiet --connect-timeout 10 --max-time \"${4:-300}\" --write-out \"$http_format\" -o \"$2\" \"$1\"\n" \
    "  else\n" \
    "    [ -n \"$3\" ] && quiet= || quiet=-q\n" \
    "    timeout \"${4:-300}\" uclient-fetch $quiet -T 10 -O \"$2\" \"$1\"\n" \
    "  fi\n" \
    "}\n"

enum check_status { CHECK_NONE, CHECK_NEWER, CHECK_CURRENT, CHECK_ERROR };

struct result {
    enum check_status status;
    long long build;
    char commit[64], sha[80], tag[80], error[32];
};

static struct result latest;
static pid_t check_pid = -1;
static lv_timer_t *auto_timer, *prompt_timer;
static lv_obj_t *check_step;
static uint32_t check_started;
static bool check_interactive, prompt_pending;
static char check_state[160];
static unsigned check_sequence;

static void start_apply(void);
static void prompt_update(void);
static void auto_schedule(uint32_t ms);

static void copy_field(char *out, size_t size, const char *value)
{
    size_t n = strlen(value);
    if (n >= size)
        n = size - 1;
    memcpy(out, value, n);
    out[n] = 0;
}

static bool valid_sha(const char *sha)
{
    if (strlen(sha) != 64)
        return false;
    for (const char *p = sha; *p; p++)
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')))
            return false;
    return true;
}

static bool valid_tag(const char *tag)
{
    for (const char *p = tag; *p; p++)
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              *p == '.' || *p == '_' || *p == '-'))
            return false;
    return true;
}

static void lowercase(char *text)
{
    for (; *text; text++)
        if (*text >= 'A' && *text <= 'Z')
            *text = (char)(*text - 'A' + 'a');
}

static bool read_result(const char *path, struct result *r)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    memset(r, 0, sizeof *r);
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *value = strchr(line, '=');
        if (!value)
            continue;
        *value++ = 0;
        value[strcspn(value, "\r\n")] = 0;
        if (!strcmp(line, "status")) {
            if (!strcmp(value, "newer"))
                r->status = CHECK_NEWER;
            else if (!strcmp(value, "current"))
                r->status = CHECK_CURRENT;
            else if (!strcmp(value, "error"))
                r->status = CHECK_ERROR;
        } else if (!strcmp(line, "build")) {
            r->build = strtoll(value, NULL, 10);
        } else if (!strcmp(line, "commit")) {
            copy_field(r->commit, sizeof r->commit, value);
        } else if (!strcmp(line, "sha256")) {
            copy_field(r->sha, sizeof r->sha, value);
        } else if (!strcmp(line, "tag")) {
            copy_field(r->tag, sizeof r->tag, value);
        } else if (!strcmp(line, "error")) {
            copy_field(r->error, sizeof r->error, value);
        }
    }
    fclose(f);
    return r->status != CHECK_NONE;
}

static void reap_check(void)
{
    if (check_pid < 0)
        return;
    int status;
    while (waitpid(check_pid, &status, 0) < 0 && errno == EINTR) {
    }
    check_pid = -1;
}

static void format_time(long long stamp, char *out, size_t size)
{
    if (stamp <= 0) {
        snprintf(out, size, "%s", L("未知", "Unknown"));
        return;
    }
    struct local_time t = local_at((double)stamp);
    snprintf(out, size, "%04d-%02d-%02d %02d:%02d", t.year, t.month, t.day, t.hour, t.minute);
}

bool update_auto_enabled(void)
{
    char value[8];
    screen_option(AUTO_OPTION, value, sizeof value);
    return strcmp(value, "0") != 0;
}

static void auto_schedule(uint32_t ms)
{
    if (!auto_timer || !update_auto_enabled())
        return;
    lv_timer_set_period(auto_timer, ms);
    lv_timer_reset(auto_timer);
}

static bool can_prompt(void)
{
    return !app.blank && !dialog_is_open() && !lock_active() && !phone_is_open() && !power_menu_is_open() &&
           !shell_overlay_open() && input_touch_count() == 0;
}

static const char *check_error(const char *error)
{
    if (!strcmp(error, "not_found"))
        return L("更新信息不存在（HTTP 404）", "Update information not found (HTTP 404)");
    if (!strcmp(error, "timeout"))
        return L("检查更新超时", "The update check timed out");
    if (!strcmp(error, "invalid"))
        return L("更新信息不完整", "The update details are incomplete");
    return L("无法连接 GitHub", "Cannot reach GitHub");
}

/* The update script is a child process; it writes one key=value block when done
 * and the poller below reads it. The latest version.json is tried through the
 * GitHub proxies the speed test uses; the release feed names the tag when the
 * redirect cannot be followed. */
static const char check_script[] =
    "run=/tmp/run/e5-infoscreen\n"
    "state=\"$2\"\n"
    "meta=$state.meta\n"
    "feed=$state.atom\n"
    "http_format='%{http_code}'\n"
    "error=network\n"
    "mkdir -p \"$run\"\n"
    FETCH_FN
    "progress() {\n"
    "  host=${1#https://}\n"
    "  host=${host%%/*}\n"
    "  echo \"$2 $host\" > \"$state.progress\"\n"
    "}\n"
    "fetch_meta() {\n"
    "  base=$1\n"
    "  build=\n"
    "  sum=\n"
    "  commit=\n"
    "  for mirror in https://gh-proxy.org/ https://cdn.gh-proxy.org/ https://ghproxy.net/ ''; do\n"
    "    rm -f \"$meta\"\n"
    "    progress \"$mirror$base/version.json\" manifest\n"
    "    if response=$(fetch \"$mirror$base/version.json\" \"$meta\" '' 20 2>&1); then\n"
    "      error=invalid\n"
    "    else\n"
    "      case $response in *404|*'HTTP error 404'*) error=not_found; return 1 ;; esac\n"
    "      continue\n"
    "    fi\n"
    "    build=$(jsonfilter -i \"$meta\" -e @.build 2>/dev/null)\n"
    "    sum=$(jsonfilter -i \"$meta\" -e @.sha256 2>/dev/null)\n"
    "    commit=$(jsonfilter -i \"$meta\" -e @.commit 2>/dev/null)\n"
    "    [ -n \"$build\" ] && [ -n \"$sum\" ] && return 0\n"
    "  done\n"
    "  build=\n"
    "  sum=\n"
    "  commit=\n"
    "  return 1\n"
    "}\n"
    "resolve_tag() {\n"
    "  tag=\n"
    "  for mirror in https://gh-proxy.org/ https://cdn.gh-proxy.org/ https://ghproxy.net/ ''; do\n"
    "    rm -f \"$feed\"\n"
    "    progress \"${mirror}https://github.com/" REPO "/releases.atom\" release\n"
    "    fetch \"${mirror}https://github.com/" REPO "/releases.atom\" \"$feed\" '' 20 || continue\n"
    "    tag=$(sed -n 's|.*/releases/tag/\\([^\"]*\\)\".*|\\1|p' \"$feed\" | head -n 1)\n"
    "    [ -n \"$tag\" ] && return 0\n"
    "  done\n"
    "  return 1\n"
    "}\n"
    "tag=\n"
    "resolve_tag || true\n"
    "if [ -n \"$tag\" ]; then\n"
    "  base=https://github.com/" REPO "/releases/download/$tag\n"
    "else\n"
    "  base=https://github.com/" REPO "/releases/latest/download\n"
    "fi\n"
    "fetch_meta \"$base\" || true\n"
    "case $build in\n"
    "  ''|*[!0-9]*) status=error ;;\n"
    "  *) if [ \"$build\" -gt \"$1\" ]; then status=newer; else status=current; fi ;;\n"
    "esac\n"
    "tmp=$state.part\n"
    "{\n"
    "  echo \"status=$status\"\n"
    "  if [ \"$status\" = newer ]; then\n"
    "    echo \"build=$build\"\n"
    "    echo \"commit=$commit\"\n"
    "    echo \"sha256=$sum\"\n"
    "    echo \"tag=$tag\"\n"
    "  elif [ \"$status\" = error ]; then\n"
    "    echo \"error=$error\"\n"
    "  fi\n"
    "} > \"$tmp\" && mv \"$tmp\" \"$state\"\n";

static const char apply_script_fmt[] =
    "echo @check\n"
    "release=%s\n"
    "sha=%s\n"
    "name=" ASSET "\n"
    "dir=/usr/libexec/e5-infoscreen\n"
    FETCH_FN
    "for mirror in https://gh-proxy.org/ https://cdn.gh-proxy.org/ https://ghproxy.net/ ''; do\n"
    "  host=${mirror#https://}\n"
    "  host=${host%%/}\n"
    "  echo \"@download %s ${host:-github.com}\"\n"
    "  rm -f \"$dir/ui.new\"\n"
    "  fetch \"$mirror$release/$name\" \"$dir/ui.new\" progress || continue\n"
    "  [ \"$(sha256sum \"$dir/ui.new\" | cut -d' ' -f1)\" = \"$sha\" ] || { rm -f \"$dir/ui.new\"; continue; }\n"
    "  echo @install\n"
    "  command -v setsid >/dev/null || exit 1\n"
    "  cat > \"$dir/ui.install\" <<'EOS'\n"
    "#!/bin/sh\n"
    "dir=/usr/libexec/e5-infoscreen\n"
    "bak=/etc/e5-infoscreen/ui.backup\n"
    "new=$dir/ui.new\n"
    "sha=$1\n"
    "[ -f \"$new\" ] || exit 1\n"
    "[ \"$(sha256sum \"$new\" | cut -d' ' -f1)\" = \"$sha\" ] || exit 1\n"
    "mkdir -p /etc/e5-infoscreen || exit 1\n"
    "if [ -f \"$dir/ui\" ]; then\n"
    "  cp -p \"$dir/ui\" \"$bak.new\" || exit 1\n"
    "  mv \"$bak.new\" \"$bak\" || exit 1\n"
    "fi\n"
    "chmod 755 \"$new\" && mv \"$new\" \"$dir/ui\" || exit 1\n"
    "( sleep 1; /etc/init.d/e5-infoscreen restart ) >/dev/null 2>&1 &\n"
    "rm -f \"$0\"\n"
    "exit 0\n"
    "EOS\n"
    "  chmod 755 \"$dir/ui.install\" || exit 1\n"
    "  setsid /bin/sh \"$dir/ui.install\" \"$sha\" >/dev/null 2>&1 < /dev/null &\n"
    "  wait \"$!\"\n"
    "  exit \"$?\"\n"
    "done\n"
    "rm -f \"$dir/ui.new\"\n"
    "exit 1\n";

static const char rollback_script[] =
    "echo @install\n"
    "dir=/usr/libexec/e5-infoscreen\n"
    "bak=/etc/e5-infoscreen/ui.backup\n"
    "[ -f \"$bak\" ] || exit 1\n"
    "cp -p \"$bak\" \"$dir/ui.new\" || exit 1\n"
    "chmod 755 \"$dir/ui.new\" && mv \"$dir/ui.new\" \"$dir/ui\" || exit 1\n"
    "rm -f \"$bak\"\n"
    "( sleep 1; /etc/init.d/e5-infoscreen restart ) >/dev/null 2>&1 &\n"
    "exit 0\n";

static void done_noop(void)
{
}

static void start_apply(void)
{
    if (install_running()) {
        toast("%s", L("正在更新，请稍候", "An update is running"));
        return;
    }
    char release[160];
    if (*latest.tag)
        snprintf(release, sizeof release, "https://github.com/" REPO "/releases/download/%.79s", latest.tag);
    else
        snprintf(release, sizeof release, "https://github.com/" REPO "/releases/latest/download");
    char script[4096];
    snprintf(script, sizeof script, apply_script_fmt, release, latest.sha, *latest.tag ? latest.tag : "latest");
    install_run(L("正在更新界面", "Updating the screen"), script, done_noop);
}

static void on_apply(lv_event_t *e)
{
    dialog_close();
    start_apply();
}

static void show_prompt(void)
{
    char when[32], note[192];
    format_time(latest.build, when, sizeof when);
    snprintf(note, sizeof note,
             L("构建时间 %.31s · 提交 %.12s\n下载后界面会自动重启，当前程序会保留以便回滚。",
               "Built %.31s · commit %.12s\nThe screen restarts after the download; the current binary is kept for rollback."),
             when, latest.commit);
    dialog_open(L("发现新版本", "Update available"), note);
    ui_primary(dialog_button(L("下载并更新", "Download and update"), on_apply, NULL));
    dialog_cancel();
}

static void prompt_retry(lv_timer_t *t)
{
    if (!prompt_pending) {
        lv_timer_delete(t);
        prompt_timer = NULL;
        return;
    }
    if (!can_prompt())
        return;
    prompt_pending = false;
    lv_timer_delete(t);
    prompt_timer = NULL;
    show_prompt();
}

static void prompt_update(void)
{
    if (can_prompt()) {
        prompt_pending = false;
        show_prompt();
        return;
    }
    prompt_pending = true;
    if (!prompt_timer)
        prompt_timer = lv_timer_create(prompt_retry, 10000, NULL);
}

static void check_dialog_closed(void)
{
    check_step = NULL;
    check_interactive = false;
}

static void check_in_background(lv_event_t *e)
{
    dialog_close();
}

static void show_check_progress(void)
{
    dialog_open(L("检查更新", "Check for updates"), NULL);
    check_interactive = true;
    lv_obj_t *sheet = dialog_sheet();
    check_step = ui_text(sheet, 13, false, theme.text);
    lv_obj_set_width(check_step, LV_PCT(100));
    lv_label_set_long_mode(check_step, LV_LABEL_LONG_MODE_WRAP);
    ui_set_text(check_step, L("正在获取最新版本…", "Finding the latest version…"));
    dialog_button(L("后台检查", "Continue in background"), check_in_background, NULL);
    dialog_on_close(check_dialog_closed);
}

static void refresh_check_progress(void)
{
    if (!check_step)
        return;
    char path[200], phase[16], host[80];
    snprintf(path, sizeof path, "%.159s.progress", check_state);
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    int fields = fscanf(f, "%15s %79s", phase, host);
    fclose(f);
    if (fields != 2)
        return;
    char text[192];
    snprintf(text, sizeof text, "%s\n%s",
             !strcmp(phase, "manifest") ? L("正在读取更新信息…", "Reading update information…")
                                        : L("正在获取最新版本…", "Finding the latest version…"), host);
    ui_set_text(check_step, text);
}

static void remove_check_files(void)
{
    if (!*check_state)
        return;
    char path[200];
    unlink(check_state);
    snprintf(path, sizeof path, "%.159s.meta", check_state);
    unlink(path);
    snprintf(path, sizeof path, "%.159s.atom", check_state);
    unlink(path);
    snprintf(path, sizeof path, "%.159s.part", check_state);
    unlink(path);
    snprintf(path, sizeof path, "%.159s.progress", check_state);
    unlink(path);
    check_state[0] = 0;
}

static void finish_check(lv_timer_t *t, struct result *r)
{
    lv_timer_delete(t);
    reap_check();
    remove_check_files();
    if (r->status == CHECK_NEWER && (!valid_sha(r->sha) || !valid_tag(r->tag))) {
        r->status = CHECK_ERROR;
        copy_field(r->error, sizeof r->error, "invalid");
    }
    if (r->status == CHECK_NEWER)
        lowercase(r->sha);
    bool interactive = check_interactive;
    if (check_step)
        dialog_close();
    check_interactive = false;
    if (r->status == CHECK_NEWER) {
        if (interactive || update_auto_enabled()) {
            latest = *r;
            prompt_update();
        }
        auto_schedule(AUTO_INTERVAL);
    } else if (r->status == CHECK_CURRENT) {
        if (interactive) {
            dialog_open(L("检查更新", "Check for updates"), L("已是最新版本", "Up to date"));
            dialog_cancel();
        }
        auto_schedule(AUTO_INTERVAL);
    } else {
        if (interactive) {
            dialog_open(L("检查更新失败", "Update check failed"), check_error(r->error));
            dialog_cancel();
        }
        auto_schedule(AUTO_RETRY);
    }
}

static void check_poll(lv_timer_t *t)
{
    refresh_check_progress();
    struct result r = {0};
    if (read_result(check_state, &r)) {
        finish_check(t, &r);
        return;
    }
    int status;
    pid_t exited = waitpid(check_pid, &status, WNOHANG);
    if (exited == check_pid || (exited < 0 && errno != EINTR)) {
        check_pid = -1;
        if (!read_result(check_state, &r)) {
            r.status = CHECK_ERROR;
            copy_field(r.error, sizeof r.error, "network");
        }
        finish_check(t, &r);
        return;
    }
    if (lv_tick_get() - check_started > CHECK_TIMEOUT) {
        kill(-check_pid, SIGKILL);
        reap_check();
        r.status = CHECK_ERROR;
        copy_field(r.error, sizeof r.error, "timeout");
        finish_check(t, &r);
    }
}

static bool start_check(bool interactive)
{
    if (check_pid >= 0) {
        if (interactive)
            show_check_progress();
        return false;
    }
    if (install_running()) {
        if (interactive)
            toast("%s", L("正在更新，请稍候", "An update is running"));
        return false;
    }
    check_interactive = interactive;
    snprintf(check_state, sizeof check_state, STATE_FILE ".%d.%u", (int)getpid(), ++check_sequence);
    char current[24], state[160];
    snprintf(current, sizeof current, "%lld", (long long)BUILD_TIMESTAMP);
    snprintf(state, sizeof state, "%s", check_state);
    pid_t pid = fork();
    if (pid < 0) {
        check_state[0] = 0;
        if (interactive)
            toast("%s", L("无法启动更新检查", "Cannot start the update check"));
        return false;
    }
    if (pid == 0) {
        setpgid(0, 0);
        execl("/bin/sh", "sh", "-c", check_script, "e5-update", current, state, (char *)NULL);
        _exit(127);
    }
    check_pid = pid;
    check_started = lv_tick_get();
    lv_timer_create(check_poll, 250, NULL);
    if (interactive)
        show_check_progress();
    return true;
}

static void auto_tick(lv_timer_t *t)
{
    if (!update_auto_enabled()) {
        lv_timer_pause(t);
        return;
    }
    if (check_pid >= 0 || install_running() || dialog_is_open()) {
        lv_timer_set_period(t, 300000);
        return;
    }
    if (start_check(false))
        lv_timer_set_period(t, AUTO_INTERVAL);
    else
        lv_timer_set_period(t, 300000);
}

void update_init(void)
{
    auto_timer = lv_timer_create(auto_tick, 60000, NULL);
    if (!update_auto_enabled())
        lv_timer_pause(auto_timer);
}

void update_check(bool interactive)
{
    start_check(interactive);
}

bool update_set_auto(bool on)
{
    if (!screen_option_set(AUTO_OPTION, on ? "1" : "0"))
        return false;
    if (!auto_timer)
        return true;
    if (on) {
        lv_timer_resume(auto_timer);
        auto_schedule(1000);
    } else {
        lv_timer_pause(auto_timer);
    }
    return true;
}

bool update_backup_exists(void)
{
    return access(BACKUP_FILE, F_OK) == 0;
}

void update_rollback(void)
{
    if (install_running()) {
        toast("%s", L("正在更新，请稍候", "An update is running"));
        return;
    }
    if (!update_backup_exists()) {
        toast("%s", L("没有可回滚的更新", "No update to roll back"));
        return;
    }
    install_run(L("正在回滚界面", "Rolling the screen back"), rollback_script, done_noop);
}

void update_version_text(char *time, size_t time_size, char *commit, size_t commit_size)
{
    format_time((long long)BUILD_TIMESTAMP, time, time_size);
    if (!BUILD_COMMIT[0] || !strcmp(BUILD_COMMIT, "unknown"))
        snprintf(commit, commit_size, "%s", L("未知", "Unknown"));
    else
        snprintf(commit, commit_size, "%.7s", BUILD_COMMIT);
}
