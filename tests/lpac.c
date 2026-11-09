#define access test_access
#include "../src/lpac.c"
#undef access

#include <assert.h>

static bool program_present, tls_present;
int test_access(const char *path, int mode)
{
    return (!strcmp(path, LPAC) ? program_present : tls_present) ? 0 : -1;
}

static bool completed, result_ok;
static size_t result_length;
void idle_reset(void) {}

static void result(bool ok, const cJSON *data, const char *error, const char *detail)
{
    completed = true;
    result_ok = ok;
    result_length = cJSON_IsString(data) ? strlen(data->valuestring) : 0;
}

static void receive(pid_t child, int fd)
{
    run.pid = child;
    run.out = fd;
    fcntl(fd, F_SETFL, O_NONBLOCK);
    run.count = 1;
    run.head = 0;
    run.q[0] = (struct job){.argv = calloc(2, sizeof(char *)), .done = result};
    run.q[0].argv[0] = (char *)LPAC;
    completed = false;
    for (int i = 0; !completed && i < 5000; i++) {
        output_poll();
        usleep(1000);
    }
    assert(completed);
}

static volatile sig_atomic_t interrupted;
static void cancel_signal(int sig) { interrupted = 1; }

int main(void)
{
    assert(!lpac_present());
    program_present = true;
    assert(!lpac_present());
    program_present = false;
    tls_present = true;
    assert(!lpac_present());
    program_present = true;
    assert(lpac_present());
    const char *replies[] = {"\r\nOK\r\n", "ERROR\r\n", "+CME ERROR: 10\r\n", "", "NOT OK\r\n"};
    for (unsigned i = 0; i < sizeof replies / sizeof *replies; i++) {
        int fds[2];
        assert(pipe(fds) == 0);
        run.master = fds[1];
        on_plain(replies[i]);
        close(fds[1]);
        char answer[16] = {0};
        assert(read(fds[0], answer, sizeof answer - 1) > 0);
        assert(!strcmp(answer, i == 0 ? "OK\r\n" : "ERROR\r\n"));
        close(fds[0]);
    }
    run.master = -1;

    int fds[2];
    assert(pipe(fds) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        close(fds[0]);
        FILE *out = fdopen(fds[1], "w");
        fputs("{\"type\":\"lpa\",\"payload\":{\"code\":0,\"data\":\"", out);
        for (int i = 0; i < 50000; i++)
            fputc('x', out);
        fputs("\"}}", out);
        fclose(out);
        _exit(0);
    }
    close(fds[1]);
    receive(child, fds[0]);
    assert(result_ok && result_length == 50000);

    int ready[2];
    assert(pipe(fds) == 0 && pipe(ready) == 0);
    child = fork();
    assert(child >= 0);
    if (!child) {
        close(fds[0]);
        close(ready[0]);
        signal(SIGINT, cancel_signal);
        assert(write(ready[1], "r", 1) == 1);
        close(ready[1]);
        while (!interrupted)
            usleep(1000);
        const char text[] = "{\"type\":\"lpa\",\"payload\":{\"code\":-1,\"message\":\"cancelled\"}}\n";
        assert(write(fds[1], text, sizeof text - 1) == sizeof text - 1);
        close(fds[1]);
        _exit(0);
    }
    close(fds[1]);
    close(ready[1]);
    char byte;
    assert(read(ready[0], &byte, 1) == 1);
    close(ready[0]);
    run.pid = child;
    lpac_cancel();
    assert(run.cancelling);
    receive(child, fds[0]);
    assert(!result_ok && !run.cancelling);

    assert(pipe(fds) == 0);
    run.in = fds[1];
    run.preview = true;
    assert(lpac_download_confirm(true) && !run.preview);
    char answer[2];
    assert(read(fds[0], answer, 2) == 2 && !memcmp(answer, "y\n", 2));
    close(fds[0]);
    close(fds[1]);
    run.in = -1;

    run.refresh_ok = true;
    on_off("ERROR\r\n");
    on_radio("OK\r\n");
    run.refresh_done = result;
    completed = false;
    on_refresh("OK\r\n");
    assert(completed && !result_ok);
    bridge_clear();

    char activation[4096];
    memset(activation, 'a', sizeof activation - 1);
    activation[sizeof activation - 1] = 0;
    ending = true;
    lpac_call(1, (char *const[]){"profile", "download", "-a", activation, NULL}, NULL, result);
    assert(!strcmp(run.q[run.head].argv[4], activation));
    free_job(&run.q[run.head]);
    run.count = 0;
    ending = false;
    puts("LPAC: replies, 50KB output, graceful cancellation, preview, refresh failures and long arguments passed");
}
