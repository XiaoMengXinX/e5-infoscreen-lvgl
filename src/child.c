#include "child.h"

#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include "app.h"

static int reap(struct child *c)
{
    int status = 0;
    close(c->fd);
    c->fd = -1;
    while (waitpid(c->pid, &status, 0) < 0 && errno == EINTR) {
    }
    c->pid = 0;
    return status;
}

static void take_line(struct child *c)
{
    c->line[c->len] = 0;
    c->len = 0;
    if (*c->line)
        c->on_line(c->line);
}

static void poll_output(lv_timer_t *t)
{
    struct child *c = lv_timer_get_user_data(t);
    /* A step can stay silent for a while; the screen stays up until the program ends. */
    idle_reset();
    char buf[1024];
    for (;;) {
        ssize_t n = read(c->fd, buf, sizeof buf);
        if (n > 0) {
            for (ssize_t i = 0; i < n; i++) {
                if (buf[i] == '\r' || buf[i] == '\n')
                    take_line(c);
                else if (c->len < sizeof c->line - 1)
                    c->line[c->len++] = buf[i];
            }
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && errno == EAGAIN)
            return;
        if (c->len)
            take_line(c);
        /* EOF, or EIO once the program has exited and closed the terminal. */
        lv_timer_delete(t);
        c->reader = NULL;
        c->on_exit(reap(c));
        return;
    }
}

bool child_start(struct child *c, char *const argv[], void (*on_line)(const char *line), void (*on_exit)(int status))
{
    int fd;
    pid_t pid = forkpty(&fd, NULL, NULL, NULL);
    if (pid < 0)
        return false;
    if (pid == 0) {
        execv(argv[0], argv);
        _exit(127);
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    *c = (struct child){.pid = pid, .fd = fd, .on_line = on_line, .on_exit = on_exit};
    c->reader = lv_timer_create(poll_output, 50, c);
    return true;
}

bool child_running(const struct child *c)
{
    return c->pid > 0;
}

void child_kill(struct child *c)
{
    if (c->pid <= 0)
        return;
    /* forkpty made the program a session leader, so its group is its own. */
    kill(-c->pid, SIGKILL);
    lv_timer_delete(c->reader);
    c->reader = NULL;
    reap(c);
}
